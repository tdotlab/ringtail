ENTRY(_start)

SECTIONS {
    /* Load kernel at 1MB - standard for Multiboot */
    . = 1M;

    .text BLOCK(4K) : ALIGN(4K) {
        *(.multiboot)
        *(.text)
    }

    .rodata BLOCK(4K) : ALIGN(4K) {
        *(.rodata)
    }

    .data BLOCK(4K) : ALIGN(4K) {
        *(.data)
    }

    .bss BLOCK(4K) : ALIGN(4K) {
        _bss_start = .;
        *(COMMON)
        *(.bss)
        _bss_end = .;
    }
    _kernel_end = .;
}
