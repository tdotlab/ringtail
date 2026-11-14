// Memory management: kernel heap, physical frames, address spaces,
// and checked access to user memory.
#ifndef RINGTAIL_MM_H
#define RINGTAIL_MM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096u

// ---- Kernel heap (heap.c) ----
// First-fit allocator with coalescing. Safe to call with interrupts
// on or off, but not from an interrupt handler that interrupted the
// heap itself (it disables interrupts internally, so that cannot
// happen).
void heap_init(void* start, size_t size);
void* kmalloc(size_t size);
void* kzalloc(size_t size);
// Returns 0, -1 for a pointer that is not a heap block, -2 for a
// block that is already free.
int kfree(void* p);

typedef struct {
    uintptr_t start;
    uint32_t total, used, free, free_blocks, largest_free;
} heap_stats_t;
void heap_get_stats(heap_stats_t* s);

// ---- Physical memory as the kernel sees it ----
// The kernel reaches RAM through a mapping of physical memory: today
// the identity map of the first gigabyte, so these change nothing.
// Every place that turns a physical address into a pointer, or a
// pointer into an address a device will use, goes through them, so
// the mapping can move (to the top of a 64-bit address space) without
// hunting for the places that assumed it.
static inline void* phys_to_virt(uintptr_t phys) { return (void*)phys; }
static inline uintptr_t virt_to_phys(const void* p) { return (uintptr_t)p; }

// ---- Physical frames (frame.c) ----
// Hand every usable frame above 1 MB to the allocator, except the
// kernel image and everything up to reserved_end (the heap).
void frames_init(uintptr_t reserved_end);
// Returns a zeroed 4 KB frame, or 0 when memory is exhausted.
// Frames below RAM_IDENTITY_LIMIT only, so the kernel can always
// reach a frame through phys_to_virt.
uintptr_t frame_alloc(void);
void frame_free(uintptr_t phys);
void frame_stats(uint32_t* total, uint32_t* free);

// ---- Paging (paging.c) ----
// Kernel space is the first 1 GB, identity mapped and supervisor
// only. User space is 1 GB to 3 GB, private to each address space.
#define RAM_IDENTITY_LIMIT 0x40000000u
#define USER_BASE        0x40000000u
#define USER_TOP         0xC0000000u
#define USER_STACK_TOP   USER_TOP
#define USER_STACK_MAX   (1024u * 1024u)
#define USER_STACK_LOW   (USER_STACK_TOP - USER_STACK_MAX)
// Program images must end below this, leaving a guard gap that is
// never mapped between the image and the lowest possible stack page.
#define USER_IMAGE_LIMIT (USER_STACK_LOW - PAGE_SIZE)

// A page table entry is as wide as a pointer: 32 bits on i386 (no
// PAE), 64 on x86_64.
typedef uintptr_t pte_t;
#define PTE_PRESENT  0x001
#define PTE_WRITE    0x002
#define PTE_USER     0x004
#define PTE_PS       0x080
#define PTE_GLOBAL   0x100

void paging_init(uintptr_t ram_top);
uintptr_t kernel_pd(void);

// Device registers. Kernel only, uncached, mapped at boot before any
// program starts (the window's page table is shared by every address
// space). Returns the virtual address of phys, or 0 when the window
// (MMIO_WINDOW_SIZE in all) is full.
#define MMIO_WINDOW      0xC0000000u
#define MMIO_WINDOW_SIZE 0x00400000u
void* mmio_map(uintptr_t phys, uint32_t size);

// Address spaces are identified by their page directory's physical
// (equal to virtual, below 1 GB) address.
uintptr_t as_create(void);
void as_destroy(uintptr_t pd);
// Map a fresh zeroed frame at va (page aligned, user range). If the
// page is already mapped, it is kept and its write permission is
// widened when writable is set. Returns the frame or 0 when out of
// memory.
uintptr_t as_map(uintptr_t pd, uintptr_t va, int writable);
// Frame backing va, or 0 when it is not mapped.
uintptr_t as_lookup(uintptr_t pd, uintptr_t va, int* writable);
// Count of user pages mapped, for the task list.
uint32_t as_pages(uintptr_t pd);

// Called for a page fault taken in user mode. Returns 1 if the
// fault was a stack access that has now been satisfied by growing
// the stack, 0 if the program must be killed.
int user_fault_fixup(uintptr_t addr, uintptr_t err);

// ---- Checked user access (usercopy in paging.c) ----
// These validate against the current task's page tables: every byte
// must be in the user range, mapped, user accessible, and writable
// for writes. They return 0 or -EFAULT. Nothing is copied unless the
// whole range is valid.
int user_range_ok(uintptr_t uaddr, size_t len, int write);
int copy_from_user(void* dst, const void* usrc, size_t len);
int copy_to_user(void* udst, const void* src, size_t len);
// Copy a NUL-terminated string of at most max-1 characters. Returns
// its length, -EFAULT, or -ENAMETOOLONG when it does not fit.
int strncpy_from_user(char* dst, const char* usrc, size_t max);

#endif
