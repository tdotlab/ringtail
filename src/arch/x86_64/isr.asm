; Interrupt entry, x86_64. Every vector gets a tiny stub that pushes a
; dummy error code (when the CPU does not push one) and its vector
; number, so all of them arrive at isr_common with the same frame.
; isr_common saves the general registers and hands a pointer to the
; frame (regs_t, see arch/regs.h) to interrupt_dispatch.
;
; In 64-bit mode the CPU always pushes SS and RSP and aligns the stack
; to 16 bytes first, so the frame looks the same whether the trap came
; from ring 0 or ring 3, and rsp is 16-byte aligned at the call. The
; data segment registers play no part in addressing here, so there is
; nothing of the user's to replace on the way in but gs's base: from
; ring 3 the entry does a swapgs, for the kernel's (this CPU's block,
; arch/percpu.h), and the exit does another when it goes back there.
; The ring is in the saved CS.
;
; That leaves the instructions where the kernel runs in ring 0 with the
; program's gs: the SYSCALL entry's first one, and from the last
; swapgs of an exit to its iretq or sysret. Maskable interrupts are off
; there, but an NMI or a machine check is not stopped by that, and a
; double fault can come from anywhere; those three take isr_paranoid,
; which asks the CPU which gs it has.

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

%macro ISR_PARANOID 1
isr%1:
%if %1 != 8
    push 0                  ; the double fault pushes an error code (0)
%endif
    push %1
    jmp isr_paranoid
%endmacro

; CPU exceptions. 8, 10-14, 17, 21, 29 and 30 push an error code.
ISR_NOERR 0
ISR_NOERR 1
ISR_PARANOID 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_PARANOID 8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_PARANOID 18
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
; Hardware IRQs 0-15, on vectors 32-47 from the 8259s or the I/O APIC.
%assign v 32
%rep 16
ISR_NOERR v
%assign v v+1
%endrep
; System call.
ISR_NOERR 128
; The reschedule IPI, RESCHED_VECTOR.
ISR_NOERR 240

; The local APIC's spurious vector (0xFF): an interrupt that went away
; between the APIC signalling it and the CPU taking it. Nothing is in
; service, so there is no EOI to send and nothing else to do; it
; touches no register, so it needs no frame.
isr_spurious:
    iretq

%macro PUSH_REGS 0
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
%endmacro

%macro POP_REGS 0
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
%endmacro

isr_common:
    test byte [rsp + 24], 3 ; the saved CS, past the vector and error code
    jz .from_kernel
    swapgs
.from_kernel:
    PUSH_REGS
    cld
    mov rdi, rsp            ; regs_t* for the C handler
    call interrupt_dispatch
isr_return:
    cli                     ; nothing may come in between the swapgs and the iretq
    POP_REGS
    add rsp, 16             ; vector and error code
    test byte [rsp + 8], 3  ; the CS iretq goes back to
    jz .to_kernel
    swapgs
.to_kernel:
    iretq

; An NMI, double fault or machine check: from anywhere, the instructions
; above with the program's gs included. The GS_BASE MSR says which gs
; this is: the kernel's is in the upper half, the program's is 0. rbx
; remembers the answer across the C call, which preserves it.
MSR_GS_BASE equ 0xC0000101

isr_paranoid:
    PUSH_REGS
    cld
    mov ecx, MSR_GS_BASE
    rdmsr                   ; edx: the top half
    xor ebx, ebx
    test edx, edx
    js .kernel_gs
    swapgs
    mov ebx, 1
.kernel_gs:
    mov rdi, rsp
    call interrupt_dispatch ; returns only for an NMI
    test ebx, ebx
    jz .same_gs
    swapgs
.same_gs:
    POP_REGS
    add rsp, 16
    iretq

; The SYSCALL instruction, the usual way in for a system call. It saves
; the return rip in rcx and rflags in r11, clears the flags in FMASK
; (IF among them) and jumps here in ring 0, still on the user's stack.
; The stub swaps in the kernel's gs, parks the user's rsp in the per-CPU
; block (arch/percpu.h), switches to the task's kernel stack, and builds
; the same frame an int 0x80 would have: so interrupt_dispatch, the
; scheduler and a kill all work on it unchanged, and it can always
; leave by iretq.
;
; It leaves by SYSRET when it can, which is faster: rcx and r11 come
; back as the return address and flags, so they are not preserved, and
; the return address must be a canonical user address. On Intel a
; SYSRET to a non-canonical one faults in ring 0, on the user's stack
; (the hole several kernels had in 2012); those returns, and any frame
; not headed back to user code, take the iretq.
;
; Until the stack switch, and again after the last instruction that
; reads the frame, rsp is the user's. Interrupts are off then, but an
; NMI or machine check is not stopped by that; they have their own
; stacks (IST, gdt.c), and take the paranoid entry, since gs is the
; program's for the first instruction and the last two.

CPU_USER_RSP equ 0          ; offsets in cpu_t, arch/percpu.h
CPU_KSTACK equ 8
USER_DATA equ 0x1B          ; SEL_USER_DATA and SEL_USER_CODE, kernel.h
USER_CODE equ 0x23
FRAME_RIP equ 17*8          ; regs_t offsets, arch/regs.h
FRAME_CS equ 18*8
FRAME_RFLAGS equ 19*8
FRAME_RSP equ 20*8

global syscall_entry
syscall_entry:
    swapgs                  ; gs now points at this CPU's block
    mov [gs:CPU_USER_RSP], rsp
    mov rsp, [gs:CPU_KSTACK]
    push USER_DATA          ; ss
    push qword [gs:CPU_USER_RSP]
    push r11                ; rflags
    push USER_CODE          ; cs
    push rcx                ; rip
    push 0                  ; error code
    push 128                ; vector: SYSCALL_VECTOR, the same as int 0x80
    PUSH_REGS
    mov rdi, rsp
    call interrupt_dispatch
    cli                     ; interrupt_dispatch returns with them off; make sure
    mov rcx, [rsp + FRAME_RIP]
    mov r11, rcx
    shr r11, 47             ; a canonical user address has bits 47-63 clear
    jnz isr_return          ; which swaps gs back if the frame goes to ring 3
    cmp qword [rsp + FRAME_CS], USER_CODE
    jne isr_return
    mov r11, [rsp + FRAME_RFLAGS]
    pop r15
    pop r14
    pop r13
    pop r12
    add rsp, 8              ; r11: the flags, already loaded
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    add rsp, 8              ; rcx: the return address, already loaded
    pop rbx
    pop rax
    mov rsp, [rsp + FRAME_RSP - 15*8]
    swapgs
    o64 sysret

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
global isr_spurious_stub
isr_spurious_stub:
    dq isr_spurious
global isr_resched_stub
isr_resched_stub:
    dq isr240

section .text

; idt_load(ptr)
global idt_load
idt_load:
    lidt [rdi]
    ret

; gdt_load(ptr): install the GDT, reload the segment registers and load
; the task register with the TSS, so the CPU knows which stack to use
; for traps from ring 3. CS takes a far return. Loading gs can clear
; its base, so gdt_init_cpu sets that afterwards.
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
