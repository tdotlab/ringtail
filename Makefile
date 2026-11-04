CC = i686-elf-gcc
AS = nasm
LD = i686-elf-ld

CFLAGS = -ffreestanding -O2 -Wall -Wextra -nostdlib -lgcc -fno-builtin -fno-stack-protector -DRINGTAIL_KERNEL
ASFLAGS = -f elf32

all: ringtail.iso

OBJS = src/boot/boot.o src/kernel/isr.o src/kernel/kernel.o src/kernel/idt.o src/kernel/task.o src/kernel/task_asm.o src/kernel/syscall.o src/kernel/faultlog.o src/kernel/loader.o src/drivers/keyboard.o src/drivers/mouse.o src/drivers/vga_gfx.o src/shell/shell.o src/mm/heap.o src/mm/paging.o src/fs/diskfs.o src/drivers/ata.o src/drivers/timer.o

src/boot/boot.o: src/boot/boot.asm
	$(AS) $(ASFLAGS) src/boot/boot.asm -o src/boot/boot.o

src/kernel/isr.o: src/kernel/isr.asm
	$(AS) $(ASFLAGS) src/kernel/isr.asm -o src/kernel/isr.o

src/kernel/task_asm.o: src/kernel/task.asm
	$(AS) $(ASFLAGS) src/kernel/task.asm -o src/kernel/task_asm.o

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

ringtail.bin: $(OBJS) src/boot/linker.ld
	$(LD) -T src/boot/linker.ld -o ringtail.bin $(OBJS)

ringtail.iso: ringtail.bin
	mkdir -p isodir/boot/grub
	cp ringtail.bin isodir/boot/ringtail.bin
	printf 'set timeout=0\nset default=0\nmenuentry "RingTail" {\n  multiboot /boot/ringtail.bin\n  boot\n}\n' > isodir/boot/grub/grub.cfg
	grub-mkrescue -o ringtail.iso isodir

run: ringtail.iso
	qemu-system-i386 -cdrom ringtail.iso

clean:
	rm -rf src/*/*.o *.bin *.iso isodir
