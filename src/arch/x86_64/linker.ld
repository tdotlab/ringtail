/* Kernel layout, x86_64. Loaded at 1 MB by the multiboot loader, which
   enters 32-bit code in .text (boot.asm) that switches to long mode.
   Text and read-only data come first and are page aligned, so paging
   can map them read-only (_ro_start to _ro_end), and only text
   executable (up to _text_end). */
ENTRY(_start)

SECTIONS {
    . = 1M;
    _kernel_start = .;

    _ro_start = .;
    .text ALIGN(4K) : {
        KEEP(*(.multiboot))
        *(.text .text.*)
    }
    . = ALIGN(4K);
    _text_end = .;
    .rodata ALIGN(4K) : {
        *(.rodata .rodata.*)
    }
    . = ALIGN(4K);
    _ro_end = .;

    .data ALIGN(4K) : {
        *(.data .data.*)
    }
    .bss ALIGN(4K) : {
        _bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        . = ALIGN(4);
        _bss_end = .;
    }
    . = ALIGN(4K);
    _kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr .comment .note .note.*)
    }
}
