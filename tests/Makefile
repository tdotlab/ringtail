# RingTail host tests: heap + RAMFS, 32-bit host build.
# Needs gcc -m32 (gcc-multilib on Ubuntu). Run with: make run
CC = gcc
CFLAGS = -m32 -Wall -Wextra -g -fno-builtin

TEST = test_heap_ramfs

all: $(TEST)

$(TEST): test_heap_ramfs.c ../heap.c ../ramfs.c
	$(CC) $(CFLAGS) -o $@ $^

run: $(TEST)
	./$(TEST)

clean:
	rm -f $(TEST)
