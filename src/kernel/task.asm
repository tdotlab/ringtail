; RingTail - v0.4.0: cooperative task switch
; switch_to(old, new) swaps the running task by swapping stack
; pointers. It only saves the callee-saved registers (ebp, edi, esi,
; ebx) plus the return address that call already pushed, because a
; cooperative switch only ever happens at a yield point, inside a
; normal C call, never in the middle of an expression. Interrupts
; stay on: a timer or keyboard IRQ during a task uses its own
; pusha/iret wrapper and returns to the same task, it never switches
; tasks. That is the whole difference between cooperative and
; preemptive scheduling, and it is why this switch is so short.
;
; task_t layout (see task.c): esp is the first field, offset 0, so
; [old] is the slot to save esp into and [new] is the slot to load
; esp from. A freshly created task's stack is faked to look like a
; task that just called switch_to: ebx, esi, edi, ebp (all zero),
; then a return address pointing at task_trampoline, which calls
; the task's entry function and then task_exit().

global switch_to
switch_to:
    mov eax, [esp+4]    ; old task_t*
    mov edx, [esp+8]    ; new task_t*
    push ebp
    push edi
    push esi
    push ebx
    mov [eax], esp      ; save old stack pointer
    mov esp, [edx]      ; load new stack pointer
    pop ebx
    pop esi
    pop edi
    pop ebp
    ret                 ; into the new task, at its saved return address

section .note.GNU-stack noalloc noexec nowrite progbits
