# Host tests: the kernel heap and FAT16 code built as a 32-bit Linux
# program against a RAM disk. Needs gcc -m32 (gcc-multilib). The
# disk image the run leaves behind is checked with fsck.fat when it
# is installed.
CC = gcc
CFLAGS = -m32 -std=gnu11 -O1 -g -Wall -Wextra -fno-builtin -I../src/include
TEST = test_heap_diskfs
SRCS = test_heap_diskfs.c ../src/mm/heap.c ../src/fs/diskfs.c ../src/lib/string.c

all: $(TEST)

$(TEST): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

run: $(TEST)
	./$(TEST) || qemu-i386 ./$(TEST)
	@if command -v fsck.fat >/dev/null; then \
		fsck.fat -n test_disk.img >/dev/null && echo "fsck.fat: disk image is clean"; \
	else echo "fsck.fat not installed, skipping the image check"; fi

clean:
	rm -f $(TEST) test_disk.img
