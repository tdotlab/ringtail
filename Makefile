# RingTail build. Everything goes into build/.
#   make          kernel, programs, disk image and bootable ISO
#   make run      boot in QEMU (screen + serial on this terminal)
#   make test     host unit tests, then the QEMU integration tests
#   make clean

BUILD := build

# Prefer a real cross compiler; otherwise a multilib host gcc works,
# with position-independent code turned off.
ifneq ($(shell command -v i686-elf-gcc 2>/dev/null),)
CC := i686-elf-gcc
ARCH :=
else
CC := gcc
ARCH := -m32 -fno-pie -fno-pic
endif
AS := nasm
PYTHON := python3
QEMU := qemu-system-i386

COMMON := $(ARCH) -std=gnu11 -ffreestanding -O2 -Wall -Wextra -fno-builtin \
          -fno-stack-protector -fno-omit-frame-pointer -march=i686 \
          -fno-asynchronous-unwind-tables -MMD -MP
KCFLAGS := $(COMMON) -mgeneral-regs-only -DRINGTAIL_KERNEL -Isrc/include
UCFLAGS := $(COMMON) -mno-sse -mno-mmx -Isrc/include -Isrc/programs/lib
LDFLAGS := $(ARCH) -nostdlib -static -no-pie -Wl,--build-id=none
ASFLAGS := -f elf32

KERNEL_C := $(wildcard src/kernel/*.c src/drivers/*.c src/mm/*.c src/fs/*.c src/lib/*.c src/shell/*.c)
KERNEL_ASM := src/boot/boot.asm src/kernel/isr.asm src/kernel/switch.asm
KERNEL_OBJS := $(KERNEL_C:%.c=$(BUILD)/k/%.o) $(KERNEL_ASM:%.asm=$(BUILD)/k/%.o)
KERNEL := $(BUILD)/ringtail.bin

ULIB_C := $(wildcard src/programs/lib/*.c) src/lib/string.c src/lib/printf.c
ULIB_OBJS := $(ULIB_C:%.c=$(BUILD)/u/%.o)
PROGRAMS := $(notdir $(basename $(wildcard src/programs/*.c)))
PROGRAM_BINS := $(PROGRAMS:%=$(BUILD)/programs/%)

DISK := $(BUILD)/disk.img
ISO := $(BUILD)/ringtail.iso

.PHONY: all kernel programs disk iso run run-serial test test-host test-qemu clean
all: iso disk

kernel: $(KERNEL)
programs: $(PROGRAM_BINS)
disk: $(DISK)
iso: $(ISO)

$(BUILD)/k/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/k/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(KERNEL): $(KERNEL_OBJS) src/boot/linker.ld
	$(CC) $(LDFLAGS) -T src/boot/linker.ld -o $@ $(KERNEL_OBJS) -lgcc

$(BUILD)/u/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/programs/%: $(BUILD)/u/src/programs/%.o $(ULIB_OBJS) src/programs/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -T src/programs/user.ld -o $@ $< $(ULIB_OBJS) -lgcc

# Disk names are the program names in uppercase (DOS 8.3).
$(DISK): $(PROGRAM_BINS) disk/welcome.txt tools/mkdisk.py
	$(PYTHON) tools/mkdisk.py -o $@ WELCOME.TXT:disk/welcome.txt \
	    $(foreach p,$(PROGRAMS),$(shell echo $(p) | tr a-z A-Z):$(BUILD)/programs/$(p))

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
	$(MAKE) -C tests run

test-qemu: all
	$(PYTHON) tests/qemu_test.py --iso $(ISO) --disk $(DISK)

clean:
	rm -rf $(BUILD)
	$(MAKE) -C tests clean

-include $(KERNEL_OBJS:.o=.d) $(ULIB_OBJS:.o=.d)
