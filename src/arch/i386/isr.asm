; Interrupt entry. Every vector gets a tiny stub that pushes a
; dummy error code (when the CPU does not push one) and its vector
; number, so all of them arrive at isr_common with the same frame
; layout. isr_common saves the rest, switches to kernel data
; segments, and hands a pointer to the frame (regs_t, see
; interrupt.h) to interrupt_dispatch.
;
; The segment reload matters for security as much as for function:
; on entry from ring 3, DS/ES/FS/GS still hold whatever the user
; left in them. The kernel must never run on user-chosen segments.

section .text

extern interrupt_dispatch

%macro ISR_NOERR 1
isr%1:
    push dword 0
    push dword %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
isr%1:
    push dword %1
    jmp isr_common
%endmacro

; CPU exceptions. 8, 10-14, 17, 21, 29 and 30 push an error code.
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31
; Hardware IRQs 0-15, remapped to vectors 32-47.
%assign v 32
%rep 16
ISR_NOERR v
%assign v v+1
%endrep
; System call.
ISR_NOERR 128

isr_common:
    pusha
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld
    push esp                ; regs_t* for the C handler
    call interrupt_dispatch
    add esp, 4
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8              ; vector and error code
    iret

section .rodata
global isr_stub_table
isr_stub_table:
%assign v 0
%rep 48
    dd isr %+ v
%assign v v+1
%endrep
global isr_syscall_stub
isr_syscall_stub:
    dd isr128

section .text

; idt_load(ptr)
global idt_load
idt_load:
    mov eax, [esp+4]
    lidt [eax]
    ret

; gdt_load(ptr): install the GDT and reload every segment register.
; CS needs a far jump; the task register is loaded with the TSS so
; the CPU knows which stack to use for traps from ring 3.
global gdt_load
gdt_load:
    mov eax, [esp+4]
    lgdt [eax]
    jmp 0x08:.reload_cs
.reload_cs:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov ax, 0x28
    ltr ax
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
