// v0.7.0: paging and physical page frames. Two ideas that sound
// alike and are not the same. A frame is a physical 4 KB block of
// RAM. A page is a virtual 4 KB block an address refers to. Paging
// is the table that says which virtual page points at which
// physical frame, or at nothing. Without it, every address a
// program uses IS a physical address, and every program shares
// one address space. With it, every program can use the same
// virtual address (0x08000000 here) and land on different
// physical frames, and the kernel's pages can be marked
// supervisor-only so a ring 3 program that touches them faults
// instead of reading kernel memory. That is the memory wall ring
// 3 alone could not provide.
// Layout, classic two-level x86 paging, no PAE. A page directory
// has 1024 entries, each pointing at a page table, each table has
// 1024 entries, each entry points at a 4 KB frame. One directory
// entry covers 4 MB (1024 pages). The kernel identity-maps the
// first 16 MB: virtual address N points at physical address N,
// for N below 16 MB. That covers the kernel image, the heap, the
// VGA memory, and every frame the allocator hands out, because
// the frame pool is capped at 16 MB. Identity mapping is what
// makes turning paging on survivable: the very next instruction
// after the switch is fetched through the new tables, and since
// its virtual address equals its physical address, it is still
// there. It is also what lets kernel code use a physical address
// as a pointer to read or write a page directory or table: those
// structures live below 16 MB, so their physical address works
// as a virtual address under any page directory in this design,
// because every directory copies the kernel's identity entries.
// User pages live at 0x08000000 and up, in one page table per
// task (directory index 32). Flags used here: present (1),
// read/write (2), user (4). Kernel pages are 0x03 (present,
// read/write, supervisor): ring 3 access to them faults.
// User pages are 0x07 (present, read/write, user). There is no
// execute-disable bit in classic paging, so "executable" is not
// a separate permission here; a present user page can be executed.
// Frame allocation is a free list threaded through the frames
// themselves: the first word of a free frame holds the next free
// frame's address, so no extra memory is needed to track them.
// The pool starts just above the heap (aligned up to 4 KB) and
// ends at the smaller of RAM top and 16 MB, so no frame ever
// overlaps the kernel, the heap, or unmapped memory above the
// identity range. Frames are never handed out above 16 MB in
// this slice, even on a machine with more RAM, because the loader
// copies program bytes to frames through their identity-mapped
// address, and that only works below 16 MB.

#include <stdint.h>

#define PAGE_SIZE 4096
#define PD_USER_INDEX 32
#define USER_BASE 0x08000000u

static uint32_t frame_free_head = 0;
static uint32_t kernel_pd_pa = 0;

uint32_t paging_kernel_pd(void) {
    return kernel_pd_pa;
}

uint32_t frame_alloc(void) {
    if (!frame_free_head) return 0;
    uint32_t pa = frame_free_head;
    frame_free_head = *(uint32_t*)pa;
    return pa;
}

void frame_free(uint32_t pa) {
    *(uint32_t*)pa = frame_free_head;
    frame_free_head = pa;
}

static void frame_init(uint32_t heap_end, uint32_t ram_top) {
    uint32_t start = (heap_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t end = ram_top < 0x01000000u ? ram_top : 0x01000000u;
    end &= ~(PAGE_SIZE - 1);
    frame_free_head = 0;
    if (end <= start) return;
    // Link frames in increasing order, head at the lowest frame,
    // so sequential allocations come out consecutive (which the
    // loader does not rely on, but which keeps dumps readable).
    for (uint32_t pa = start; pa + PAGE_SIZE <= end; pa += PAGE_SIZE) {
        uint32_t next = pa + PAGE_SIZE;
        *(uint32_t*)pa = (next + PAGE_SIZE <= end) ? next : 0;
    }
    frame_free_head = start;
}

static void zero_frame(uint32_t pa) {
    uint32_t* p = (uint32_t*)pa;
    for (int i = 0; i < 1024; i++) p[i] = 0;
}

static void paging_enable(void) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(kernel_pd_pa));
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u;
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
    // Flush the pipeline: the next fetch must go through paging.
    __asm__ volatile("jmp 1f\n1:");
}

void paging_init(uint32_t heap_end, uint32_t ram_top) {
    frame_init(heap_end, ram_top);
    kernel_pd_pa = frame_alloc();
    if (!kernel_pd_pa) {
        // No frames at all: cannot build page tables. Halt with a
        // plain message on the VGA screen via print, which still
        // works because paging is not on yet.
        extern void print(const char* s);
        print("No free frames for paging - halting.\n");
        while (1) __asm__ volatile("hlt");
    }
    zero_frame(kernel_pd_pa);
    uint32_t* pd = (uint32_t*)kernel_pd_pa;
    // Identity-map 0-16 MB with 4 page tables, supervisor-only.
    for (int i = 0; i < 4; i++) {
        uint32_t pt = frame_alloc();
        if (!pt) {
            extern void print(const char* s);
            print("Out of frames for kernel page tables - halting.\n");
            while (1) __asm__ volatile("hlt");
        }
        zero_frame(pt);
        uint32_t* table = (uint32_t*)pt;
        for (int j = 0; j < 1024; j++) {
            uint32_t va = (uint32_t)(i * 1024 + j) * PAGE_SIZE;
            table[j] = va | 0x03; // present, read/write, supervisor
        }
        pd[i] = pt | 0x03;
    }
    paging_enable();
}

