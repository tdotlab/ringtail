// RingTail - v0.2.0 Piece 10: RAM filesystem (ramfs)
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
    if (ramfs_find(name)) return -1; // already exists
    if (strlen(name) >= MAX_NAME) return -2;
    file_t* f = (file_t*)kmalloc(sizeof(file_t));
    if (!f) return -3;
    strcpy(f->name, name);
    f->size = 0;
    f->data = NULL;
    f->next = file_list;
    file_list = f;
    return 0;
}

int ramfs_write(const char* name, const char* content) {
    file_t* f = ramfs_find(name);
    if (!f) {
        if (ramfs_create(name)!=0) return -1;
        f = ramfs_find(name);
        if (!f) return -1;
    }
    uint32_t len = strlen(content);
    // Allocate first, then free the old data. The old code freed
    // first, so an out-of-memory write left the file pointing at a
    // freed block with its old size.
    uint8_t* new_data = (uint8_t*)kmalloc(len+1);
    if (!new_data) return -2;
    if (f->data) kfree(f->data);
    memcpy(new_data, (uint8_t*)content, len);
    new_data[len]=0;
    f->data = new_data;
    f->size = len;
    return 0;
}

int ramfs_read(const char* name, char* out, uint32_t max) {
    if (max == 0) return -1; // otherwise max-1 below would underflow
    file_t* f = ramfs_find(name);
    if (!f) return -1;
    if (!f->data) {
        if (max>0) out[0]=0;
        return 0;
    }
    uint32_t to_copy = f->size;
    if (to_copy >= max) to_copy = max-1;
    memcpy((uint8_t*)out, f->data, to_copy);
    out[to_copy]=0;
    return to_copy;
}

int ramfs_delete(const char* name) {
    file_t* curr = file_list;
    file_t* prev = NULL;
    while (curr) {
        if (strcmp(curr->name, name)==0) {
            if (prev) prev->next = curr->next;
            else file_list = curr->next;
            if (curr->data) kfree(curr->data);
            kfree(curr);
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }
    return -1;
}

void ramfs_list(void) {
    file_t* curr = file_list;
    if (!curr) {
        print("empty\n");
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
}

int ramfs_exists(const char* name) {
    return ramfs_find(name)!=NULL;
}
