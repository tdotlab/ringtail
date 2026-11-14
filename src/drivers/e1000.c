// Intel 8254x gigabit ethernet, the "e1000" that QEMU, Bochs,
// VirtualBox and VMware all emulate. Legacy descriptors, one receive
// and one transmit ring, no offloads. The registers are memory mapped
// (BAR 0) into the kernel's device window; descriptors and buffers are
// frames, whose physical and virtual addresses are the same, so they
// can be handed to the card as they are.
#include "kernel.h"
#include "arch.h"
#include "interrupt.h"
#include "drivers.h"
#include "mm.h"
#include "net.h"

#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_ICR    0x00C0
#define REG_IMS    0x00D0
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_RDTR   0x2820
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL    0x5400
#define REG_RAH    0x5404

#define CTRL_LRST    (1u << 3)
#define CTRL_ASDE    (1u << 5)
#define CTRL_SLU     (1u << 6)
#define CTRL_ILOS    (1u << 7)
#define CTRL_RST     (1u << 26)
#define CTRL_VME     (1u << 30)
#define CTRL_PHY_RST (1u << 31)
#define STATUS_LU    (1u << 1)
#define RCTL_EN      (1u << 1)
#define RCTL_BAM     (1u << 15)  // accept broadcasts (ARP needs them)
#define RCTL_SECRC   (1u << 26)  // strip the CRC
#define TCTL_EN      (1u << 1)
#define TCTL_PSP     (1u << 3)   // pad short frames
#define TCTL_CT      (0x0Fu << 4)
#define TCTL_COLD    (0x40u << 12)
#define INT_LSC      (1u << 2)
#define INT_RXSEQ    (1u << 3)
#define INT_RXDMT0   (1u << 4)
#define INT_RXO      (1u << 6)
#define INT_RXT0     (1u << 7)
#define RAH_AV       (1u << 31)

#define RX_STATUS_DD  0x01
#define RX_STATUS_EOP 0x02
#define TX_CMD_EOP    0x01
#define TX_CMD_IFCS   0x02
#define TX_CMD_RS     0x08
#define TX_STATUS_DD  0x01

#define RX_N 32
#define TX_N 8
#define BUF_SIZE 2048

typedef struct {
    uint64_t addr;
    uint16_t len, csum;
    uint8_t status, errors;
    uint16_t special;
} __attribute__((packed)) rx_desc_t;

typedef struct {
    uint64_t addr;
    uint16_t len;
    uint8_t cso, cmd, status, css;
    uint16_t special;
} __attribute__((packed)) tx_desc_t;

static volatile uint32_t* regs;
static volatile rx_desc_t* rx;
static volatile tx_desc_t* tx;
static uintptr_t rx_ring_phys, tx_ring_phys;  // where the card finds the rings
static uint8_t* rx_buf[RX_N];
static uint8_t* tx_buf[TX_N];
static uint32_t rx_cur, tx_cur;
static netdev_t card;

static uint32_t rd(uint32_t reg) { return regs[reg / 4]; }
static void wr(uint32_t reg, uint32_t v) { regs[reg / 4] = v; }

static const struct { uint16_t id; const char* model; } models[] = {
    { 0x100E, "82540EM" },  // QEMU's default, Bochs, VirtualBox's PRO/1000 MT Desktop
    { 0x100F, "82545EM" },  // VMware, VirtualBox's MT Server, QEMU e1000-82545em
    { 0x100C, "82544GC" },  // QEMU e1000-82544gc
    { 0x1004, "82543GC" },  // VirtualBox's PRO/1000 T Server
};

static int e1000_link_up(netdev_t* d) { (void)d; return (rd(REG_STATUS) & STATUS_LU) != 0; }

static int e1000_rx_ready(netdev_t* d) { (void)d; return (rx[rx_cur].status & RX_STATUS_DD) != 0; }

static int e1000_recv(netdev_t* d, void* buf, uint32_t size) {
    for (;;) {
        volatile rx_desc_t* r = &rx[rx_cur];
        if (!(r->status & RX_STATUS_DD)) return 0;
        uint32_t len = r->len;
        // A frame too big for one buffer arrives in pieces; there is no
        // use for those, since nothing larger than the MTU is accepted.
        int ok = (r->status & RX_STATUS_EOP) && !r->errors && len >= ETH_HLEN && len <= size;
        if (ok) memcpy(buf, rx_buf[rx_cur], len);
        else d->rx_errors++;
        r->status = 0;
        uint32_t done = rx_cur;
        rx_cur = (rx_cur + 1) % RX_N;
        __asm__ volatile("" ::: "memory");
        wr(REG_RDT, done);  // hand the buffer back
        if (ok) return (int)len;
    }
}

static int e1000_send(netdev_t* d, const void* frame, uint32_t len) {
    (void)d;
    if (len > BUF_SIZE) return -EINVAL;
    uint32_t f = irq_save();
    volatile tx_desc_t* t = &tx[tx_cur];
    // The ring is full until the card finishes the oldest frame; it
    // sends one in about 12 microseconds at gigabit speed, so a wait
    // of a few milliseconds means it is stuck.
    for (int spins = 0; !(t->status & TX_STATUS_DD); spins++) {
        if (spins == 5000) { irq_restore(f); return -EBUSY; }
        io_wait();
    }
    memcpy(tx_buf[tx_cur], frame, len);
    t->len = len;
    t->cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    t->status = 0;
    tx_cur = (tx_cur + 1) % TX_N;
    __asm__ volatile("" ::: "memory");
    wr(REG_TDT, tx_cur);
    irq_restore(f);
    return 0;
}

