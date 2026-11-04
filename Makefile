CC = i686-elf-gcc
AS = nasm
LD = i686-elf-ld

CFLAGS = -ffreestanding -O2 -Wall -Wextra -nostdlib -lgcc -fno-builtin -fno-stack-protector
ASFLAGS = -f elf32

all: myos.iso

OBJS = src/boot/boot.o src/kernel/isr.o src/kernel/kernel.o src/kernel/idt.o src/kernel/task.o src/kernel/task_asm.o src/drivers/keyboard.o src/shell/shell.o src/mm/heap.o src/fs/ramfs.o src/drivers/timer.o

src/boot/boot.o: src/boot/boot.asm
	$(AS) $(ASFLAGS) src/boot/boot.asm -o src/boot/boot.o

src/kernel/isr.o: src/kernel/isr.asm
	$(AS) $(ASFLAGS) src/kernel/isr.asm -o src/kernel/isr.o

src/kernel/task_asm.o: src/kernel/task.asm
	$(AS) $(ASFLAGS) src/kernel/task.asm -o src/kernel/task_asm.o

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

myos.bin: $(OBJS) src/boot/linker.ld
	$(LD) -T src/boot/linker.ld -o myos.bin $(OBJS)

myos.iso: myos.bin
	mkdir -p isodir/boot/grub
	cp myos.bin isodir/boot/myos.bin
	printf 'set timeout=0\nset default=0\nmenuentry "MyOS" {\n  multiboot /boot/myos.bin\n  boot\n}\n' > isodir/boot/grub/grub.cfg
	grub-mkrescue -o myos.iso isodir

run: myos.iso
	qemu-system-i386 -cdrom myos.iso

clean:
	rm -rf src/*/*.o *.bin *.iso isodir
