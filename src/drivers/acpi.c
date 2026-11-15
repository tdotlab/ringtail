// ACPI tables, read for one thing: the MADT, which says where the
// interrupt controllers are and how the ISA interrupts are wired to
// them. There is no AML interpreter, so nothing that lives in the
// DSDT (PCI interrupt routing, sleep states, the power button) is
// known here.
#include "kernel.h"
#include "lib.h"
#include "mm.h"
#include "drivers.h"

typedef struct __attribute__((packed)) {
    char sig[8];          // "RSD PTR "
    uint8_t checksum;     // over the first 20 bytes
    char oem[6];
    uint8_t revision;     // 0 for ACPI 1.0, 2 from 2.0 on
    uint32_t rsdt;
    uint32_t length;      // from here on, ACPI 2.0 and later
    uint64_t xsdt;
    uint8_t xchecksum;    // over all length bytes
    uint8_t reserved[3];
} rsdp_t;

typedef struct __attribute__((packed)) {
    char sig[4];
    uint32_t length;      // header included
    uint8_t revision;
    uint8_t checksum;     // over all length bytes
    char oem[6];
    char oem_table[8];
    uint32_t oem_revision;
    uint32_t creator;
    uint32_t creator_revision;
} sdt_t;

static madt_t madt;
static int have_madt;

static int sum_ok(const void* p, uint32_t len) {
    const uint8_t* b = p;
    uint8_t s = 0;
    while (len--) s += *b++;
    return s == 0;
}

// The tables sit in memory the firmware reserved, which the direct map
// need not cover, so they are reached through the device window. That
// window only grows (nothing is ever unmapped, so a pointer stays good),
// so the last mapping is reused while what is asked for falls inside
// it: firmware puts the tables next to each other, and most of them
// are found in a page or two.
static uintptr_t mapped_phys, mapped_len;
static const uint8_t* mapped_va;

static const void* map(uint64_t phys, uint32_t len) {
    if (phys >= mapped_phys && phys + len <= mapped_phys + mapped_len)
        return mapped_va + (phys - mapped_phys);
    uintptr_t start = ALIGN_DOWN((uintptr_t)phys, (uintptr_t)PAGE_SIZE);
    uintptr_t size = ALIGN_UP((uintptr_t)phys + len, (uintptr_t)PAGE_SIZE) - start;
    if (size < 2 * PAGE_SIZE) size = 2 * PAGE_SIZE;  // the next table is likely there too
    const uint8_t* va = mmio_map(start, (uint32_t)size);
    if (!va) return 0;
    mapped_phys = start;
    mapped_len = size;
    mapped_va = va;
    return va + (phys - start);
}

// A table whose header and checksum check out, or 0.
static const sdt_t* map_table(uint64_t phys) {
    const sdt_t* h = map(phys, sizeof(sdt_t));
    if (!h) return 0;
    uint32_t len = h->length;
    if (len < sizeof(sdt_t) || len > 1024 * 1024) return 0;
    h = map(phys, len);
    return h && sum_ok(h, len) ? h : 0;
}

// Below 1 MB the direct map reaches, all but page 0.
static const rsdp_t* scan(uintptr_t phys, uint32_t len) {
    for (uint32_t off = 0; off + sizeof(rsdp_t) <= len; off += 16) {
        const rsdp_t* r = phys_to_virt(phys + off);
        if (memcmp(r->sig, "RSD PTR ", 8) || !sum_ok(r, 20)) continue;
        if (r->revision >= 2 && (r->length < sizeof(rsdp_t) || r->length > len - off || !sum_ok(r, r->length)))
            continue;
        return r;
    }
    return 0;
}

// The RSDP is on a 16-byte boundary in the first KB of the EBDA, whose
// segment the BIOS data area holds at 0x40E, or in the BIOS ROM at
// 0xE0000-0xFFFFF. (A UEFI loader would pass its address instead; this
// kernel is booted by a BIOS loader.) The BIOS data area is in page 0,
// which the direct map leaves out to catch null pointers.
static const rsdp_t* find_rsdp(void) {
    const uint16_t* ebda_seg = mmio_map(0x40E, 2);
    if (ebda_seg) {
        uintptr_t ebda = (uintptr_t)*ebda_seg << 4;
        if (ebda >= 0x80000 && ebda < 0xA0000) {
            const rsdp_t* r = scan(ebda, 1024);
            if (r) return r;
        }
    }
    return scan(0xE0000, 0x20000);
}

