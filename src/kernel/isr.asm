; CPU can't call C directly from IDT, needs this wrapper

global idt_load
extern idt_ptr

idt_load:
    mov eax, [esp+4]
    lidt [eax]
    ret

global keyboard_isr
extern keyboard_handler

keyboard_isr:
    pusha               ; save all registers
    call keyboard_handler
    popa                ; restore registers
    iret                ; return from interrupt

global timer_isr
extern timer_handler

; v0.5.0: the timer ISR is the preemption point, so it re-enables
; interrupts (sti) before calling the handler, unlike the keyboard
; ISR, which keeps them off for its short run. The sti matters for
; two reasons. First, the task that gets switched in by
; task_preempt must resume with interrupts on, or the timer would
; never fire again and the system would go quiet. Second, a tick
; that arrives while the handler is picking a task must not be
; lost: with interrupts on, it stays pending behind task_preempt's
; cli and fires as soon as that cli lifts. The cli before popa is
; the mirror image: the register restore and the iret must not be
; interrupted, because iret itself restores the interrupted task's
; interrupt flag from its saved EFLAGS.
timer_isr:
    pusha               ; save all registers
    sti                 ; re-enable interrupts for the handler
    call timer_handler
    cli                 ; protect the restore + iret
    popa                ; restore registers
    iret                ; return from interrupt

; Exception stubs (vectors 0-31). Each stub records its vector number
; and jumps to a common handler that prints it and halts. The handler
; never returns to the faulting code, so it does not matter that the
; CPU pushes an error code for some vectors and not for others: no
; iret is ever attempted, so the stack imbalance is harmless.
section .bss
global exc_vector
exc_vector:
    resd 1

section .text
extern exception_handler

exception_common:
    pusha
    call exception_handler
    popa
    cli
.hang:
    hlt
    jmp .hang

%macro EXC_STUB 1
global exception_%1
exception_%1:
    mov dword [exc_vector], %1
    jmp exception_common
%endmacro

EXC_STUB 0
EXC_STUB 1
EXC_STUB 2
EXC_STUB 3
EXC_STUB 4
EXC_STUB 5
EXC_STUB 6
EXC_STUB 7
EXC_STUB 8
EXC_STUB 9
EXC_STUB 10
EXC_STUB 11
EXC_STUB 12
EXC_STUB 13
EXC_STUB 14
EXC_STUB 15
EXC_STUB 16
EXC_STUB 17
EXC_STUB 18
EXC_STUB 19
EXC_STUB 20
EXC_STUB 21
EXC_STUB 22
EXC_STUB 23
EXC_STUB 24
EXC_STUB 25
EXC_STUB 26
EXC_STUB 27
EXC_STUB 28
EXC_STUB 29
EXC_STUB 30
EXC_STUB 31

section .note.GNU-stack noalloc noexec nowrite progbits
