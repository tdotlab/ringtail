# Host tests: the kernel heap and FAT16 code built as a 32-bit Linux
# program against a RAM disk; the DHCP, DNS and SNTP message code (the parts
# that read bytes off the wire), the TCP engine (run against a
# simulated network), the regular-expression engine grep uses (run
# against GNU grep's answers), diff's core (against GNU diff's), base64's (against GNU base64's), the checksum-list
# parser of sha256 -c (against GNU sha256sum's), the
# cryptography (against the RFCs' and Project Wycheproof's vectors) and
# the TLS client (against recorded conversations with a scripted
# server and with OpenSSL), under the address and undefined-behaviour
# sanitizers. Needs gcc -m32 (gcc-multilib). The disk image the run
# leaves behind is checked with fsck.fat when it is installed.
CC = gcc
CFLAGS = -m32 -std=gnu11 -O1 -g -Wall -Wextra -fno-builtin -I../src/include
TEST = test_heap_diskfs
SRCS = test_heap_diskfs.c ../src/mm/heap.c ../src/fs/diskfs.c ../src/lib/string.c

NETMSG_SRCS = test_netmsg.c ../src/net/dhcpmsg.c ../src/programs/lib/dnsmsg.c ../src/programs/lib/ntpmsg.c ../src/lib/civil.c
NETMSG_CFLAGS = -m32 -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
	-I../src/include -I../src/net -I../src/programs/lib

TCP_SRCS = test_tcp.c ../src/net/tcp.c

REGEX_SRCS = test_regex.c ../src/programs/lib/regex.c

DIFF_SRCS = test_diff.c ../src/programs/lib/diffcore.c

B64_SRCS = test_b64.c ../src/programs/lib/b64.c

SHASUM_SRCS = test_shasum.c ../src/programs/lib/shasum.c

# The cryptography is built -O2, as it is for the OS: the sanitizers
# then check the code that ships, and the tests take seconds.
CRYPTO_LIB = ../src/lib/sha256.c ../src/lib/chacha20.c ../src/programs/lib/chachapoly.c ../src/programs/lib/x25519.c ../src/programs/lib/aesgcm.c
CRYPTO_HDRS = ../src/include/sha256.h ../src/include/chacha20.h ../src/include/ct.h \
	../src/programs/lib/chachapoly.h ../src/programs/lib/x25519.h ../src/programs/lib/aesgcm.h
CRYPTO_CFLAGS = $(NETMSG_CFLAGS) -O2 -I.

# test_tls replays each conversation thousands of times; --wrap lets it
# remember the key exchange's answers in place of working them out anew.
TLS_SRCS = test_tls.c ../src/programs/lib/tls.c $(CRYPTO_LIB)

SIG_LIB = ../src/programs/lib/sig.c ../src/programs/lib/sha512.c ../src/lib/sha256.c
SIG_HDRS = ../src/programs/lib/sig.h ../src/programs/lib/sha512.h ../src/programs/lib/der.h
X509_SRCS = test_x509.c ../src/programs/lib/x509.c $(SIG_LIB)

HOST_TESTS = $(TEST) test_netmsg test_tcp test_regex test_diff test_b64 test_shasum test_crypto test_tls test_sig test_x509

all: $(HOST_TESTS)

$(TEST): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

test_netmsg: $(NETMSG_SRCS) ../src/net/dhcpmsg.h ../src/programs/lib/dnsmsg.h ../src/programs/lib/ntpmsg.h ../src/include/civil.h
	$(CC) $(NETMSG_CFLAGS) -o $@ $(NETMSG_SRCS)

test_tcp: $(TCP_SRCS) ../src/net/tcp.h
	$(CC) $(NETMSG_CFLAGS) -o $@ $(TCP_SRCS)

test_regex: $(REGEX_SRCS) ../src/programs/lib/regex.h regex_cases.h capture_cases.h
	$(CC) $(NETMSG_CFLAGS) -I. -o $@ $(REGEX_SRCS)

test_diff: $(DIFF_SRCS) ../src/programs/lib/diffcore.h diff_cases.h
	$(CC) $(NETMSG_CFLAGS) -I. -o $@ $(DIFF_SRCS)

test_b64: $(B64_SRCS) ../src/programs/lib/b64.h b64_cases.h
	$(CC) $(NETMSG_CFLAGS) -I. -o $@ $(B64_SRCS)

test_shasum: $(SHASUM_SRCS) ../src/programs/lib/shasum.h shasum_cases.h
	$(CC) $(NETMSG_CFLAGS) -I. -o $@ $(SHASUM_SRCS)

test_crypto: test_crypto.c $(CRYPTO_LIB) $(CRYPTO_HDRS) crypto_cases.h
	$(CC) $(CRYPTO_CFLAGS) -o $@ test_crypto.c $(CRYPTO_LIB)

test_tls: $(TLS_SRCS) $(CRYPTO_HDRS) ../src/programs/lib/tls.h tls_cases.h
	$(CC) $(CRYPTO_CFLAGS) -Wl,--wrap=x25519 -Wl,--wrap=x25519_public -o $@ $(TLS_SRCS)

test_sig: test_sig.c $(SIG_LIB) $(SIG_HDRS) sig_cases.h
	$(CC) $(CRYPTO_CFLAGS) -o $@ test_sig.c $(SIG_LIB)

test_x509: $(X509_SRCS) $(SIG_HDRS) ../src/programs/lib/x509.h ../src/programs/lib/tls.h x509_cases.h
	$(CC) $(CRYPTO_CFLAGS) -o $@ $(X509_SRCS)

run: $(HOST_TESTS)
	./$(TEST) || qemu-i386 ./$(TEST)
	./test_netmsg
	./test_tcp
	./test_regex
	./test_diff
	./test_b64
	./test_shasum
	./test_crypto
	./test_tls
	./test_sig
	./test_x509
	@if command -v fsck.fat >/dev/null; then \
		fsck.fat -n test_disk.img >/dev/null && echo "fsck.fat: disk image is clean"; \
	else echo "fsck.fat not installed, skipping the image check"; fi

clean:
	rm -f $(HOST_TESTS) test_disk.img