static void e1000_irq(void) {
    uint32_t icr = rd(REG_ICR);  // reading it clears it and lowers the line
    if (icr) card.interrupts++;
    if (icr & (INT_RXT0 | INT_RXO | INT_RXDMT0 | INT_RXSEQ)) net_rx_notify();
}

static uint16_t eeprom_word(uint8_t addr) {
    wr(REG_EERD, (uint32_t)addr << 8 | 1);
    for (int i = 0; i < 10000; i++) {
        uint32_t v = rd(REG_EERD);
        if (v & (1u << 4)) return v >> 16;
        io_wait();
    }
    return 0;
}

static void read_mac(uint8_t mac[6]) {
    uint32_t lo = rd(REG_RAL), hi = rd(REG_RAH);
    if ((hi & RAH_AV) && (lo || (hi & 0xFFFF))) {
        for (int i = 0; i < 4; i++) mac[i] = lo >> (8 * i);
        mac[4] = hi; mac[5] = hi >> 8;
        return;
    }
    for (int i = 0; i < 3; i++) {
        uint16_t w = eeprom_word(i);
        mac[2 * i] = w; mac[2 * i + 1] = w >> 8;
    }
}

// Frames for the rings and buffers. Never freed: the card owns them
// for as long as the machine runs.
static int alloc_rings(void) {
    uintptr_t ring = frame_alloc();
    if (!ring) return -ENOMEM;
    rx = phys_to_virt(ring);
    tx = phys_to_virt(ring + RX_N * sizeof(rx_desc_t));
    for (int i = 0; i < RX_N; i += PAGE_SIZE / BUF_SIZE) {
        uintptr_t f = frame_alloc();
        if (!f) return -ENOMEM;
        for (uint32_t j = 0; j < PAGE_SIZE / BUF_SIZE; j++) rx_buf[i + j] = phys_to_virt(f + j * BUF_SIZE);
    }
    for (int i = 0; i < TX_N; i += PAGE_SIZE / BUF_SIZE) {
        uintptr_t f = frame_alloc();
        if (!f) return -ENOMEM;
        for (uint32_t j = 0; j < PAGE_SIZE / BUF_SIZE; j++) tx_buf[i + j] = phys_to_virt(f + j * BUF_SIZE);
    }
    // The card reads and writes these by physical address.
    for (int i = 0; i < RX_N; i++) { rx[i].addr = virt_to_phys(rx_buf[i]); rx[i].status = 0; }
    for (int i = 0; i < TX_N; i++) { tx[i].addr = virt_to_phys(tx_buf[i]); tx[i].status = TX_STATUS_DD; }
    rx_ring_phys = ring;
    tx_ring_phys = ring + RX_N * sizeof(rx_desc_t);
    return 0;
}

static netdev_t* start(const pci_dev_t* p, const char* model) {
    uint32_t bar = p->bar[0];
    if (bar & 1) return 0;                                   // I/O space: not this register layout
    if (((bar >> 1) & 3) == 2 && p->bar[1]) return 0;        // above 4 GB
    regs = mmio_map(bar & ~0xFu, 0x20000);
    if (!regs) return 0;
    pci_enable(p);

    wr(REG_IMC, 0xFFFFFFFFu);
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
    for (int i = 0; i < 100000 && (rd(REG_CTRL) & CTRL_RST); i++) io_wait();
    wr(REG_IMC, 0xFFFFFFFFu);
    (void)rd(REG_ICR);
    if (alloc_rings()) return 0;

    wr(REG_CTRL, (rd(REG_CTRL) | CTRL_SLU | CTRL_ASDE) & ~(CTRL_LRST | CTRL_PHY_RST | CTRL_ILOS | CTRL_VME));
    read_mac(card.mac);
    for (int i = 0; i < 128; i++) wr(REG_MTA + 4 * i, 0);

    wr(REG_RDBAL, (uint32_t)rx_ring_phys);
    wr(REG_RDBAH, (uint32_t)((uint64_t)rx_ring_phys >> 32));
    wr(REG_RDLEN, RX_N * sizeof(rx_desc_t));
    wr(REG_RDH, 0);
    wr(REG_RDT, RX_N - 1);
    wr(REG_RDTR, 0);
    wr(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);  // 2 KB buffers

    wr(REG_TDBAL, (uint32_t)tx_ring_phys);
    wr(REG_TDBAH, (uint32_t)((uint64_t)tx_ring_phys >> 32));
    wr(REG_TDLEN, TX_N * sizeof(tx_desc_t));
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    wr(REG_TIPG, 10 | 8 << 10 | 6 << 20);
    wr(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);

    card.name = "e1000";
    card.model = model;
    snprintf(card.where, sizeof card.where, "%02x:%02x.%u", p->bus, p->dev, p->fn);
    card.vendor = p->vendor;
    card.device = p->device;
    card.send = e1000_send;
    card.recv = e1000_recv;
    card.rx_ready = e1000_rx_ready;
    card.link_up = e1000_link_up;
    card.irq = -1;
    if (p->irq > 0 && p->irq < 16 && p->irq != 2) {
        card.irq = p->irq;
        irq_register(p->irq, e1000_irq);
        wr(REG_IMS, INT_RXT0 | INT_RXO | INT_RXDMT0 | INT_RXSEQ | INT_LSC);
    }
    (void)rd(REG_ICR);
    return &card;
}

netdev_t* e1000_probe(void) {
    for (int i = 0; i < pci_count(); i++) {
        const pci_dev_t* p = pci_get(i);
        if (p->vendor != 0x8086) continue;
        for (uint32_t m = 0; m < ARRAY_SIZE(models); m++)
            if (p->device == models[m].id) return start(p, models[m].model);
    }
    return 0;
}
