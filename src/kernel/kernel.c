// Kernel entry: bring the machine up in dependency order, start
// init (which starts the shell), then become the idle task.
#include "kernel.h"
#include "arch.h"
#include "console.h"
#include "interrupt.h"
#include "drivers.h"
#include "task.h"
#include "mm.h"
#include "fs.h"
#include "net.h"
#include "random.h"

#define MULTIBOOT_MAGIC 0x2BADB002u
#define MB_FLAG_CMDLINE (1u << 2)
#define MB_FLAG_MMAP    (1u << 6)

boot_info_t boot_info;

// Copy what we need out of the multiboot structures, which live in
// memory the frame allocator is about to hand out.
// The bootloader hands over 32-bit physical addresses.
static void read_boot_info(uint32_t mbi) {
    const uint32_t* m = phys_to_virt(mbi);
    uint32_t flags = m[0];
    if (flags & MB_FLAG_CMDLINE)
        strlcpy(boot_info.cmdline, phys_to_virt(m[4]), sizeof boot_info.cmdline);
    if (!(flags & MB_FLAG_MMAP)) panic("bootloader gave no memory map");
    uint32_t p = m[12], end = m[12] + m[11];
    while (p < end && boot_info.mmap_count < BOOT_MMAP_MAX) {
        const uint8_t* q = phys_to_virt(p);
        uint32_t size = *(const uint32_t*)q;
        mmap_entry_t* e = &boot_info.mmap[boot_info.mmap_count++];
        e->base = *(const uint64_t*)(q + 4);
        e->len = *(const uint64_t*)(q + 12);
        e->type = *(const uint32_t*)(q + 20);
        p += size + 4;
    }
}

// The heap goes right after the kernel image, in the RAM region the
// kernel was loaded into; frames get everything else.
static void setup_memory(void) {
    uintptr_t heap_start = ALIGN_UP(virt_to_phys(_kernel_end), PAGE_SIZE);
    uint64_t usable = 0, region_end = 0, ram_top = 0;
    for (uint32_t i = 0; i < boot_info.mmap_count; i++) {
        const mmap_entry_t* e = &boot_info.mmap[i];
        if (e->type != 1) continue;
        uint64_t hi = e->base + e->len;
        usable += e->len;
        if (hi > ram_top && e->base < RAM_IDENTITY_LIMIT) ram_top = hi;
        if (e->base <= heap_start && hi > heap_start) region_end = hi;
    }
    if (!region_end) panic("no usable RAM after the kernel image");
    if (ram_top > RAM_IDENTITY_LIMIT) ram_top = RAM_IDENTITY_LIMIT;
    if (region_end > RAM_IDENTITY_LIMIT) region_end = RAM_IDENTITY_LIMIT;

    uint64_t want = usable / 8;
    if (want < 1024 * 1024) want = 1024 * 1024;
    if (want > 16 * 1024 * 1024) want = 16 * 1024 * 1024;
    uint64_t room = region_end - heap_start;
    uint32_t size = (uint32_t)(want < room ? want : room);
    if (size < 256 * 1024) panic("only %u KB of RAM after the kernel", size / 1024);
    heap_init(phys_to_virt(heap_start), size);
    frames_init(heap_start + size);
    paging_init((uintptr_t)ram_top);
}

static void banner(void) {
    console_puts_color("RingTail OS ", COLOR_TITLE);
    kprintf_color(COLOR_TITLE, "v%s \"%s\"\n", RINGTAIL_VERSION, RINGTAIL_CODENAME);
    heap_stats_t h;
    heap_get_stats(&h);
    uint32_t frames, free_frames;
    frame_stats(&frames, &free_frames);
    kprintf("Memory: %u KB heap, %u KB for programs\n", h.total / 1024, free_frames * 4);
}

static void start_disk(void) {
    int r = ata_init();
    if (r) {
        kprintf_color(COLOR_DIM, "Disk: none found, files unavailable\n");
        return;
    }
    r = diskfs_init();
    if (r) {
        kprintf_color(COLOR_ERROR, "Disk: %u MB, no usable FAT16 filesystem (error %d)\n",
                      ata_sectors() / 2048, r);
        return;
    }
    int cursor = 0, files = 0;
    dirent_t d;
    while (diskfs_readdir(DISKFS_ROOT, &cursor, &d) == 0) files += !(d.flags & DIRENT_DIR);
    kprintf("Disk: %u MB FAT16, %d files\n", ata_sectors() / 2048, files);
}

void kernel_main(uint32_t magic, uint32_t mbi) {
    serial_init();
    console_init();
    if (magic != MULTIBOOT_MAGIC) panic("not loaded by a multiboot loader (magic %08x)", magic);
    read_boot_info(mbi);

    gdt_init();
    idt_init();
    fpu_init();
    random_init();
    setup_memory();
    task_init();
    gfx_init_font();  // needs text mode, so before anything can switch

    banner();
    // The mouse shares the keyboard controller; set it up first so
    // the keyboard starts from a quiet controller.
    if (mouse_init() == 0) kprintf("Mouse: PS/2\n");
    else kprintf_color(COLOR_DIM, "Mouse: none\n");
    keyboard_init();
    timer_init();
    serial_enable_input();
    start_disk();
    pci_init();
    net_init();

    if (task_create_kernel("init", init_main, 0) < 0) panic("cannot start init");
    sti();
    // Idle: the scheduler runs us only when nothing else is ready.
    for (;;) hlt();
}
