; CPU can't call C directly from IDT, needs this wrapper

global idt_load
extern idt_ptr

idt_load:
    mov eax, [esp+4]
    lidt [eax]
    ret

; gdt_load(ptr): install a new GDT and reload every segment
; register from it. The far jump reloads CS (the CPU cannot mov
; into CS directly); the movs reload DS/ES/FS/GS/SS. The ltr
; loads the task register with the TSS selector, which tells the
; CPU where to find esp0 for ring 3 traps. Called with interrupts
; off, before the IDT is live, so no trap can arrive mid-reload.
global gdt_load
gdt_load:
    mov eax, [esp+4]
    lgdt [eax]
    jmp 0x08:reload_cs
reload_cs:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov ax, 0x28
    ltr ax
    ret

; syscall_isr: the int 0x80 handler. The CPU, on a ring 3 trap,
; switches to the kernel stack (TSS esp0) and pushes SS/ESP/
; EFLAGS/CS/EIP, then jumps here with interrupts cleared (it is
; an interrupt gate). pusha saves the user's registers, sti
; re-enables the timer, because the syscall handler can block:
; a print syscall spins on the print lock, and if interrupts
; stayed off the lock holder could never run to release it, so
; the syscall would spin forever. With sti, a tick preempts the
; syscall exactly like any kernel task, parks its frame, and
; resumes it later. The cli before popa mirrors timer_isr: the
; register restore and the iret must not be interrupted. The
; return value in eax is written into the saved-eax slot on the
; stack (offset 28 after pusha, once the four pushed arguments
; are removed), so popa restores eax as the syscall's result
; instead of the user's original eax.
global syscall_isr
extern syscall_dispatch
syscall_isr:
    pusha
    sti
    push edx                ; 3rd arg (c)
    push ecx                ; 2nd arg (b)
    push ebx                ; 1st arg (a)
    push eax                ; syscall number
    call syscall_dispatch
    add esp, 16
    mov [esp+28], eax       ; saved eax = result
    cli
    popa
    iret

global keyboard_isr
extern keyboard_handler

keyboard_isr:
    pusha               ; save all registers
    call keyboard_handler
    popa                ; restore registers
    iret                ; return from interrupt

; Mouse ISR, vector 44 (IRQ 12, slave IRQ 4). Same shape as the
; keyboard ISR: the C handler reads the packet byte, updates the
; cursor, and sends EOI to both PICs (slave first, then master).
global mouse_isr
extern mouse_handler

mouse_isr:
    pusha
    call mouse_handler
    popa
    iret

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

; Page fault stub (vector 14), separate from the generic exception
; stubs because a page fault carries two diagnostics the generic
; handler throws away: the faulting address in CR2 and the error
; code the CPU pushes. On entry the stack top is the error code
; (the CPU pushes EFLAGS/CS/EIP first, then the error code on
; top). The stub pushes CR2 first, then the error code, so the C
; handler sees (err, addr) in cdecl order, then calls a handler
; that prints both and halts. It never returns, so no iret is
; attempted and the stack imbalance is harmless, as with the
; generic stubs.
global page_fault_isr
extern page_fault_handler
page_fault_isr:
    mov eax, cr2
    push eax                ; addr (2nd arg)
    mov eax, [esp+4]        ; error code (was on top, now shifted)
    push eax                ; err (1st arg)
    call page_fault_handler
.hang_pf:
    cli
    hlt
    jmp .hang_pf

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
