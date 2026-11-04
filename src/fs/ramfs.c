// Files live in heap, disappear on reboot - like /tmp

#include <stdint.h>
#include <stddef.h>

#define MAX_NAME 32

typedef struct file {
    char name[MAX_NAME];
    uint32_t size;
    uint8_t* data;
    struct file* next;
} file_t;

static file_t* file_list = NULL;

extern void* kmalloc(uint32_t size);
extern int kfree(void* ptr);
extern void print(const char* s);
extern void print_char(char c);

// tiny string helpers (no libc)
static int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static void strcpy(char* dst, const char* src) {
    while ((*dst++ = *src++));
}
static uint32_t strlen(const char* s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}
static void memcpy(uint8_t* dst, const uint8_t* src, uint32_t n) {
    for (uint32_t i=0;i<n;i++) dst[i]=src[i];
}

// v0.5.1: same interrupt save/restore as the heap (see heap.c
// for why save/restore, not bare cli/sti, and why the
// RINGTAIL_KERNEL guard exists). Every public ramfs function
// below runs under it, so the file list cannot be changed
// mid-walk or mid-unlink by a task switch. The private find
// helper does not lock: it is only ever called from already
// locked public functions, and save/restore nests safely anyway,
// so locking it too would be redundant, not wrong. Heap calls
// inside ramfs nest the same way: ramfs saves (interrupts on),
// kmalloc saves (interrupts off) and restores (off), ramfs
// restores (on).
#ifdef RINGTAIL_KERNEL
static uint32_t irq_save(void) {
    uint32_t flags;
    __asm__ volatile("pushf; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    return flags;
}
static void irq_restore(uint32_t flags) {
    if (flags & 0x200) __asm__ volatile("sti");
}
#else
static uint32_t irq_save(void) { return 0x200; }
static void irq_restore(uint32_t flags) { (void)flags; }
#endif

void ramfs_init(void) {
    file_list = NULL;
}

static file_t* ramfs_find(const char* name) {
    file_t* curr = file_list;
    while (curr) {
        if (strcmp(curr->name, name)==0) return curr;
        curr = curr->next;
    }
    return NULL;
}

int ramfs_create(const char* name) {
    // Single return point so every path restores interrupts,
    // matching the pattern in task_create.
    uint32_t flags = irq_save();
    int result = 0;
    if (ramfs_find(name)) result = -1; // already exists
    else if (strlen(name) >= MAX_NAME) result = -2;
    else {
        file_t* f = (file_t*)kmalloc(sizeof(file_t));
        if (!f) result = -3;
        else {
            strcpy(f->name, name);
            f->size = 0;
            f->data = NULL;
            f->next = file_list;
            file_list = f;
        }
    }
    irq_restore(flags);
    return result;
}

int ramfs_write(const char* name, const char* content) {
    uint32_t flags = irq_save();
    int result = 0;
    file_t* f = ramfs_find(name);
    if (!f) {
        if (ramfs_create(name)!=0) result = -1;
        else {
            f = ramfs_find(name);
            if (!f) result = -1;
        }
    }
    if (result == 0) {
        uint32_t len = strlen(content);
        // Allocate first, then free the old data. The old code freed
        // first, so an out-of-memory write left the file pointing at a
        // freed block with its old size.
        uint8_t* new_data = (uint8_t*)kmalloc(len+1);
        if (!new_data) result = -2;
        else {
            if (f->data) kfree(f->data);
            memcpy(new_data, (uint8_t*)content, len);
            new_data[len]=0;
            f->data = new_data;
            f->size = len;
        }
    }
    irq_restore(flags);
    return result;
}

int ramfs_read(const char* name, char* out, uint32_t max) {
    uint32_t flags = irq_save();
    int result = -1;
    if (max == 0) result = -1; // otherwise max-1 below would underflow
    else {
        file_t* f = ramfs_find(name);
        if (!f) result = -1;
        else if (!f->data) {
            out[0]=0;
            result = 0;
        } else {
            uint32_t to_copy = f->size;
            if (to_copy >= max) to_copy = max-1;
            memcpy((uint8_t*)out, f->data, to_copy);
            out[to_copy]=0;
            result = (int)to_copy;
        }
    }
    irq_restore(flags);
    return result;
}

int ramfs_delete(const char* name) {
    uint32_t flags = irq_save();
    int result = -1;
    file_t* curr = file_list;
    file_t* prev = NULL;
    while (curr) {
        if (strcmp(curr->name, name)==0) {
            if (prev) prev->next = curr->next;
            else file_list = curr->next;
            if (curr->data) kfree(curr->data);
            kfree(curr);
            result = 0;
            break;
        }
        prev = curr;
        curr = curr->next;
    }
    irq_restore(flags);
    return result;
}

void ramfs_list(void) {
    // Note: this prints under the shell's print lock (it is only
    // called from shell_execute), so its output cannot interleave
    // with a counter line, and the irq lock here keeps the list
    // walk itself atomic. The two locks nest in a fixed order
    // (print lock, then irq lock), never the other way, so no
    // lock-ordering deadlock is possible.
    uint32_t flags = irq_save();
    file_t* curr = file_list;
    if (!curr) {
        print("empty\n");
        irq_restore(flags);
        return;
    }
    while (curr) {
        print(curr->name);
        print(" (");
        // print size
        // simple decimal
        uint32_t n = curr->size;
        char buf[12];
        int i=0;
        if (n==0) { print_char('0'); }
        else {
            while (n>0) { buf[i++]='0'+(n%10); n/=10; }
            while (i>0) print_char(buf[--i]);
        }
        print(" bytes)\n");
        curr = curr->next;
    }
    irq_restore(flags);
}

int ramfs_exists(const char* name) {
    uint32_t flags = irq_save();
    int result = ramfs_find(name)!=NULL;
    irq_restore(flags);
    return result;
}
