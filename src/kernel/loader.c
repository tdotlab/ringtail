// Program loader: reads a static 32-bit ELF executable from the disk
// into a fresh address space and starts it as a user task.
//
// The file is untrusted. Every header field is checked before use,
// segments must lie inside the user image range, and file offsets
// must lie inside the file. Anything else is -ENOEXEC.
//
// The program break starts at the first page after the image, so the
// heap grows up from there toward the stack's guard gap.
//
// Initial user stack, from the top down: the argument strings, the
// argv array (NULL terminated), then argc and argv for _start, laid
// out as if _start(argc, argv) had been called normally, with argc
// 16-byte aligned as the i386 ABI expects.
#include "kernel.h"
#include "task.h"
#include "fs.h"
#include "mm.h"
#include "lib.h"
#include "ringtail/abi.h"

#define ET_EXEC 2
#define EM_386 3
#define PT_LOAD 1
#define PF_W 2
#define MAX_PHDRS 16
#define ARGS_MAX_BYTES 2048

typedef struct {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf_header_t;

typedef struct {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
} elf_phdr_t;

static int read_exact(int node, void* buf, uint32_t len, uint32_t off) {
    int n = diskfs_pread(node, buf, len, off);
    if (n < 0) return n;
    return (uint32_t)n == len ? 0 : -ENOEXEC;
}

static int load_segment(int node, uint32_t pd, const elf_phdr_t* ph, uint32_t file_size) {
    if (ph->filesz > ph->memsz) return -ENOEXEC;
    if (ph->offset > file_size || ph->filesz > file_size - ph->offset) return -ENOEXEC;
    if (ph->vaddr < USER_BASE || ph->memsz > USER_IMAGE_LIMIT - ph->vaddr) return -ENOEXEC;
    if (ph->vaddr >= USER_IMAGE_LIMIT) return -ENOEXEC;

    uint32_t file_end = ph->vaddr + ph->filesz;
    uint32_t mem_end = ph->vaddr + ph->memsz;
    for (uint32_t va = ALIGN_DOWN(ph->vaddr, PAGE_SIZE); va < mem_end; va += PAGE_SIZE) {
        uint32_t frame = as_map(pd, va, (ph->flags & PF_W) != 0);
        if (!frame) return -ENOMEM;
        // Frames come zeroed, which takes care of .bss. Copy the part
        // of the file that lands in this page, through the kernel's
        // identity mapping of the frame.
        uint32_t lo = MAX(va, ph->vaddr);
        uint32_t hi = MIN(va + PAGE_SIZE, file_end);
        if (lo < hi) {
            int r = read_exact(node, (void*)(frame + (lo - va)), hi - lo, ph->offset + (lo - ph->vaddr));
            if (r) return r;
        }
    }
    return 0;
}

static int load_image(int node, uint32_t pd, uint32_t* entry, uint32_t* end) {
    elf_header_t h;
    uint32_t size = diskfs_size(node);
    int r = read_exact(node, &h, sizeof h, 0);
    if (r) return r;
    if (h.ident[0] != 0x7F || h.ident[1] != 'E' || h.ident[2] != 'L' || h.ident[3] != 'F') return -ENOEXEC;
    if (h.ident[4] != 1 || h.ident[5] != 1) return -ENOEXEC;  // 32-bit, little endian
    if (h.type != ET_EXEC || h.machine != EM_386 || h.version != 1) return -ENOEXEC;
    if (h.phentsize != sizeof(elf_phdr_t) || h.phnum == 0 || h.phnum > MAX_PHDRS) return -ENOEXEC;
    if (h.entry < USER_BASE || h.entry >= USER_IMAGE_LIMIT) return -ENOEXEC;

    elf_phdr_t ph[MAX_PHDRS];
    r = read_exact(node, ph, h.phnum * sizeof(elf_phdr_t), h.phoff);
    if (r) return r;
    int loaded = 0;
    uint32_t top = 0;
    for (int i = 0; i < h.phnum; i++) {
        if (ph[i].type != PT_LOAD || ph[i].memsz == 0) continue;
        r = load_segment(node, pd, &ph[i], size);
        if (r) return r;
        loaded++;
        if (ph[i].vaddr + ph[i].memsz > top) top = ph[i].vaddr + ph[i].memsz;
    }
    if (!loaded) return -ENOEXEC;
    *entry = h.entry;
    *end = ALIGN_UP(top, PAGE_SIZE);
    return 0;
}

// Build the initial stack in the top stack page. Returns the user esp.
static int setup_stack(uint32_t pd, int argc, const char* const* argv, uint32_t* esp_out) {
    uint32_t base = USER_STACK_TOP - PAGE_SIZE;
    uint8_t* page = (uint8_t*)as_map(pd, base, 1);
    if (!page) return -ENOMEM;

    uint32_t sp = PAGE_SIZE;  // offset into the page
    uint32_t uargv[ARGV_MAX + 1];
    for (int i = argc - 1; i >= 0; i--) {
        uint32_t len = strlen(argv[i]) + 1;
        if (PAGE_SIZE - sp + len > ARGS_MAX_BYTES) return -E2BIG;
        sp -= len;
        memcpy(page + sp, argv[i], len);
        uargv[i] = base + sp;
    }
    uargv[argc] = 0;
    sp = ALIGN_DOWN(sp, 4) - (uint32_t)(argc + 1) * 4;
    memcpy(page + sp, uargv, (uint32_t)(argc + 1) * 4);
    uint32_t argv_addr = base + sp;

    uint32_t a = ALIGN_DOWN(sp - 8, 16);  // where argc goes
    uint32_t* w = (uint32_t*)(page + a);
    w[0] = (uint32_t)argc;
    w[1] = argv_addr;
    w[-1] = 0;  // return address: _start never returns
    *esp_out = base + a - 4;
    return 0;
}

int loader_spawn(int at, const char* path, int argc, const char* const* argv, int job, const int std[3], int held) {
    if (argc < 1 || argc > ARGV_MAX) return -E2BIG;
    int node = diskfs_lookup(at, path);  // referenced, so nobody deletes it while we read it
    if (node < 0) return node;

    int r;
    uint32_t entry = 0, esp = 0, end = 0;
    uint32_t pd = as_create();
    if (!pd) { r = -ENOMEM; goto out; }
    r = load_image(node, pd, &entry, &end);
    if (!r) r = setup_stack(pd, argc, argv, &esp);
    if (r) { as_destroy(pd); goto out; }
    r = task_create_user(argv[0], pd, entry, esp, end, job, std, held);  // owns pd from here
out:
    diskfs_unref(node);
    return r;
}
