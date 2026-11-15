; Where another CPU starts. A startup IPI (smp.c) sets it running in
; 16-bit real mode at CS:IP = 0x0800:0, which is physical 0x8000, so
; smp.c copies this code there first; it is assembled here for that
; address (TRAMPOLINE), which is why every address in it is worked
; out from TRAMPOLINE and the label's offset into the code.
;
; It does what the boot stub (boot.asm) did, in one go: protected
; mode, PAE, the page tables, long mode with NX if the boot CPU has
; it, then paging, and so 64-bit code. The page tables are made for
; it (smp.c): the kernel's upper half, plus the first 2 MB at 0, where
; this code runs until it jumps to the kernel's own address. CR0 comes
; from the boot CPU, so caching, write protection and the FPU bits are
; switched on as there. The C entry (ap_main in smp.c) does the rest,
; the kernel's own page tables first.
;
; smp.c fills in the block at offset 8 (trampoline_t) before each start.

TRAMPOLINE equ 0x8000

section .rodata
global ap_trampoline
global ap_trampoline_end

bits 16
ap_trampoline:
    jmp short start16
    times 8 - ($ - ap_trampoline) db 0
t_cr3:   dd 0                   ; the page tables, below 4 GB
t_cr4:   dd 0                   ; PAE, and PGE if the boot CPU has it
t_cr0:   dd 0                   ; the boot CPU's
t_efer:  dd 0                   ; LME, and NXE if the boot CPU has it
t_stack: dq 0                   ; the top of this CPU's first stack
t_entry: dq 0                   ; ap_main
t_arg:   dq 0                   ; its argument: this CPU's index

; Flat 32-bit code and data, and 64-bit code.
align 8
gdt:
    dq 0
    dq 0x00CF9A000000FFFF       ; 0x08: 32-bit code
    dq 0x00CF92000000FFFF       ; 0x10: data
    dq 0x00AF9A000000FFFF       ; 0x18: 64-bit code
gdt_end:
gdtr:
    dw gdt_end - gdt - 1
    dd TRAMPOLINE + (gdt - ap_trampoline)

start16:
    cli
    cld
    mov ax, cs                  ; 0x0800: offsets into this code
    mov ds, ax
    o32 lgdt [gdtr - ap_trampoline]
    mov eax, cr0
    or eax, 1                   ; PE
    mov cr0, eax
    jmp dword 0x08:TRAMPOLINE + (start32 - ap_trampoline)

bits 32
start32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov ebx, TRAMPOLINE
    mov eax, [ebx + (t_cr4 - ap_trampoline)]
    mov cr4, eax
    mov eax, [ebx + (t_cr3 - ap_trampoline)]
    mov cr3, eax
    mov ecx, 0xC0000080         ; EFER
    rdmsr
    or eax, [ebx + (t_efer - ap_trampoline)]
    wrmsr
    mov eax, [ebx + (t_cr0 - ap_trampoline)]
    mov cr0, eax                ; paging on: long mode, in compatibility mode
    jmp 0x18:TRAMPOLINE + (start64 - ap_trampoline)

bits 64
start64:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax
    mov ebx, TRAMPOLINE
    mov rsp, [rbx + (t_stack - ap_trampoline)]
    mov rdi, [rbx + (t_arg - ap_trampoline)]
    mov rax, [rbx + (t_entry - ap_trampoline)]
    xor ebp, ebp                ; the end of the frame-pointer chain
    call rax                    ; does not return
.hang:
    cli
    hlt
    jmp .hang
ap_trampoline_end:

section .note.GNU-stack noalloc noexec nowrite progbits
