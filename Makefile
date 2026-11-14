# RingTail build. Everything goes into build/$(ARCH)/.
#   make          kernel, programs, disk image and bootable ISO
#   make run      boot in QEMU (screen + serial on this terminal)
#   make test     host unit tests, then the QEMU integration tests
#                 (on the test disk, which adds tests/programs)
#   make clean
# ARCH picks the machine: x86_64 (64-bit) or i386 (32-bit), as in
# make ARCH=x86_64 test.

ARCH ?= i386
BUILD := build/$(ARCH)

ifeq ($(ARCH),i386)
CROSS := i686-elf-gcc
HOST_BITS := 32
ARCH_CFLAGS := -march=i686
KARCH_CFLAGS :=
# No SSE in programs: the kernel saves only the x87 state.
UARCH_CFLAGS := -mno-sse -mno-mmx
ARCH_LDFLAGS :=
ASFLAGS := -f elf32
QEMU := qemu-system-i386
else ifeq ($(ARCH),x86_64)
CROSS := x86_64-elf-gcc
HOST_BITS := 64
ARCH_CFLAGS :=
# The kernel takes interrupts on the stack it is running on, so the
# 128 bytes below rsp are not free for it to use.
KARCH_CFLAGS := -mno-red-zone
UARCH_CFLAGS :=
# Without this the linker aligns segments in the file to 2 MB, which
# moves the multiboot header out of the first 8 KB, where GRUB looks.
ARCH_LDFLAGS := -Wl,-z,max-page-size=0x1000
ASFLAGS := -f elf64
QEMU := qemu-system-x86_64
else
$(error ARCH is i386 or x86_64, not $(ARCH))
endif

# Prefer a real cross compiler; otherwise a multilib host gcc works,
# with position-independent code turned off.
ifneq ($(shell command -v $(CROSS) 2>/dev/null),)
CC := $(CROSS)
TARGET :=
else
CC := gcc
TARGET := -m$(HOST_BITS) -fno-pie -fno-pic
endif
AS := nasm
PYTHON := python3

ARCHDIR := src/arch/$(ARCH)
COMMON := $(TARGET) $(ARCH_CFLAGS) -std=gnu11 -ffreestanding -O2 -Wall -Wextra -fno-builtin \
          -fno-stack-protector -fno-omit-frame-pointer \
          -fno-asynchronous-unwind-tables -MMD -MP
KCFLAGS := $(COMMON) $(KARCH_CFLAGS) -mgeneral-regs-only -DRINGTAIL_KERNEL -Isrc/include -I$(ARCHDIR)/include \
           -I$(BUILD)/gen
UCFLAGS := $(COMMON) $(UARCH_CFLAGS) -Isrc/include -Isrc/programs/lib
LDFLAGS := $(TARGET) $(ARCH_LDFLAGS) -nostdlib -static -no-pie -Wl,--build-id=none

