// PS/2 mouse driver. The mouse is the second device on the keyboard
// controller (the 8042), sharing the data port 0x60 with the keyboard
// but with its own interrupt, IRQ 12, and its own command channel.
// The controller tells the two apart with a status bit: bit 5 of the
// status port 0x64 is set when the byte waiting in 0x60 came from
// the mouse (aux device), clear when it came from the keyboard.
//
// How a mouse talks, in plain terms: it does not report an absolute
// position, like a touchscreen would. It reports movement: "I moved
// 3 pixels left and 2 up since last time", in three-byte packets,
// and the OS keeps a running cursor position by adding those
// movements up and clamping the result to the screen. That is why
// the driver holds mouse_x/mouse_y as state, not just the last
// packet: the packet alone is a delta, the position is accumulated.
//
// Packet layout (3 bytes, sent on IRQ 12):
//   byte 0: bit 0 left button, bit 1 right, bit 2 middle,
//           bit 3 always 1 (this is the sync check: a first byte
//           with bit 3 clear is not a packet start, so it is
//           dropped and the state machine waits for a real one),
//           bit 4 X sign, bit 5 Y sign, bit 6 X overflow,
//           bit 7 Y overflow.
//   byte 1: X movement, signed 8-bit (if the X sign bit is set,
//           the value is byte - 256, so 0xFF means -1, not 255).
//   byte 2: Y movement, same encoding, but positive means UP,
//           the opposite of screen coordinates, where Y grows
//           downward, so the driver subtracts Y movement from
//           the cursor's Y.
// An overflow bit means the mouse moved further than one signed
// byte can express since the last packet, so that axis is ignored
// for that packet rather than trusted: a wrong jump is worse
// than a missed one, and the next packet corrects it.
//
// Init sequence, each step with a finite timeout so a missing
// mouse cannot hang the boot (the same graceful-degradation rule
// as the disk: no mouse, no crash, just "not found"):
//   1. Enable the aux device (command 0xA8 to port 0x64).
//   2. Read the controller command byte (command 0x20), set bit 1
//      (mouse IRQ enable) and clear bit 5 (mouse disable, so 0
//      means enabled), write it back (command 0x60). The keyboard
//      bits are left exactly as they were.
//   3. Reset the mouse (0xFF via the aux channel): it answers
//      0xAA (self-test passed) then 0x00 (its device ID).
//   4. Set defaults (0xF6), enable data reporting (0xF4), each
//      answered with 0xFA (ACK).
// The aux channel: to send a byte to the mouse, write 0xD4 to the
// command port, then the byte to the data port. The mouse's
// replies arrive as aux bytes on IRQ 12 once reporting is on,
// but during init interrupts are still off, so init polls the
// status port and reads aux bytes directly, discarding any
// keyboard byte that happens to be waiting (status bit 5 clear).

#include <stdint.h>

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

#define KBC_STATUS 0x64
#define KBC_CMD    0x64
#define KBC_DATA   0x60

// Polling loops need a bound: without one, a missing or stuck
// device would spin forever and the boot would never finish.
// 100000 port reads is far more than a real controller needs
// (it answers in microseconds) and still finishes instantly.
#define KBC_TIMEOUT 100000

static int kbc_wait_input_empty(void) {
    for (int i = 0; i < KBC_TIMEOUT; i++) {
        if (!(inb(KBC_STATUS) & 0x02)) return 0;
    }
    return -1;
}

// Wait for a byte from the MOUSE (aux bit set). A keyboard byte
// waiting in the same port is read and discarded, not mistaken
// for a mouse reply: the status bit is checked before each read,
// so a key pressed during boot cannot derail mouse init.
static int kbc_wait_aux_byte(uint8_t* out) {
    for (int i = 0; i < KBC_TIMEOUT; i++) {
        uint8_t st = inb(KBC_STATUS);
        if (st & 0x01) {
            uint8_t aux = (st & 0x20) != 0;
            uint8_t b = inb(KBC_DATA);
            if (aux) { *out = b; return 0; }
            // else: a keyboard byte, discard and keep waiting
        }
    }
    return -1;
}

// Wait for any byte (used for the command-byte read, which is a
// controller reply, not a mouse byte, so the aux bit is clear).
static int kbc_wait_byte(uint8_t* out) {
    for (int i = 0; i < KBC_TIMEOUT; i++) {
        if (inb(KBC_STATUS) & 0x01) {
            *out = inb(KBC_DATA);
            return 0;
        }
    }
    return -1;
}


static int kbc_write_cmd(uint8_t cmd) {
    if (kbc_wait_input_empty() != 0) return -1;
    outb(KBC_CMD, cmd);
    return 0;
}

// Send one byte to the mouse and wait for its ACK (0xFA).
static int mouse_cmd(uint8_t cmd) {
    uint8_t ack;
    if (kbc_write_cmd(0xD4) != 0) return -1;
    if (kbc_wait_input_empty() != 0) return -1;
    outb(KBC_DATA, cmd);
    if (kbc_wait_aux_byte(&ack) != 0) return -1;
    return (ack == 0xFA) ? 0 : -1;
}

// Cursor state. Written only by the IRQ handler, read by the
// shell and the graphics demo. The reads are single aligned
// 32-bit loads, atomic on x86, so no lock is needed: the worst
// case is one frame where X comes from the old packet and Y
// from the new one, which is invisible at 100 frames a second.
static volatile int mouse_found_flag = 0;
static volatile int mouse_x_val = 160;
static volatile int mouse_y_val = 100;
static volatile int mouse_buttons_val = 0;
static volatile uint32_t mouse_packets = 0;

