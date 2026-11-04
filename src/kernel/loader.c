// v0.7.0: the program loader. This is what turns a file on disk
// into a running program, and it is deliberately the smallest
// loader that is still honest. A program file is a flat binary:
// raw machine code, linked to run at virtual address 0x08000000,
// with its entry point at offset 0 (the first byte is the first
// instruction). There is no ELF parser, no relocation, no dynamic
// linking: the file says "I run at this address", the loader puts
// it there, and the program's own addresses (string literals,
// function calls) already match, because it was linked for that
// address. That only works because every program loads at the
// same virtual address, in its own address space: two programs
// can both live at 0x08000000 at once, on different physical
// frames, because each task has its own page directory.
// Steps, in order. Read the file whole into a heap buffer
// (capped at 64 KB by diskfs). Work out how many 4 KB pages it
// needs, plus one guard page, plus one stack page: the program
// occupies pages 0..N-1, page N is left unmapped on purpose so
// a stack overflow or a runaway write faults instead of silently
// corrupting the program, and page N+1 is the stack, with its
// top as the program's initial stack pointer. Create a page
// directory, map each program page to a fresh frame and copy the
// file's bytes into it through the frame's identity-mapped
// address (frames live below 16 MB, so their physical address
// works as a kernel virtual address), map the stack page to a
// fresh frame, and hand the directory, entry address, and stack
// top to task_create_user. If any step fails (no file, no
// frames, no page table), the loader frees what it allocated and
// returns -1, and no half-built task is left behind. On success
// the heap buffer is freed: the program now lives only in its
// frames, and the buffer was just a staging area. The task's
// page directory owns all of it, and task_cleanup frees it via
// paging_destroy_user_pd when the program exits or is killed,
// so the loader never has to track frames itself after handoff.

#include <stdint.h>

#define USER_BASE 0x08000000u
#define PAGE_SIZE 4096

extern uint8_t* diskfs_read_all(const char* name, uint32_t* size_out);
extern int kfree(void* ptr);
extern uint32_t paging_create_user_pd(void);
extern int paging_map_user(uint32_t pd_pa, uint32_t va, uint32_t pa);
extern void paging_destroy_user_pd(uint32_t pd_pa);
extern uint32_t frame_alloc(void);
extern void frame_free(uint32_t pa);
extern int task_create_user(const char* name, uint32_t pd_pa, uint32_t entry_va, uint32_t stack_top_va);

static void copy_bytes(uint8_t* dst, const uint8_t* src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
}

int loader_run(const char* name) {
    uint32_t size = 0;
    uint8_t* buf = diskfs_read_all(name, &size);
    if (!buf) return -1;
    uint32_t npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (npages == 0) npages = 1; // empty file still gets one page
    uint32_t pd = paging_create_user_pd();
    if (!pd) { kfree(buf); return -1; }
    int ok = 1;
    // Map and fill program pages.
    for (uint32_t i = 0; i < npages && ok; i++) {
        uint32_t frame = frame_alloc();
        if (!frame) { ok = 0; break; }
        if (paging_map_user(pd, USER_BASE + i * PAGE_SIZE, frame) != 0) {
            frame_free(frame);
            ok = 0;
            break;
        }
        uint32_t off = i * PAGE_SIZE;
        uint32_t chunk = size > off ? size - off : 0;
        if (chunk > PAGE_SIZE) chunk = PAGE_SIZE;
        uint8_t* dst = (uint8_t*)frame; // identity-mapped
        copy_bytes(dst, buf + off, chunk);
        for (uint32_t j = chunk; j < PAGE_SIZE; j++) dst[j] = 0;
    }
    // Stack page, after one guard page (left unmapped).
    uint32_t stack_va = USER_BASE + (npages + 1) * PAGE_SIZE;
    uint32_t stack_frame = 0;
    if (ok) {
        stack_frame = frame_alloc();
        if (!stack_frame) ok = 0;
        else if (paging_map_user(pd, stack_va, stack_frame) != 0) {
            frame_free(stack_frame);
            ok = 0;
        }
    }
    kfree(buf);
    if (!ok) {
        // Destroying the directory frees every frame it maps,
        // including any program pages mapped before the failure.
        paging_destroy_user_pd(pd);
        return -1;
    }
    uint32_t stack_top = stack_va + PAGE_SIZE;
    int id = task_create_user(name, pd, USER_BASE, stack_top);
    if (id < 0) {
        paging_destroy_user_pd(pd);
        return -1;
    }
    return id;
}
