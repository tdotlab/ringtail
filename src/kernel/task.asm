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

; user_entry: first entry into a ring 3 task. switch_to pops the
; callee-saved registers and rets here, on the task's kernel stack,
; with interrupts still off. The task to enter is current_task
; (the switcher set it before switching), and its entry point and
; user stack top come from its task_t, both VIRTUAL addresses in
; the task's own address space: entry at offset 32, user_stack_top
; at offset 16 (see task.c; esp is offset 0, stack 4, kstack_top 8,
; pd 12). v0.7.0 change: user_stack_top is already a stack top,
; not a stack base, so no add-4096 here anymore; the loader
; computes the top from the program size plus a guard page.
; Three things must happen before the iret, or the user task faults
; on its first instruction. First, DS/ES/FS/GS must be the user
; data selector (0x23): they still hold the kernel data selector
; (0x10, DPL 0), and a ring 3 task cannot use a DPL 0 data segment
; (its privilege is lower than the segment's, so the first data
; access would fault). Second, TSS esp0 must be this task's
; kernel stack top, so its first trap lands on its own stack:
; setting esp0 at creation would have pointed the creating
; task's next trap here instead, overwriting this task's
; startup frame. Third, the iret frame must carry user
; SS/ESP/CS/EIP and EFLAGS with interrupts on (0x202: IF plus the
; always-1 bit), so the CPU switches stacks, drops to ring 3, and
; resumes with the timer able to preempt it. The iret itself does
; the privilege drop: no far call, no gate, just the frame. After
; the iret, the kernel stack is left with the five pushed words
; below its top, dead: the next ring 3 trap loads esp0 (the stack
; top) and pushes its frame downward over them, which is fine,
; because they are not live data anymore.

global user_entry
extern current_task
extern tss_set_esp0
user_entry:
    mov eax, [current_task]
    mov edx, [eax+32]   ; entry virtual address
    mov ecx, [eax+16]   ; user stack top, virtual address
    ; Set TSS esp0 to this task's kernel stack top (offset 8
    ; in task_t), before the first iret. This is the first
    ; moment it is safe to do so: at creation, another task
    ; is running, and setting esp0 then would point that
    ; task's next trap onto this task's stack, overwriting
    ; its startup frame. Here, this task is about to run,
    ; so its first trap will correctly land on its own
    ; stack. The C call clobbers eax, ecx, edx, so entry
    ; and stack top are saved across it and restored after.
    push edx
    push ecx
    push dword [eax+8]  ; kstack_top
    call tss_set_esp0
    add esp, 4
    pop ecx
    pop edx
    mov ax, 0x23        ; user data selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push 0x23           ; SS
    push ecx            ; ESP
    push 0x202          ; EFLAGS: interrupts on
    push 0x1B           ; CS (user code selector)
    push edx            ; EIP
    iret                ; into ring 3, never returns here

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
    ; v0.7.0: switch address spaces with the task. The new task's
    ; page directory is at offset 12 in its task_t. Loading CR3
    ; flushes the TLB, so stale mappings from the old task cannot
    ; leak into the new one. Kernel pages are identical in every
    ; directory (copied identity entries), so the kernel stack
    ; and code keep working across the switch; only the user
    ; range changes. For kernel tasks the directory is the kernel
    ; directory, so the load is a no-op in effect, just a TLB
    ; flush the toy can afford.
    mov eax, [edx+12]
    mov cr3, eax
    pop ebx
    pop esi
    pop edi
    pop ebp
    ret                 ; into the new task, at its saved return address

section .note.GNU-stack noalloc noexec nowrite progbits
