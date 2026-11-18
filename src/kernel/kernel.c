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
#define MB_FLAG_FB      (1u << 12)
#define MB_FB_RGB       1           // direct colour; 0 is a palette, 2 is EGA text

boot_info_t boot_info;

// The framebuffer GRUB set up, if it set one up and it is a kind the
// console can draw on: direct colour, 15 to 32 bits a pixel, at least
// 640x400 (80x25 characters).
static void read_framebuffer(const uint32_t* m) {
    const uint8_t* b = (const uint8_t*)m;
    framebuffer_t f = { 0 };
    f.addr = (uint64_t)m[22] | (uint64_t)m[23] << 32;
    f.pitch = m[24];
    f.width = m[25];
    f.height = m[26];
    f.bpp = b[108];
    if (b[109] != MB_FB_RGB || !f.addr) return;
    if (f.bpp != 32 && f.bpp != 24 && f.bpp != 16 && f.bpp != 15) return;
    if (f.width < 640 || f.height < 400 || f.pitch < f.width * ((f.bpp + 7u) / 8)) return;
    // The multiboot spec puts the colour layout at byte 110, but GRUB's
    // own header declares it in a union that also holds a 32-bit
    // field, so the compiler aligned it to 112, and 112 is what GRUB
    // fills in.
    f.red_pos = b[112], f.red_bits = b[113];
    f.green_pos = b[114], f.green_bits = b[115];
    f.blue_pos = b[116], f.blue_bits = b[117];
    if (!f.red_bits || f.red_bits > 8 || !f.green_bits || f.green_bits > 8 || !f.blue_bits || f.blue_bits > 8) return;
    boot_info.fb = f;
}

// Copy what we need out of the multiboot structures, which live in
// memory the frame allocator is about to hand out. This runs before
// the console, which needs to know what the screen is, so it cannot
// complain; kernel_main does once the console is up.
// The bootloader hands over 32-bit physical addresses.
static void read_boot_info(uint32_t mbi) {
    const uint32_t* m = phys_to_virt(mbi);
    uint32_t flags = m[0];
    if (flags & MB_FLAG_CMDLINE)
        strlcpy(boot_info.cmdline, phys_to_virt(m[4]), sizeof boot_info.cmdline);
    if (flags & MB_FLAG_FB) read_framebuffer(m);
    if (!(flags & MB_FLAG_MMAP)) return;
    boot_info.have_mmap = 1;
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
// kernel was loaded into; frames get everything else. Until paging_init
// has mapped all of RAM, the kernel reaches only the first 4 GB
// (BOOT_MAP_LIMIT), so the heap stays below that.
static void setup_memory(void) {
    uintptr_t heap_start = ALIGN_UP(virt_to_phys(_kernel_end), PAGE_SIZE);
    uint64_t usable = 0, region_end = 0, ram_top = 0;
    for (uint32_t i = 0; i < boot_info.mmap_count; i++) {
        const mmap_entry_t* e = &boot_info.mmap[i];
        if (e->type != 1) continue;
        uint64_t hi = e->base + e->len;
        usable += e->len;
        if (hi > ram_top) ram_top = hi;
        if (e->base <= heap_start && hi > heap_start) region_end = hi;
    }
    if (!region_end) panic("no usable RAM after the kernel image");
    if (ram_top > DIRECT_MAP_SIZE) ram_top = DIRECT_MAP_SIZE;
    if (region_end > BOOT_MAP_LIMIT) region_end = BOOT_MAP_LIMIT;

    uint64_t want = usable / 8;
    if (want < 1024 * 1024) want = 1024 * 1024;
    if (want > 16 * 1024 * 1024) want = 16 * 1024 * 1024;
    uint64_t room = region_end - heap_start;
    uint32_t size = (uint32_t)(want < room ? want : room);
    if (size < 256 * 1024) panic("only %u KB of RAM after the kernel", size / 1024);
    heap_init(phys_to_virt(heap_start), size);
    frames_init(heap_start + size, (uintptr_t)ram_top);
    paging_init((uintptr_t)ram_top);
    frames_mapped();
}

static void banner(void) {
    console_puts_color("RingTail OS ", COLOR_TITLE);
    kprintf_color(COLOR_TITLE, "v%s \"%s\"\n", RINGTAIL_VERSION, RINGTAIL_CODENAME);
    heap_stats_t h;
    heap_get_stats(&h);
    uint32_t frames, free_frames;
    frame_stats(&frames, &free_frames);
    kprintf("Memory: %u KB heap, %u KB for programs\n", h.total / 1024, free_frames * 4);
    char buf[64];
    kprintf("Interrupts: %s\n", irq_controller(buf, sizeof buf));
    kprintf("Display: %s\n", console_describe(buf, sizeof buf));
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
    // First, since it sets up gs, which finds the running task and the
    // kernel lock's owner (arch/percpu.h); that lock is this CPU's from
    // here until it goes idle.
    gdt_init();
    kernel_lock();
    serial_init();
    if (magic == MULTIBOOT_MAGIC) read_boot_info(mbi);
    console_init();
    if (magic != MULTIBOOT_MAGIC) panic("not loaded by a multiboot loader (magic %08x)", magic);
    if (!boot_info.have_mmap) panic("bootloader gave no memory map");

    idt_init();
    fpu_init();
    random_init();
    setup_memory();
    irq_controller_init();  // the APICs' registers go through the page tables
    console_map();          // and so does the framebuffer
    task_init();
    gfx_init();  // on VGA, reads the font while still in text mode

    banner();
    // The mouse shares the keyboard controller; set it up first so
    // the keyboard starts from a quiet controller.
    if (mouse_init() == 0) kprintf("Mouse: PS/2\n");
    else kprintf_color(COLOR_DIM, "Mouse: none\n");
    keyboard_init();
    timer_init();
    char buf[64];
    kprintf("Timer: %s\n", timer_source(buf, sizeof buf));
    smp_init();
    kprintf("CPUs: %s\n", smp_describe(buf, sizeof buf));
    serial_enable_input();
    start_disk();
    pci_init();
    net_init();

    console_start();  // from here the console draws in a task of its own
    if (task_create_kernel("init", init_main, 0) < 0) panic("cannot start init");
    // Idle: the scheduler runs us only when nothing else is ready, and
    // then the other CPUs may have the kernel.
    kernel_unlock();
    sti();
    for (;;) hlt();
}
