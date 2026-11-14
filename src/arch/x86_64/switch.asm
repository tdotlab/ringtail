; Context switching, x86_64.
;
; switch_to(uintptr_t* save_sp, uintptr_t new_sp) saves the
; callee-saved registers on the current kernel stack, stores the
; stack pointer, loads the other task's, and pops its registers.
; Everything else a task needs (its interrupt frame, if it was
; preempted; its C call chain) is already on its own stack, so this
; is the whole switch. Interrupts are off throughout: the scheduler
; is the only caller and requires it.
;
; A new task's stack is prepared to look as if it had called
; switch_to itself (arch_task_stack in cpu.c): six zero registers,
; then a return address pointing at task_trampoline (task.c).

section .text

global switch_to
switch_to:                  ; rdi = save_sp, rsi = new_sp
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; enter_user(ip, sp): drop to ring 3 for the first time. The iretq
; frame carries the user code and stack selectors and RFLAGS with only
; IF set, so the program starts with interrupts on and IOPL 0 (no port
; access). General registers are cleared so no kernel values leak into
; the program.
global enter_user
enter_user:                 ; rdi = ip, rsi = sp
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax
    push 0x23               ; SS
    push rsi                ; RSP
    push 0x202              ; RFLAGS
    push 0x1B               ; CS
    push rdi                ; RIP
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    iretq

section .note.GNU-stack noalloc noexec nowrite progbits
