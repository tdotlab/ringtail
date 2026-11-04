// PS/2 mouse on the keyboard controller's auxiliary port, IRQ 12.
// Standard 3-byte packets: buttons and sign bits, then X and Y
// movement. The position is kept in graphics coordinates (320x200).
#include "kernel.h"
#include "arch.h"
#include "interrupt.h"
#include "drivers.h"
#include "tty.h"
#include "task.h"
#include "ringtail/abi.h"

#define KBC_STATUS 0x64
#define KBC_CMD    0x64
#define KBC_DATA   0x60
#define KBC_TIMEOUT 100000

static int found;
static volatile int mx = GFX_WIDTH / 2, my = GFX_HEIGHT / 2, mbuttons;
static volatile uint32_t packets;
static uint8_t pkt[3];
static int pkt_idx;
static uint32_t last_byte_tick;

static int wait_input_empty(void) {
    for (int i = 0; i < KBC_TIMEOUT; i++)
        if (!(inb(KBC_STATUS) & 0x02)) return 0;
    return -1;
}

static int read_byte(uint8_t* out, int aux_only) {
    for (int i = 0; i < KBC_TIMEOUT; i++) {
        uint8_t st = inb(KBC_STATUS);
        if (!(st & 0x01)) continue;
        uint8_t b = inb(KBC_DATA);
        if (aux_only && !(st & 0x20)) continue;  // a keyboard byte: drop it
        *out = b;
        return 0;
    }
    return -1;
}

static int kbc_cmd(uint8_t cmd) {
    if (wait_input_empty()) return -1;
    outb(KBC_CMD, cmd);
    return 0;
}

static int mouse_write(uint8_t b) {
    if (kbc_cmd(0xD4) || wait_input_empty()) return -1;
    outb(KBC_DATA, b);
    uint8_t ack;
    return (read_byte(&ack, 1) == 0 && ack == 0xFA) ? 0 : -1;
}

static void drain(void) {
    for (int i = 0; i < 100 && (inb(KBC_STATUS) & 0x01); i++) (void)inb(KBC_DATA);
}

static void mouse_irq(void) {
    uint8_t st = inb(KBC_STATUS);
    if (!(st & 0x01)) return;
    uint8_t b = inb(KBC_DATA);
    if (!(st & 0x20)) return;
    // A lost byte would leave every later packet misaligned. Bytes of
    // one packet arrive within a millisecond of each other, so a gap
    // of more than a couple of ticks means a new packet is starting.
    uint32_t now = timer_ticks();
    if (pkt_idx && now - last_byte_tick > 2) pkt_idx = 0;
    last_byte_tick = now;
    // Bit 3 of the first byte is always set: resynchronise on it.
    if (pkt_idx == 0 && !(b & 0x08)) return;
    pkt[pkt_idx++] = b;
    if (pkt_idx < 3) return;
    pkt_idx = 0;
    packets++;
    mbuttons = pkt[0] & 0x07;
    if (!(pkt[0] & 0x40)) {  // skip overflowed movement
        int dx = pkt[1] - ((pkt[0] & 0x10) ? 256 : 0);
        mx = MAX(0, MIN(GFX_WIDTH - 1, mx + dx));
    }
    if (!(pkt[0] & 0x80)) {
        int dy = pkt[2] - ((pkt[0] & 0x20) ? 256 : 0);
        my = MAX(0, MIN(GFX_HEIGHT - 1, my - dy));  // PS/2 Y grows upwards
    }
    task_wakeup(&input_chan);
}

int mouse_init(void) {
    uint8_t cfg, b;
    drain();
    if (kbc_cmd(0xA8)) goto fail;                 // enable aux port
    if (kbc_cmd(0x20) || read_byte(&cfg, 0)) goto fail;
    cfg = (uint8_t)((cfg | 0x02) & ~0x20);        // aux IRQ on, aux clock on
    if (kbc_cmd(0x60) || wait_input_empty()) goto fail;
    outb(KBC_DATA, cfg);
    if (mouse_write(0xFF)) goto fail;             // reset
    if (read_byte(&b, 1) || b != 0xAA) goto fail; // self-test passed
    if (read_byte(&b, 1) || b != 0x00) goto fail; // device id: plain mouse
    if (mouse_write(0xF6) || mouse_write(0xF4)) goto fail;  // defaults, streaming on
    found = 1;
    irq_register(IRQ_MOUSE, mouse_irq);
    return 0;
fail:
    drain();
    return -1;
}

int mouse_found(void) { return found; }

void mouse_get(int* x, int* y, int* buttons) {
    uint32_t f = irq_save();
    *x = mx;
    *y = my;
    *buttons = mbuttons;
    irq_restore(f);
}

uint32_t mouse_packets(void) { return packets; }
