# Host tests: the kernel heap and FAT16 code built as a 32-bit Linux
# program against a RAM disk; the DHCP and DNS message code (the parts
# that read bytes off the wire), the TCP engine (run against a
# simulated network) and the regular-expression engine grep uses (run
# against GNU grep's answers), under the address and undefined-behaviour
# sanitizers. Needs gcc -m32 (gcc-multilib). The
# disk image the run leaves behind is checked with fsck.fat when it
# is installed.
CC = gcc
CFLAGS = -m32 -std=gnu11 -O1 -g -Wall -Wextra -fno-builtin -I../src/include
TEST = test_heap_diskfs
SRCS = test_heap_diskfs.c ../src/mm/heap.c ../src/fs/diskfs.c ../src/lib/string.c

NETMSG_SRCS = test_netmsg.c ../src/net/dhcpmsg.c ../src/programs/lib/dnsmsg.c
NETMSG_CFLAGS = -m32 -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
	-I../src/include -I../src/net -I../src/programs/lib

TCP_SRCS = test_tcp.c ../src/net/tcp.c

REGEX_SRCS = test_regex.c ../src/programs/lib/regex.c

all: $(TEST) test_netmsg test_tcp test_regex

$(TEST): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

test_netmsg: $(NETMSG_SRCS) ../src/net/dhcpmsg.h ../src/programs/lib/dnsmsg.h
	$(CC) $(NETMSG_CFLAGS) -o $@ $(NETMSG_SRCS)

test_tcp: $(TCP_SRCS) ../src/net/tcp.h
	$(CC) $(NETMSG_CFLAGS) -o $@ $(TCP_SRCS)

test_regex: $(REGEX_SRCS) ../src/programs/lib/regex.h regex_cases.h
	$(CC) $(NETMSG_CFLAGS) -I. -o $@ $(REGEX_SRCS)

run: $(TEST) test_netmsg test_tcp test_regex
	./$(TEST) || qemu-i386 ./$(TEST)
	./test_netmsg
	./test_tcp
	./test_regex
	@if command -v fsck.fat >/dev/null; then \
		fsck.fat -n test_disk.img >/dev/null && echo "fsck.fat: disk image is clean"; \
	else echo "fsck.fat not installed, skipping the image check"; fi

clean:
	rm -f $(TEST) test_netmsg test_tcp test_regex test_disk.img