// Create a page directory for a user task: a fresh frame, kernel
// identity entries copied in, user entries left empty for the
// loader to fill. Returns the directory's physical address, or 0
// on out of frames.
uint32_t paging_create_user_pd(void) {
    uint32_t pd_pa = frame_alloc();
    if (!pd_pa) return 0;
    zero_frame(pd_pa);
    uint32_t* pd = (uint32_t*)pd_pa;
    uint32_t* kpd = (uint32_t*)kernel_pd_pa;
    for (int i = 0; i < 4; i++) pd[i] = kpd[i];
    return pd_pa;
}

// Map one user page: va must be in the user range (directory
// index 32), pa must be a frame. Allocates the user page table
// on first use. Returns 0 on success, -1 on out of frames.
int paging_map_user(uint32_t pd_pa, uint32_t va, uint32_t pa) {
    uint32_t* pd = (uint32_t*)pd_pa;
    uint32_t pde = pd[PD_USER_INDEX];
    uint32_t pt_pa;
    if (!(pde & 1)) {
        pt_pa = frame_alloc();
        if (!pt_pa) return -1;
        zero_frame(pt_pa);
        pd[PD_USER_INDEX] = pt_pa | 0x07; // present, RW, user
    } else {
        pt_pa = pde & ~0xFFFu;
    }
    uint32_t* pt = (uint32_t*)pt_pa;
    uint32_t index = (va >> 12) & 0x3FF;
    pt[index] = (pa & ~0xFFFu) | 0x07; // present, RW, user
    return 0;
}

// Free a user task's paging structures and every user frame they
// map. Walks the single user page table (directory index 32) and
// frees each present frame, then the table, then the directory.
// Kernel identity entries are shared, not owned, so they are left
// alone: freeing them would pull the kernel's own mappings out
// from under every other task.
void paging_destroy_user_pd(uint32_t pd_pa) {
    if (!pd_pa || pd_pa == kernel_pd_pa) return;
    uint32_t* pd = (uint32_t*)pd_pa;
    uint32_t pde = pd[PD_USER_INDEX];
    if (pde & 1) {
        uint32_t pt_pa = pde & ~0xFFFu;
        uint32_t* pt = (uint32_t*)pt_pa;
        for (int i = 0; i < 1024; i++) {
            if (pt[i] & 1) frame_free(pt[i] & ~0xFFFu);
        }
        frame_free(pt_pa);
    }
    frame_free(pd_pa);
}

// Page fault diagnostics: the stub in isr.asm passes the CPU's
// error code and the faulting address (CR2). v0.7.1: this is
// now a recovery point, not just a diagnostic. If the faulting
// task is a user program, the handler breaks the print lock (the
// fault may have happened while the program held it through a
// syscall, and a dead owner would deadlock every later print),
// prints the fault, and kills the task via task_exit, which
// switches away and never returns: the task's page directory,
// frames, and stacks are freed by the main loop's cleanup, and
// the shell and every other task keep running. If the faulting
// task is a kernel task, there is no safe recovery: a kernel
// fault is an OS bug, and killing the shell would leave no
// prompt at all, so the handler prints and halts, as before.
// Note the decision is by TASK, not by privilege level at fault
// time: a syscall with a bad user pointer faults in ring 0
// (the kernel is reading the pointer), but it is still the user
// program's fault, so it is still killed, not halted. That turns
// the shallow syscall pointer validation from an OS-killer into
// a program-killer, which is the right tradeoff for this scope.
// The error code bits, for the reader: bit 0 = page was present
// (1) or not (0), bit 1 = write (1) or read (0), bit 2 = fault
// from user mode (1) or kernel (0). So "code 5, addr 0" means a
// user read of a present supervisor page: the memory wall doing
// its job, now with a survivable ending.
extern void print(const char* s);
extern void print_char(char c);
extern void vga_lock_break(void);
extern int task_current_is_user(void);
extern void task_exit(void);

static void print_hex32(uint32_t n) {
    const char* digits = "0123456789ABCDEF";
    print("0x");
    for (int i = 7; i >= 0; i--) print_char(digits[(n >> (i * 4)) & 0xF]);
}

void page_fault_handler(uint32_t err, uint32_t addr) {
    // Interrupts are off in the trap (interrupt gate), so no
    // other task can print concurrently during this diagnostic:
    // printing without the print lock here is safe, and breaking
    // the lock first is what makes later prints safe again.
    vga_lock_break();
    print("\nPAGE FAULT addr=");
    print_hex32(addr);
    print(" code=");
    // Small decimal print, local to avoid depending on a helper.
    char buf[12];
    int i = 0;
    if (err == 0) { print_char('0'); }
    else {
        while (err > 0) { buf[i++] = '0' + (err % 10); err /= 10; }
        while (i > 0) print_char(buf[--i]);
    }
    if (task_current_is_user()) {
        print(" - task killed.\n");
        task_exit(); // never returns: switches away from the dead task
        while (1) __asm__ volatile("hlt"); // backstop, unreachable
    }
    print(" - system halted.\n");
    while (1) __asm__ volatile("hlt");
}
