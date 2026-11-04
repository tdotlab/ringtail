// RingTail - v0.4.0: cooperative multitasking
// A task is a function with its own stack. The scheduler is not a
// separate thread and there is no timer-driven switch: a task runs
// until it yields, sleeps, or exits, and only then does another
// task run. That is cooperative scheduling, the oldest kind, and
// the reason it is safe here is that a switch only happens at a
// yield point, never in the middle of a print or a heap operation,
// so no lock is needed around VGA output or the heap.
// The kernel main loop is task 0 ("shell"). It drains the keyboard
// queue, cleans up dead tasks, calls task_yield() to run the next
// ready task, and then halts until the next interrupt. A task that
// sleeps is skipped until its wake tick arrives; the main task is
// always ready, so a sleeping task always has somewhere to yield
// to, and the main loop's hlt waits for the timer tick that will
// eventually wake it. Shell `sleep` still uses timer_sleep and
// still blocks the scheduler while it runs: that is a known v0.4.0
// limitation, not a bug in task_sleep, which is for tasks only.

#include <stdint.h>

#define TASK_READY   0
#define TASK_RUNNING 1
#define TASK_BLOCKED 2
#define TASK_DEAD    3

#define TASK_STACK_SIZE 4096
#define MAX_SLEEP_SECONDS (0x7FFFFFFFu / 100)

typedef struct task {
    uint32_t esp;        // MUST be first: switch_to saves/loads [task]
    uint32_t stack;      // heap address of the stack allocation
    uint32_t wake_tick;  // for BLOCKED tasks: tick at which to wake
    int id;
    int state;
    void (*entry)(void*);
    void* arg;
    char name[32];
    struct task* next;
} task_t;

extern void print(const char* s);
extern void print_char(char c);
extern void print_color(const char* s, unsigned char color);
extern void* kmalloc(uint32_t size);
extern int kfree(void* ptr);
extern uint32_t timer_get_ticks(void);
extern void switch_to(task_t* old, task_t* new);

static task_t main_task;       // task 0, the kernel main loop itself
static task_t* task_list = 0;  // head of the list, main_task first
static task_t* current_task = 0;
static int next_id = 1;

static void print_num(uint32_t n) {
    char buf[12];
    int i = 0;
    if (n == 0) { print_char('0'); return; }
    while (n > 0) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    while (i > 0) print_char(buf[--i]);
}

void task_init(void) {
    main_task.esp = 0;
    main_task.stack = 0;
    main_task.wake_tick = 0;
    main_task.id = 0;
    main_task.state = TASK_RUNNING;
    main_task.entry = 0;
    main_task.arg = 0;
    main_task.name[0] = 's'; main_task.name[1] = 'h';
    main_task.name[2] = 'e'; main_task.name[3] = 'l';
    main_task.name[4] = 'l'; main_task.name[5] = 0;
    main_task.next = 0;
    task_list = &main_task;
    current_task = &main_task;
    next_id = 1;
}

// First task in the list after `from`, wrapping around, that is
// ready to run. A blocked task whose wake tick has arrived becomes
// ready here. Dead and still-sleeping tasks are skipped. Returns 0
// if nothing but `from` itself is runnable.
static task_t* task_pick_next(task_t* from) {
    task_t* t = from->next ? from->next : task_list;
    task_t* start = t;
    while (1) {
        if (t->state == TASK_BLOCKED) {
            // Wrap-safe wake check: (ticks - wake) >= 0 in signed
            // arithmetic means wake has passed, even across the
            // 32-bit tick wraparound, for sleeps under 2^31 ticks.
            if ((int32_t)(timer_get_ticks() - t->wake_tick) >= 0)
                t->state = TASK_READY;
        }
        if (t->state == TASK_READY) return t;
        t = t->next ? t->next : task_list;
        if (t == start) break;
    }
    return 0;
}

void task_yield(void) {
    task_t* old = current_task;
    task_t* next = task_pick_next(old);
    if (!next || next == old) return;
    if (old->state == TASK_RUNNING) old->state = TASK_READY;
    next->state = TASK_RUNNING;
    current_task = next;
    switch_to(old, next);
    // Execution resumes here when this task is switched back to.
    current_task = old;
    old->state = TASK_RUNNING;
}

