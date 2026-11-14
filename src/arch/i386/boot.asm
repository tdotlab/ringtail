; RingTail - Boot
; v0.2.3: own GDT (no longer trusting GRUB's segment setup), a
; multiboot magic check, and explicit BSS clearing before C runs.
; GRUB leaves the CPU in 32-bit protected mode with eax = magic,
; ebx = multiboot info pointer.

MAGIC equ 0x1BADB002
; v0.10.4: FLAGS was 0x00, and GRUB in QEMU provided
; mem_upper anyway, which is why kernel_main could use
; it without asking. The full memory map is only
; guaranteed when requested, so now bit 1 (provide
; memory information) is set, which is what makes
; heap_init_from_map's mmap read a request, not a
; lucky accident. Bit 0 (align boot modules on page
; boundaries) is the conventional partner flag, harmless
; with no modules loaded, and the checksum recomputes
; automatically from MAGIC + FLAGS, so no manual
; checksum update is needed.
FLAGS equ 0x03
CHECKSUM equ -(MAGIC + FLAGS)

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM

section .text
global _start
global stack_top
extern kernel_main
extern _bss_start
extern _bss_end

_start:
    cli
    cmp eax, 0x2BADB002
    jne bad_boot

    ; Save the multiboot magic NOW: the segment reload below does
    ; mov ax, 0x10, which clobbers the low 16 bits of eax (the magic
    ; lives in eax). Saving after the reload saved 0x2BAD0010 and
    ; made kernel_main halt with "Bad multiboot magic". Found by
    ; actually booting in QEMU; no syntax check can catch this.
    mov esi, eax
    ; Load our own GDT so IDT segment 0x08 and data segment 0x10
    ; are valid whatever the loader left behind.
    lgdt [gdt_ptr]
    jmp 0x08:reload_cs
reload_cs:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, stack_top

    ; BSS clear: the loop uses edi, ecx and eax, but not esi
    ; (magic, saved above) or ebx (info pointer).
    mov edi, _bss_start
    mov ecx, _bss_end
    sub ecx, edi
    shr ecx, 2
    xor eax, eax
    cld
    rep stosd

    ; cdecl: push info, then magic, then call.
    push ebx
    push esi
    call kernel_main

    ; if kernel returns (it shouldn't), halt forever
    cli
.hang:
    hlt
    jmp .hang

bad_boot:
    ; Red 'B' in the top-left VGA cell, then halt.
    mov edi, 0xB8000
    mov word [edi], 0x0C42
    cli
.hang2:
    hlt
    jmp .hang2

section .data
align 4
gdt_start:
    dq 0 ; null descriptor
    ; code: base 0, limit 4 GB, execute/read, present, DPL0, 32-bit
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x9A
    db 0xCF
    db 0x00
    ; data: base 0, limit 4 GB, read/write, present, DPL0, 32-bit
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92
    db 0xCF
    db 0x00
gdt_end:
gdt_ptr:
    dw gdt_end - gdt_start - 1
    dd gdt_start

section .bss
align 16
stack_bottom:
    resb 16384 ; 16KB stack - tiny but enough for TUI
stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits
