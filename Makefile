CC = i686-elf-gcc
AS = nasm
LD = i686-elf-ld

CFLAGS = -ffreestanding -O2 -Wall -Wextra -nostdlib -lgcc
ASFLAGS = -f elf32

all: myos.iso

OBJS = boot.o isr.o kernel.o idt.o keyboard.o shell.o heap.o ramfs.o timer.o

boot.o: boot.asm
	$(AS) $(ASFLAGS) boot.asm -o boot.o

isr.o: isr.asm
	$(AS) $(ASFLAGS) isr.asm -o isr.o

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

myos.bin: $(OBJS) linker.ld
	$(LD) -T linker.ld -o myos.bin $(OBJS)

myos.iso: myos.bin
	mkdir -p isodir/boot/grub
	cp myos.bin isodir/boot/myos.bin
	echo 'set timeout=0
set default=0
menuentry "MyOS" {
  multiboot /boot/myos.bin
  boot
}' > isodir/boot/grub/grub.cfg
	grub-mkrescue -o myos.iso isodir

run: myos.iso
	qemu-system-i386 -cdrom myos.iso

clean:
	rm -rf *.o *.bin *.iso isodir
