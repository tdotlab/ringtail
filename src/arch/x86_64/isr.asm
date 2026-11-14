; Interrupt entry, x86_64. Every vector gets a tiny stub that pushes a
; dummy error code (when the CPU does not push one) and its vector
; number, so all of them arrive at isr_common with the same frame.
; isr_common saves the general registers and hands a pointer to the
; frame (regs_t, see arch/regs.h) to interrupt_dispatch.
;
; In 64-bit mode the CPU always pushes SS and RSP and aligns the stack
; to 16 bytes first, so the frame looks the same whether the trap came
; from ring 0 or ring 3, and rsp is 16-byte aligned at the call. The
; data segment registers play no part in addressing here, so unlike on
; i386 there is nothing of the user's to replace on the way in.

section .text

extern interrupt_dispatch

%macro ISR_NOERR 1
isr%1:
    push 0
    push %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
isr%1:
    push %1
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
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    cld
    mov rdi, rsp            ; regs_t* for the C handler
    call interrupt_dispatch
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16             ; vector and error code
    iretq

section .rodata
global isr_stub_table
isr_stub_table:
%assign v 0
%rep 48
    dq isr %+ v
%assign v v+1
%endrep
global isr_syscall_stub
isr_syscall_stub:
    dq isr128

section .text

; idt_load(ptr)
global idt_load
idt_load:
    lidt [rdi]
    ret

; gdt_load(ptr): install the GDT, reload the segment registers and load
; the task register with the TSS, so the CPU knows which stack to use
; for traps from ring 3. CS takes a far return.
global gdt_load
gdt_load:
    lgdt [rdi]
    push 0x08
    lea rax, [rel .reload_cs]
    push rax
    o64 retf
.reload_cs:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax
    mov ax, 0x28
    ltr ax
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
