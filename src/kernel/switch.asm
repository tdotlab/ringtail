; Context switching.
;
; switch_to(uint32_t* save_esp, uint32_t new_esp) saves the
; callee-saved registers on the current kernel stack, stores the
; stack pointer, loads the other task's, and pops its registers.
; Everything else a task needs (its interrupt frame, if it was
; preempted; its C call chain) is already on its own stack, so this
; is the whole switch. Interrupts are off throughout: the scheduler
; is the only caller and requires it.
;
; A new task's stack is prepared to look as if it had called
; switch_to itself: four zero registers, then a return address
; pointing at task_trampoline (task.c).

section .text

global switch_to
switch_to:
    mov eax, [esp+4]
    mov edx, [esp+8]
    push ebp
    push edi
    push esi
    push ebx
    mov [eax], esp
    mov esp, edx
    pop ebx
    pop esi
    pop edi
    pop ebp
    ret

; enter_user(eip, esp): drop to ring 3 for the first time. The iret
; frame carries the user code and stack selectors and EFLAGS with
; only IF set, so the program starts with interrupts on and IOPL 0
; (no port access). General registers are cleared so no kernel
; values leak into the program.
global enter_user
enter_user:
    mov ecx, [esp+4]
    mov edx, [esp+8]
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push dword 0x23         ; SS
    push edx                ; ESP
    push dword 0x202        ; EFLAGS
    push dword 0x1B         ; CS
    push ecx                ; EIP
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    iret

section .note.GNU-stack noalloc noexec nowrite progbits
