/* Kernel layout, x86_64. The multiboot loader puts the image at 1 MB
   physical and enters 32-bit code with paging off, so the boot stub
   (.boot: the multiboot header, the code that gets into long mode, its
   GDT and page tables) is linked where it is loaded, at 1 MB.

   The rest of the kernel is linked at the top of the address space,
   KERNEL_VMA (-2 GB) plus its physical address, and loaded right after
   the stub (the AT() addresses). -mcmodel=kernel needs it in the top
   2 GB. Text and read-only data are page aligned, so paging can map
   them read-only (_ro_start to _ro_end), and only text executable (up
   to _text_end). Every symbol below but the stub's is a high address;
   virt_to_phys turns one into the physical address. */
ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;

SECTIONS {
    . = 1M;
    .boot : {
        KEEP(*(.multiboot))
        *(.boot)
        *(.boot.data)
    }
    .boot.bss (NOLOAD) : ALIGN(4K) {
        *(.boot.bss)
    }
    . = ALIGN(4K);

    _kernel_start = 1M + KERNEL_VMA;
    . += KERNEL_VMA;

    _ro_start = .;
    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA) {
        *(.text .text.*)
    }
    . = ALIGN(4K);
    _text_end = .;
    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA) {
        *(.rodata .rodata.*)
    }
    . = ALIGN(4K);
    _ro_end = .;

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA) {
        *(.data .data.*)
    }
    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA) {
        _bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        . = ALIGN(8);
        _bss_end = .;
    }
    . = ALIGN(4K);
    _kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr .comment .note .note.*)
    }
}
