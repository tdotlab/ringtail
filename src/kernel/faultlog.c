// Fault log, v0.10.3. A fault diagnostic printed to the text
// screen is lost when the fault happens in graphics mode:
// the text screen is restored from the copy saved at
// GFX_BEGIN, which predates the diagnostic, so the restore
// overwrites it, and the shell never shows what killed the
// program. The log fixes that by keeping the same diagnostic
// in RAM, not in video memory, so it survives a graphics
// restore, a task death, and a mode switch. The shell's
// `faults` command prints it, so after a graphics fault you
// can still read what happened, even though the screen
// never showed it.
//
// Design, kept small on purpose. Eight entries, ring buffer,
// oldest overwritten first, each holding the diagnostic
// string exactly as printed (without the leading and trailing
// newlines the screen version gets), plus the tick at which
// it happened and the faulting task's id. Eight is enough
// to see a fault and the few before it, and small enough
// (8 x 64 bytes plus two ints) to live in BSS forever
// without a heap allocation, which matters because the log
// is written from fault handlers, where allocating would
// be unsafe. No lock is needed, and none is taken: log
// writes happen only in fault handlers, which run with
// interrupts off (interrupt gates), so no task switch and
// no other log write can interleave with one, and reads
// (the shell dump, in normal kernel context) cannot run
// concurrently with a handler, because the handler never
// yields except through task_exit, which switches away
// from the dead task for good. A kernel fault still halts
// after logging, so its entry is in RAM but unreachable:
// the log is for user faults the OS survives, which is
// the case that matters, and the halt case is stated
// here rather than hidden. The log does not persist
// across reboots: it is RAM, cleared by BSS zeroing at
// boot, and that is the honest scope for this slice.

#include <stdint.h>

extern void print(const char* s);
extern int task_current_id(void);
extern uint32_t timer_get_ticks(void);

#define FAULTLOG_ENTRIES 8
#define FAULTLOG_MSG_LEN 64

typedef struct {
    uint32_t tick;
    int task_id;
    char msg[FAULTLOG_MSG_LEN];
} faultlog_entry_t;

static faultlog_entry_t faultlog[FAULTLOG_ENTRIES];
static int faultlog_start = 0; // index of the oldest entry
static int faultlog_count = 0; // valid entries, 0..8

// Record one diagnostic. The message is copied, bounded to
// 63 characters plus null, so a longer diagnostic truncates
// rather than overflowing the entry. Tick and task id are
// captured here, so callers pass only the diagnostic string,
// and the log and the screen message stay identical by
// construction: the handler builds one string, logs it,
// then prints it.
void faultlog_add(const char* msg) {
    faultlog_entry_t* e;
    if (faultlog_count < FAULTLOG_ENTRIES) {
        e = &faultlog[(faultlog_start + faultlog_count) % FAULTLOG_ENTRIES];
        faultlog_count++;
    } else {
        // Full: overwrite the oldest, and the next oldest
        // becomes the start.
        e = &faultlog[faultlog_start];
        faultlog_start = (faultlog_start + 1) % FAULTLOG_ENTRIES;
    }
    e->tick = timer_get_ticks();
    e->task_id = task_current_id();
    int i = 0;
    while (msg[i] && i < FAULTLOG_MSG_LEN - 1) {
        e->msg[i] = msg[i];
        i++;
    }
    e->msg[i] = 0;
}

static void faultlog_print_dec(uint32_t n) {
    char buf[12];
    int i = 0;
    if (n == 0) { print("0"); return; }
    while (n > 0) { buf[i++] = (char)('0' + (n % 10)); n /= 10; }
    // Print in reverse, without a print_char helper here:
    // print() takes a string, so build a null-terminated
    // reversed copy on the stack.
    char out[12];
    int j = 0;
    while (i > 0) out[j++] = buf[--i];
    out[j] = 0;
    print(out);
}

// Print every entry, oldest first, or a plain "none" line
// when the log is empty. Called from the shell, in normal
// kernel context, under the shell's print lock, so its
// output cannot interleave with another task's prints.
void faultlog_dump(void) {
    if (faultlog_count == 0) {
        print("No faults logged.\n");
        return;
    }
    for (int i = 0; i < faultlog_count; i++) {
        faultlog_entry_t* e = &faultlog[(faultlog_start + i) % FAULTLOG_ENTRIES];
        print("tick ");
        faultlog_print_dec(e->tick);
        print(" task ");
        // Task ids are 0 and up once task_init has run; -1
        // only means "before task_init", which can only
        // happen for a boot fault that halts anyway, so
        // printing it as -1, not as a huge unsigned, is
        // the honest form.
        if (e->task_id < 0) print("-1");
        else faultlog_print_dec((uint32_t)e->task_id);
        print(": ");
        print(e->msg);
        print("\n");
    }
}