static void parse_madt(const sdt_t* t) {
    const uint8_t* p = (const uint8_t*)t;
    const uint8_t* end = p + t->length;
    if (t->length < sizeof(sdt_t) + 8) return;
    madt.lapic_phys = *(const uint32_t*)(p + sizeof(sdt_t));
    madt.flags = *(const uint32_t*)(p + sizeof(sdt_t) + 4);
    for (p += sizeof(sdt_t) + 8; p + 2 <= end && p[1] >= 2 && p + p[1] <= end; p += p[1]) {
        uint8_t type = p[0], len = p[1];
        if (type == 0 && len >= 8) {  // a processor's local APIC
            // Bit 0: enabled. A disabled one marked online capable
            // (bit 1) could be added later; it is not here now.
            if ((*(const uint32_t*)(p + 4) & 1) && madt.cpus < MADT_MAX_CPUS) {
                madt.cpu[madt.cpus].acpi_id = p[2];
                madt.cpu[madt.cpus].apic_id = p[3];
                madt.cpus++;
            }
        } else if (type == 1 && len >= 12 && madt.ioapics < MADT_MAX_IOAPICS) {
            madt.ioapic[madt.ioapics].id = p[2];
            madt.ioapic[madt.ioapics].phys = *(const uint32_t*)(p + 4);
            madt.ioapic[madt.ioapics].gsi_base = *(const uint32_t*)(p + 8);
            madt.ioapics++;
        } else if (type == 2 && len >= 10 && madt.overrides < MADT_MAX_OVERRIDES) {
            if (p[2] != 0) continue;  // only bus 0, ISA, is defined
            madt.override[madt.overrides].irq = p[3];
            madt.override[madt.overrides].gsi = *(const uint32_t*)(p + 4);
            madt.override[madt.overrides].flags = *(const uint16_t*)(p + 8);
            madt.overrides++;
        } else if (type == 4 && len >= 6 && madt.nmis < MADT_MAX_NMIS) {
            madt.nmi[madt.nmis].acpi_id = p[2];
            madt.nmi[madt.nmis].flags = *(const uint16_t*)(p + 3);
            madt.nmi[madt.nmis].lint = p[5];
            madt.nmis++;
        } else if (type == 5 && len >= 12) {
            madt.lapic_phys = *(const uint64_t*)(p + 4);
        } else if (type == 9 && len >= 16) {
            // A processor whose APIC ID needs x2APIC mode (more than
            // 255): counted, but not one this kernel could start.
            if (*(const uint32_t*)(p + 8) & 1) madt.x2apic_cpus++;
        }
    }
    have_madt = 1;
}

int acpi_init(void) {
    const rsdp_t* r = find_rsdp();
    if (!r) return -ENOENT;
    uint8_t revision = r->revision;
    uint64_t xsdt = revision >= 2 ? r->xsdt : 0;
    uint32_t rsdt = r->rsdt;
    madt.revision = revision;
    memcpy(madt.oem, r->oem, 6);
    madt.oem[6] = 0;

    // The XSDT holds 64-bit addresses, the RSDT 32-bit ones; take the
    // XSDT when there is one.
    uint64_t root = xsdt ? xsdt : rsdt;
    uint32_t width = xsdt ? 8 : 4;
    const sdt_t* t = map_table(root);
    if (!t && xsdt) {
        root = rsdt;
        width = 4;
        t = map_table(root);
    }
    if (!t) return -EIO;
    uint32_t n = (t->length - sizeof(sdt_t)) / width;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* e = (const uint8_t*)t + sizeof(sdt_t) + i * width;
        uint64_t phys = width == 8 ? *(const uint64_t*)e : *(const uint32_t*)e;
        const sdt_t* s = map_table(phys);
        if (s && !memcmp(s->sig, "APIC", 4)) {
            parse_madt(s);
            break;
        }
    }
    return have_madt ? 0 : -ENOENT;
}

const madt_t* acpi_madt(void) { return have_madt ? &madt : 0; }
