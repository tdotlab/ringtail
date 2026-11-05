// PCI bus scan through configuration mechanism #1 (ports 0xCF8 and
// 0xCFC), which every PC with PCI has. The scan runs once at boot and
// keeps what drivers need: ids, class, interrupt line and BARs.
#include "kernel.h"
#include "arch.h"
#include "drivers.h"

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC
#define PCI_MAX_DEVS 64

static pci_dev_t devs[PCI_MAX_DEVS];
static int ndevs;

static uint32_t cfg_addr(int bus, int dev, int fn, uint8_t off) {
    return 0x80000000u | (uint32_t)bus << 16 | (uint32_t)dev << 11 | (uint32_t)fn << 8 | (off & 0xFC);
}

static uint32_t cfg_read(int bus, int dev, int fn, uint8_t off) {
    uint32_t f = irq_save();
    outl(PCI_ADDR, cfg_addr(bus, dev, fn, off));
    uint32_t v = inl(PCI_DATA);
    irq_restore(f);
    return v;
}

static void cfg_write(int bus, int dev, int fn, uint8_t off, uint32_t v) {
    uint32_t f = irq_save();
    outl(PCI_ADDR, cfg_addr(bus, dev, fn, off));
    outl(PCI_DATA, v);
    irq_restore(f);
}

uint32_t pci_read32(const pci_dev_t* d, uint8_t off) { return cfg_read(d->bus, d->dev, d->fn, off); }
void pci_write32(const pci_dev_t* d, uint8_t off, uint32_t v) { cfg_write(d->bus, d->dev, d->fn, off, v); }

static void add(int bus, int dev, int fn, uint32_t id) {
    if (ndevs == PCI_MAX_DEVS) return;
    pci_dev_t* d = &devs[ndevs++];
    d->bus = bus; d->dev = dev; d->fn = fn;
    d->vendor = id & 0xFFFF;
    d->device = id >> 16;
    uint32_t cls = cfg_read(bus, dev, fn, 0x08);
    d->class_code = cls >> 24;
    d->subclass = cls >> 16;
    d->prog_if = cls >> 8;
    uint8_t type = (cfg_read(bus, dev, fn, 0x0C) >> 16) & 0x7F;
    d->irq = 0xFF;
    for (int i = 0; i < 6; i++) d->bar[i] = 0;
    if (type == 0) {  // an ordinary device, not a bridge
        for (int i = 0; i < 6; i++) d->bar[i] = cfg_read(bus, dev, fn, 0x10 + 4 * i);
        d->irq = cfg_read(bus, dev, fn, 0x3C) & 0xFF;
    }
}

void pci_init(void) {
    // A machine without PCI reads back something other than what was
    // written to the address port.
    outl(PCI_ADDR, 0x80000000u);
    if (inl(PCI_ADDR) != 0x80000000u) return;
    for (int bus = 0; bus < 256; bus++)
        for (int dev = 0; dev < 32; dev++) {
            uint32_t id = cfg_read(bus, dev, 0, 0);
            if ((id & 0xFFFF) == 0xFFFF) continue;
            int fns = (cfg_read(bus, dev, 0, 0x0C) >> 16) & 0x80 ? 8 : 1;
            for (int fn = 0; fn < fns; fn++) {
                if (fn) id = cfg_read(bus, dev, fn, 0);
                if ((id & 0xFFFF) != 0xFFFF) add(bus, dev, fn, id);
            }
        }
}

int pci_count(void) { return ndevs; }
const pci_dev_t* pci_get(int i) { return i >= 0 && i < ndevs ? &devs[i] : 0; }

void pci_enable(const pci_dev_t* d) {
    // Memory space, I/O space and bus mastering (so it can DMA);
    // keep its interrupt enabled.
    uint32_t cmd = pci_read32(d, 0x04);
    cmd = (cmd & 0xFFFF & ~(1u << 10)) | 0x7;
    pci_write32(d, 0x04, cmd);
}

static const char* class_name(const pci_dev_t* d) {
    switch (d->class_code) {
    case 0x01: return d->subclass == 0x01 ? "IDE controller" : d->subclass == 0x06 ? "SATA controller"
                    : d->subclass == 0x08 ? "NVMe controller" : "storage";
    case 0x02: return d->subclass == 0x00 ? "ethernet" : d->subclass == 0x80 ? "network" : "network";
    case 0x03: return "display";
    case 0x04: return "multimedia";
    case 0x05: return "memory";
    case 0x06: return d->subclass == 0x00 ? "host bridge" : d->subclass == 0x01 ? "ISA bridge"
                    : d->subclass == 0x04 ? "PCI bridge" : "bridge";
    case 0x07: return "communication";
    case 0x08: return "system";
    case 0x0C: return d->subclass == 0x03 ? "USB controller" : d->subclass == 0x05 ? "SMBus" : "serial bus";
    case 0x0D: return "wireless";
    }
    return "other";
}

static const char* vendor_name(uint16_t v) {
    switch (v) {
    case 0x8086: return "Intel";
    case 0x10EC: return "Realtek";
    case 0x14E4: return "Broadcom";
    case 0x1022: return "AMD";
    case 0x1002: return "AMD/ATI";
    case 0x10DE: return "NVIDIA";
    case 0x1234: return "QEMU/Bochs";
    case 0x1AF4: return "virtio";
    case 0x15AD: return "VMware";
    case 0x80EE: return "VirtualBox";
    case 0x1969: return "Qualcomm Atheros";
    case 0x168C: return "Qualcomm Atheros";
    case 0x11AB: return "Marvell";
    case 0x1B21: return "ASMedia";
    }
    return "";
}

const char* pci_describe(const pci_dev_t* d, char* buf, uint32_t size) {
    const char* v = vendor_name(d->vendor);
    snprintf(buf, size, "%s%s%s", v, *v ? " " : "", class_name(d));
    return buf;
}

void pci_format(sbuf_t* sb) {
    if (!ndevs) { sbprintf(sb, "No PCI devices.\n"); return; }
    char what[48];
    for (int i = 0; i < ndevs; i++) {
        const pci_dev_t* d = &devs[i];
        pci_describe(d, what, sizeof what);
        sbprintf(sb, "%02x:%02x.%u  %04x:%04x  ", d->bus, d->dev, d->fn, d->vendor, d->device);
        if (d->irq != 0xFF && d->irq != 0) sbprintf(sb, "%-28s irq %u\n", what, d->irq);
        else sbprintf(sb, "%s\n", what);  // no padding to trail off the line
    }
}
