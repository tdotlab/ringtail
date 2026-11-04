/* User programs: static ELF executables linked at the start of user
   space. Text, read-only data and data get their own pages, so the
   loader can map text and rodata read-only. */
ENTRY(_start)

PHDRS {
    text   PT_LOAD FLAGS(5);  /* r-x */
    rodata PT_LOAD FLAGS(4);  /* r-- */
    data   PT_LOAD FLAGS(6);  /* rw- */
}

SECTIONS {
    . = 0x40000000;
    .text : { *(.text .text.*) } :text
    . = ALIGN(4K);
    .rodata : { *(.rodata .rodata.*) } :rodata
    . = ALIGN(4K);
    .data : { *(.data .data.*) } :data
    .bss : { *(.bss .bss.*) *(COMMON) } :data
    /DISCARD/ : { *(.eh_frame .eh_frame_hdr .comment .note .note.*) }
}
