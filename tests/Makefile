# RingTail host tests: heap + RAMFS, 32-bit host build.
# Needs gcc -m32 (gcc-multilib on Ubuntu). Run with: make run
CC = gcc
CFLAGS = -m32 -Wall -Wextra -g -fno-builtin

TEST = test_heap_ramfs

all: $(TEST)

$(TEST): test_heap_ramfs.c ../heap.c ../ramfs.c
	$(CC) $(CFLAGS) -o $@ $^

# Native 32-bit execution is unavailable on some VMs (on this one
# even a trivial 32-bit binary segfaults), so fall back to
# qemu-i386 user emulation when the native run fails.
run: $(TEST)
	./$(TEST) || qemu-i386 ./$(TEST)

clean:
	rm -f $(TEST)
