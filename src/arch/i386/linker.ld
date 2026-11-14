/* Kernel layout. Loaded at 1 MB by the multiboot loader.
   Text and read-only data come first and are page aligned, so paging
   can map them read-only (_ro_start to _ro_end). */
ENTRY(_start)

SECTIONS {
    . = 1M;
    _kernel_start = .;

    _ro_start = .;
    .text ALIGN(4K) : {
        KEEP(*(.multiboot))
        *(.text .text.*)
    }
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
