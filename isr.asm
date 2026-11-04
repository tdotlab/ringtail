; RingTail - Piece 5: ISR glue
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
