; switch_to(old, new) swaps the running task by swapping stack
; pointers. It only saves the callee-saved registers (ebp, edi, esi,
; ebx) plus the return address that call already pushed. In v0.4.0
; that was enough because a switch only ever happened at a yield
; point, inside a normal C call, never in the middle of an
; expression. In v0.5.0 it is still enough, for a subtler reason:
; when the timer preempts a task, the switch happens inside
; timer_handler, which is itself a normal C call from timer_isr,
; so the interrupted task's full interrupt frame (pusha registers
; plus EFLAGS/CS/EIP) is sitting on its stack underneath the
; switch_to frame, and it stays there, parked, until that task is
; switched back to and timer_isr popa/irets it away. The switch
; itself never has to know about the interrupt frame; it only has
; to not disturb it, and saving/restoring just the callee-saved
; registers on top of it does exactly that.
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