// Packet assembly state, IRQ handler only.
static uint8_t pkt[3];
static int pkt_idx = 0;

int mouse_found(void) { return mouse_found_flag; }
int mouse_get_x(void) { return mouse_x_val; }
int mouse_get_y(void) { return mouse_y_val; }
int mouse_get_buttons(void) { return mouse_buttons_val; }
uint32_t mouse_get_packets(void) { return mouse_packets; }

// Called from mouse_isr (isr.asm). Keep it short: read, assemble,
// update, acknowledge. Never print from here: print takes the
// VGA lock, and an IRQ handler that blocks on a lock held by
// the task it interrupted would deadlock the machine.
void mouse_handler(void) {
    uint8_t st = inb(KBC_STATUS);
    if (st & 0x01) {
        uint8_t aux = (st & 0x20) != 0;
        uint8_t b = inb(KBC_DATA);
        if (aux) {
            // Resync: a first byte must have bit 3 set. If it
            // does not, a byte was lost somewhere and this one
            // is a middle byte, so drop it and wait for a real
            // start instead of assembling a shifted packet.
            if (pkt_idx == 0 && !(b & 0x08)) {
                // drop, stay at index 0
            } else {
                pkt[pkt_idx++] = b;
                if (pkt_idx == 3) {
                    pkt_idx = 0;
                    mouse_packets++;
                    mouse_buttons_val = pkt[0] & 0x07;
                    if (!(pkt[0] & 0x40)) { // X overflow: ignore
                        int dx = pkt[1];
                        if (pkt[0] & 0x10) dx -= 256;
                        mouse_x_val += dx;
                        if (mouse_x_val < 0) mouse_x_val = 0;
                        if (mouse_x_val > 319) mouse_x_val = 319;
                    }
                    if (!(pkt[0] & 0x80)) { // Y overflow: ignore
                        int dy = pkt[2];
                        if (pkt[0] & 0x20) dy -= 256;
                        // PS/2 Y is positive up, screen Y is
                        // positive down, so subtract.
                        mouse_y_val -= dy;
                        if (mouse_y_val < 0) mouse_y_val = 0;
                        if (mouse_y_val > 199) mouse_y_val = 199;
                    }
                }
            }
        }
        // else: a keyboard byte on the mouse IRQ, which should
        // not happen; it was already read, so just drop it.
    }
    // EOI to BOTH pics, slave first: IRQ 12 lives on the slave,
    // and the slave cannot signal again until it is told, nor
    // can the master forward anything until it is told. The
    // keyboard IRQ needs only the master EOI because it lives
    // on the master; this one needs both, in this order.
    outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

// Called from kernel_main after idt_init, before sti: polling
// works with interrupts off, and the first mouse IRQ will only
// arrive once sti runs, by which time the state machine is ready.
// Returns 0 if a mouse answered, -1 if not (boot continues either
// way; the shell reports "not found" and graphics still works,
// just with a cursor that cannot move).
int mouse_init(void) {
    uint8_t cmd_byte, reply = 0;
    mouse_found_flag = 0;

    // Drain any stale output first: on power-on, both the keyboard
    // and the mouse send a self-test byte (0xAA) that can still be
    // sitting in the controller when the kernel starts. If that
    // byte is read as the "command byte" below, the write-back
    // stores garbage and can disable the keyboard IRQ entirely,
    // which is exactly what the first v0.8.0 QEMU run showed:
    // typing dead, mouse not found. Drain first, then every byte
    // read after a command is guaranteed fresh.
    for (int i = 0; i < 100; i++) {
        if (!(inb(KBC_STATUS) & 0x01)) break;
        (void)inb(KBC_DATA);
    }

    if (kbc_write_cmd(0xA8) != 0) goto fail;
    if (kbc_write_cmd(0x20) != 0) goto fail;
    if (kbc_wait_byte(&cmd_byte) != 0) goto fail;
    cmd_byte = (uint8_t)((cmd_byte | 0x02) & ~0x20); // IRQ on, mouse on
    if (kbc_write_cmd(0x60) != 0) goto fail;
    if (kbc_wait_input_empty() != 0) goto fail;
    outb(KBC_DATA, cmd_byte);

    // Reset: the mouse ACKs the command (0xFA) FIRST, then sends
    // its self-test result (0xAA) and device ID (0x00). Waiting
    // for 0xAA straight after 0xFF reads the ACK instead and
    // fails, which is what the first debug run showed
    // ("MFAIL reset-AA 250", 250 = 0xFA). All three, in order.
    if (kbc_write_cmd(0xD4) != 0) goto fail;
    if (kbc_wait_input_empty() != 0) goto fail;
    outb(KBC_DATA, 0xFF);
    if (kbc_wait_aux_byte(&reply) != 0 || reply != 0xFA) goto fail;
    if (kbc_wait_aux_byte(&reply) != 0 || reply != 0xAA) goto fail;
    if (kbc_wait_aux_byte(&reply) != 0 || reply != 0x00) goto fail;

    if (mouse_cmd(0xF6) != 0) goto fail;
    if (mouse_cmd(0xF4) != 0) goto fail;

    mouse_x_val = 160;
    mouse_y_val = 100;
    mouse_buttons_val = 0;
    pkt_idx = 0;
    mouse_found_flag = 1;
    return 0;

fail:
    // A failed init must not leave half a handshake in the
    // controller: unread reset or ACK bytes would sit in the
    // output register and block the keyboard's bytes behind them.
    // Drain, then report not found and let the boot continue.
    for (int i = 0; i < 100; i++) {
        if (!(inb(KBC_STATUS) & 0x01)) break;
        (void)inb(KBC_DATA);
    }
    return -1;
}
