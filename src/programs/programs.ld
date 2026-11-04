/* Linker script for RingTail user programs. Every program links
   at the same virtual address, 0x08000000, because every program
   runs in its own address space: the loader maps each program's
   pages at this address in its own page directory, so two
   programs can both live at 0x08000000 at once. .text._start
   comes first on purpose: _start carries that section attribute
   in each program file, and the loader has no header to read an
   entry offset from, so offset 0 of the flat binary must be the
   entry point. The output is a flat binary (objcopy -O binary),
   not an ELF: just code and data, in address order, ready for
   the loader to copy into frames. */
ENTRY(_start)
SECTIONS {
  . = 0x08000000;
  .text : {
    *(.text._start)
    *(.text)
    *(.text.*)
  }
  .rodata : {
    *(.rodata)
    *(.rodata.*)
  }
  .data : {
    *(.data)
    *(.data.*)
  }
  .bss : {
    *(.bss)
    *(.bss.*)
    *(COMMON)
  }
}