KERNEL_C := $(wildcard src/kernel/*.c src/drivers/*.c src/mm/*.c src/fs/*.c src/net/*.c src/lib/*.c $(ARCHDIR)/*.c)
KERNEL_ASM := $(wildcard $(ARCHDIR)/*.asm)
KERNEL_OBJS := $(KERNEL_C:%.c=$(BUILD)/k/%.o) $(KERNEL_ASM:%.asm=$(BUILD)/k/%.o)
KERNEL := $(BUILD)/ringtail.bin

# The user library is an archive, so a program gets only the parts it
# uses (the resolver stays out of hello). crt0 goes in every program.
ULIB_C := $(wildcard src/programs/lib/*.c) src/lib/string.c src/lib/printf.c src/lib/sha256.c src/lib/chacha20.c src/lib/civil.c
ULIB_OBJS := $(ULIB_C:%.c=$(BUILD)/u/%.o)
CRT0 := $(BUILD)/u/src/programs/lib/crt0.o
ULIB := $(BUILD)/u/libu.a
# Programs live one folder down (apps, files, text, net, util); lib/ is
# the library, not programs. The binaries all go flat in build/programs,
# since the disk has one directory and names must fit 8.3.
PROGRAM_SRCS := $(filter-out src/programs/lib/%,$(wildcard src/programs/*/*.c))
PROGRAMS := $(notdir $(basename $(PROGRAM_SRCS)))
ifneq ($(words $(PROGRAMS)),$(words $(sort $(PROGRAMS))))
DUPLICATES := $(strip $(foreach n,$(sort $(PROGRAMS)),$(if $(filter-out 1,$(words $(filter $(n),$(PROGRAMS)))),$(n))))
$(error more than one program is called $(DUPLICATES) (they would share one name on the disk): $(filter %/$(firstword $(DUPLICATES)).c,$(PROGRAM_SRCS)))
endif
PROGRAM_BINS := $(PROGRAMS:%=$(BUILD)/programs/%)
# Programs that exist to test the kernel. They go on a separate test
# disk with everything else, not on the disk you boot.
TEST_PROGRAMS := $(notdir $(basename $(wildcard tests/programs/*.c)))
TEST_PROGRAM_BINS := $(TEST_PROGRAMS:%=$(BUILD)/test-programs/%)

DISK := $(BUILD)/disk.img
TEST_DISK := $(BUILD)/test-disk.img
ISO := $(BUILD)/ringtail.iso

.PHONY: all kernel programs disk test-disk iso run run-serial test test-host test-qemu clean FORCE
all: iso disk

kernel: $(KERNEL)
programs: $(PROGRAM_BINS)
disk: $(DISK)
test-disk: $(TEST_DISK)
iso: $(ISO)

# The kernel will not set its clock earlier than this: the time of the
# last commit (or SOURCE_DATE_EPOCH, or now, outside a git checkout).
# The header is rewritten only when the value changes, so an unchanged
# tree does not rebuild.
TIME_FLOOR := $(or $(SOURCE_DATE_EPOCH),$(shell git log -1 --format=%ct 2>/dev/null),$(shell date +%s))
$(BUILD)/gen/timefloor.h: FORCE
	@mkdir -p $(dir $@)
	@echo '#define RINGTAIL_TIME_FLOOR $(TIME_FLOOR)u' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@
$(BUILD)/k/src/kernel/syscall.o: $(BUILD)/gen/timefloor.h

$(BUILD)/k/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/k/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(KERNEL): $(KERNEL_OBJS) $(ARCHDIR)/linker.ld
	$(CC) $(LDFLAGS) -T $(ARCHDIR)/linker.ld -o $@ $(KERNEL_OBJS) -lgcc

$(BUILD)/u/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

$(ULIB): $(filter-out $(CRT0),$(ULIB_OBJS))
	rm -f $@
	ar rcs $@ $^

# One link rule for each program: its object is under its folder's mirror in
# build/u, but the binary goes flat in build/programs.
define PROGRAM_RULE
$(BUILD)/programs/$(notdir $(basename $(1))): $(BUILD)/u/$(1:.c=.o) $(CRT0) $(ULIB) src/programs/user.ld
	@mkdir -p $$(dir $$@)
	$$(CC) $$(LDFLAGS) -T src/programs/user.ld -o $$@ $$< $$(CRT0) $$(ULIB) -lgcc
endef
$(foreach s,$(PROGRAM_SRCS),$(eval $(call PROGRAM_RULE,$(s))))

$(BUILD)/test-programs/%: $(BUILD)/u/tests/programs/%.o $(CRT0) $(ULIB) src/programs/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -T src/programs/user.ld -o $@ $< $(CRT0) $(ULIB) -lgcc

# Disk names are the program names in uppercase (DOS 8.3).
upper = $(shell echo $(1) | tr a-z A-Z)

$(DISK): $(PROGRAM_BINS) disk/welcome.txt disk/roots.pem tools/mkdisk.py
	$(PYTHON) tools/mkdisk.py -o $@ WELCOME.TXT:disk/welcome.txt ROOTS.PEM:disk/roots.pem \
	    $(foreach p,$(PROGRAMS),$(call upper,$(p)):$(BUILD)/programs/$(p))

$(TEST_DISK): $(PROGRAM_BINS) $(TEST_PROGRAM_BINS) disk/welcome.txt disk/roots.pem tools/mkdisk.py
	$(PYTHON) tools/mkdisk.py -o $@ WELCOME.TXT:disk/welcome.txt ROOTS.PEM:disk/roots.pem \
	    $(foreach p,$(PROGRAMS),$(call upper,$(p)):$(BUILD)/programs/$(p)) \
	    $(foreach p,$(TEST_PROGRAMS),$(call upper,$(p)):$(BUILD)/test-programs/$(p))

$(ISO): $(KERNEL)
	@mkdir -p $(BUILD)/isodir/boot/grub
	cp $(KERNEL) $(BUILD)/isodir/boot/ringtail.bin
	printf 'set timeout=0\nset default=0\nmenuentry "RingTail" {\n  multiboot /boot/ringtail.bin\n  boot\n}\n' > $(BUILD)/isodir/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/isodir 2>/dev/null

QEMU_ARGS := -cdrom $(ISO) -drive file=$(DISK),format=raw,if=ide,index=0 -boot d -m 64

run: all
	$(QEMU) $(QEMU_ARGS) -serial stdio

# No window: the console is the serial port.
run-serial: all
	$(QEMU) $(QEMU_ARGS) -display none -serial stdio

test: test-host test-qemu

test-host:
	$(MAKE) -C tests run BITS=$(HOST_BITS)

test-qemu: all $(TEST_DISK)
	$(PYTHON) tests/qemu_test.py --qemu $(QEMU) --iso $(ISO) --disk $(TEST_DISK)

clean:
	rm -rf build
	$(MAKE) -C tests clean

-include $(KERNEL_OBJS:.o=.d) $(ULIB_OBJS:.o=.d)
