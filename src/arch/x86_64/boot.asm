; x86_64 boot. GRUB loads the kernel as a Multiboot (version 1) ELF and
; enters _start in 32-bit protected mode with paging off, eax = the
; multiboot magic and ebx = the physical address of the info block.
;
; This code checks that the CPU can run 64-bit code, clears .bss,
; builds page tables that identity map the first 4 GB with 2 MB pages,
; turns on PAE and long mode and paging, and far-jumps into 64-bit code,
; which calls kernel_main(magic, info). paging_init later replaces the
; boot tables with the real ones (4 KB pages, read-only kernel text, no
; page 0); these only have to last until then.

MAGIC equ 0x1BADB002
FLAGS equ 0x03              ; page-aligned modules, and the memory map
CHECKSUM equ -(MAGIC + FLAGS)

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM

section .text
bits 32
global _start
global stack_top
extern kernel_main
extern _bss_start
extern _bss_end

_start:
    cli
    cmp eax, 0x2BADB002
    jne bad_boot
    ; Keep the magic in esi and the info pointer in ebp: cpuid clobbers
    ; ebx, and the .bss clear uses eax, ecx and edi.
    mov esi, eax
    mov ebp, ebx
    mov esp, stack_top

    ; Long mode needs CPUID leaf 0x80000001, and its LM bit.
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb no_long_mode
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz no_long_mode

    mov edi, _bss_start
    mov ecx, _bss_end
    sub ecx, edi
    shr ecx, 2
    xor eax, eax
    cld
    rep stosd

    ; PML4[0] -> PDPT; PDPT[0..3] -> four page directories; each of
    ; their 2048 entries maps 2 MB, present and writable.
    mov eax, boot_pdpt
    or eax, 3
    mov [boot_pml4], eax
    xor ecx, ecx
.pdpt:
    mov eax, ecx
    shl eax, 12
    add eax, boot_pd
    or eax, 3
    mov [boot_pdpt + ecx*8], eax
    inc ecx
    cmp ecx, 4
    jne .pdpt
    xor ecx, ecx
.pd:
    mov eax, ecx
    shl eax, 21
    or eax, 0x83            ; present, writable, 2 MB page
    mov [boot_pd + ecx*8], eax
    mov eax, ecx
    shr eax, 11             ; bits 32 and up of the address
    mov [boot_pd + ecx*8 + 4], eax
    inc ecx
    cmp ecx, 2048
    jne .pd

    mov eax, boot_pml4
    mov cr3, eax
    mov eax, cr4
    or eax, 1 << 5          ; PAE
    mov cr4, eax
    mov ecx, 0xC0000080     ; EFER
    rdmsr
    or eax, 1 << 8          ; LME
    wrmsr
    mov eax, cr0
    or eax, 1 << 31         ; PG: long mode is now active, in 32-bit compatibility mode
    mov cr0, eax

    lgdt [boot_gdt_ptr]
    jmp 0x08:long_entry

; A message in the top-left corner of the VGA text screen, then halt.
; esi points at the string, ah holds the colour.
show_and_halt:
    mov edi, 0xB8000
.next:
    lodsb
    test al, al
    jz .halt
    stosw
    jmp .next
.halt:
    cli
    hlt
    jmp .halt

bad_boot:
    mov esi, msg_bad_boot
    mov ah, 0x0C
    jmp show_and_halt

no_long_mode:
    mov esi, msg_no_long_mode
    mov ah, 0x0C
    jmp show_and_halt

bits 64
long_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax
    mov rsp, stack_top
    ; The upper halves of the registers are undefined after the switch;
    ; 32-bit moves clear them.
    mov edi, esi            ; magic
    mov esi, ebp            ; info
    xor ebp, ebp            ; the end of the frame-pointer chain
    call kernel_main
.hang:
    cli
    hlt
    jmp .hang

section .rodata
msg_bad_boot: db "RingTail: not loaded by a multiboot loader", 0
msg_no_long_mode: db "RingTail needs a 64-bit (x86-64) CPU", 0

; The GDT that gets the CPU into 64-bit mode. gdt_init loads the real
; one, with the same kernel selectors, later. Writable, since the CPU
; sets the accessed bits as it loads the segments.
section .data
align 8
boot_gdt:
    dq 0
    dq 0x00AF9A000000FFFF   ; 0x08: 64-bit code, ring 0
    dq 0x00CF92000000FFFF   ; 0x10: data, ring 0
boot_gdt_end:
boot_gdt_ptr:
    dw boot_gdt_end - boot_gdt - 1
    dd boot_gdt

section .bss
align 4096
boot_pml4: resb 4096
boot_pdpt: resb 4096
boot_pd:   resb 4 * 4096
align 16
stack_bottom:
    resb 16384
stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits
