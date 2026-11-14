// Checked access to user memory, and stack growth: the parts of
// paging that do not depend on the page table format.
#include "kernel.h"
#include "mm.h"
#include "task.h"
#include "ringtail/abi.h"

static int in_stack_region(uintptr_t addr) {
    return addr >= USER_STACK_LOW && addr < USER_STACK_TOP;
}

int user_fault_fixup(uintptr_t addr, uintptr_t err) {
    if (err & 1) return 0;  // the page was present: a protection violation
    if (!in_stack_region(addr)) return 0;
    return as_map(current->cr3, ALIGN_DOWN(addr, PAGE_SIZE), 1) != 0;
}

// Validate one page of the current address space for access, growing
// the stack if the page lies in the stack region and is not mapped.
static int page_ok(uintptr_t va, int write) {
    int w = 0;
    if (as_lookup(current->cr3, va, &w)) return !write || w;
    if (in_stack_region(va)) return as_map(current->cr3, ALIGN_DOWN(va, PAGE_SIZE), 1) != 0;
    return 0;
}

int user_range_ok(uintptr_t uaddr, size_t len, int write) {
    if (len == 0) return 0;
    uintptr_t end = uaddr + len;
    if (uaddr < USER_BASE || end < uaddr || end > USER_TOP) return -EFAULT;
    for (uintptr_t p = ALIGN_DOWN(uaddr, PAGE_SIZE); p < end; p += PAGE_SIZE) {
        if (!page_ok(p, write)) return -EFAULT;
        if (p + PAGE_SIZE < p) break;
    }
    return 0;
}

int copy_from_user(void* dst, const void* usrc, size_t len) {
    if (user_range_ok((uintptr_t)usrc, len, 0)) return -EFAULT;
    memcpy(dst, usrc, len);
    return 0;
}

int copy_to_user(void* udst, const void* src, size_t len) {
    if (user_range_ok((uintptr_t)udst, len, 1)) return -EFAULT;
    memcpy(udst, src, len);
    return 0;
}

int strncpy_from_user(char* dst, const char* usrc, size_t max) {
    uintptr_t a = (uintptr_t)usrc;
    for (size_t i = 0; i < max; i++, a++) {
        if (i == 0 || (a & (PAGE_SIZE - 1)) == 0) {
            if (user_range_ok(a, 1, 0)) return -EFAULT;
        }
        dst[i] = *(const char*)a;
        if (!dst[i]) return (int)i;
    }
    if (max) dst[max - 1] = 0;
    return -ENAMETOOLONG;
}