// Sleep `seconds` seconds, yielding to other tasks meanwhile.
// Returns 0 on success, -1 if the duration is too large (same bound
// as timer_sleep). For tasks only: the shell's sleep command still
// uses timer_sleep, which blocks the scheduler.
int task_sleep(uint32_t seconds) {
    if (seconds > MAX_SLEEP_SECONDS) return -1;
    if (seconds == 0) { task_yield(); return 0; }
    current_task->state = TASK_BLOCKED;
    current_task->wake_tick = timer_get_ticks() + seconds * 100;
    task_yield();
    return 0;
}

void task_exit(void) {
    current_task->state = TASK_DEAD;
    task_yield();
    // Never reached: a dead task is never picked again, so the
    // switch away from it never returns. If it ever did, the task
    // would loop here yielding forever rather than running dead
    // code, but the scheduler guarantees it cannot happen.
    while (1) task_yield();
}

// Free stacks and descriptors of dead tasks. Called from the main
// loop, where the current task is always the main task, so a dead
// task's stack is never the stack running this code.
void task_cleanup(void) {
    task_t* prev = task_list;
    task_t* t = task_list->next;
    while (t) {
        task_t* nxt = t->next;
        if (t->state == TASK_DEAD) {
            prev->next = nxt;
            if (t->stack) kfree((void*)t->stack);
            kfree(t);
        } else {
            prev = t;
        }
        t = nxt;
    }
}

static void task_trampoline(void) {
    task_t* t = current_task;
    t->entry(t->arg);
    task_exit();
}

// Create a task running entry(arg) on a fresh 4096-byte heap stack.
// Returns the task id, or -1 on out of memory. The stack is faked
// to look like a task that just called switch_to (see task.asm),
// so the first switch to it "returns" into task_trampoline.
int task_create(const char* name, void (*entry)(void*), void* arg) {
    task_t* t = (task_t*)kmalloc(sizeof(task_t));
    if (!t) return -1;
    uint32_t stack = (uint32_t)kmalloc(TASK_STACK_SIZE);
    if (!stack) { kfree(t); return -1; }
    uint32_t top = (stack + TASK_STACK_SIZE) & ~0xFu;
    uint32_t* sp = (uint32_t*)(top - 20);
    sp[0] = 0; // ebx
    sp[1] = 0; // esi
    sp[2] = 0; // edi
    sp[3] = 0; // ebp
    sp[4] = (uint32_t)task_trampoline;
    t->esp = (uint32_t)sp;
    t->stack = stack;
    t->wake_tick = 0;
    t->id = next_id++;
    t->state = TASK_READY;
    t->entry = entry;
    t->arg = arg;
    int i = 0;
    while (name[i] && i < 31) { t->name[i] = name[i]; i++; }
    t->name[i] = 0;
    t->next = 0;
    task_t* tail = task_list;
    while (tail->next) tail = tail->next;
    tail->next = t;
    return t->id;
}

// Mark a task dead; the main loop's task_cleanup frees it. Cannot
// kill the main task (id 0) or a task that does not exist, and a
// task may not kill itself (it should return, which exits it).
int task_kill(int id) {
    if (id == 0) return -1;
    task_t* t = task_list->next;
    while (t) {
        if (t->id == id) {
            if (t == current_task) return -1;
            t->state = TASK_DEAD;
            return 0;
        }
        t = t->next;
    }
    return -1;
}

static const char* task_state_name(int state) {
    if (state == TASK_READY) return "ready";
    if (state == TASK_RUNNING) return "running";
    if (state == TASK_BLOCKED) return "sleeping";
    return "dead";
}

void task_list_tasks(void) {
    task_t* t = task_list;
    while (t) {
        print_num((uint32_t)t->id);
        print("  ");
        print(t->name);
        print("  ");
        print(task_state_name(t->state));
        print("\n");
        t = t->next;
    }
}

// Demo task: counts once a second, forever, yielding between
// counts. Its prints are atomic with respect to other tasks,
// because a switch only happens inside task_sleep, never in the
// middle of a print.
static void counter_task(void* arg) {
    (void)arg;
    uint32_t n = 0;
    while (1) {
        print("counter: ");
        print_num(n++);
        print("\n");
        task_sleep(1);
    }
}

int task_spawn_counter(void) {
    return task_create("counter", counter_task, 0);
}
