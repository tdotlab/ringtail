; Part 1 - Multiboot bootloader stub
; This is the FIRST code the CPU runs after GRUB/Limine loads us
; CPU starts in 32-bit protected mode, we set up a stack and jump to C

MAGIC equ 0x1BADB002
FLAGS equ 0x00
CHECKSUM equ -(MAGIC + FLAGS)

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM

section .text
global _start
extern kernel_main

_start:
    ; set up stack (stack grows down, so point to top)
    mov esp, stack_top

    ; call C kernel - this is where we leave assembly world
    call kernel_main

    ; if kernel returns (it shouldn't), halt forever
    cli
.hang:
    hlt
    jmp .hang

section .bss
align 16
stack_bottom:
    resb 16384 ; 16KB stack - tiny but enough for TUI
stack_top:
