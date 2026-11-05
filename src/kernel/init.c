// init: the one kernel task that runs programs on its own. It starts
// the shell from the disk, gives it the terminal and waits; when the
// shell ends, for any reason, it starts a new one. So `exit`, a crash
// or Ctrl+backslash at the prompt cost the shell's history, nothing
// more.
#include "kernel.h"
#include "console.h"
#include "drivers.h"
#include "task.h"
#include "tty.h"
#include "fs.h"

#define SHELL "SAKLAS"

void init_main(void* arg) {
    (void)arg;
    if (!diskfs_ready()) {
        kprintf_color(COLOR_ERROR, "No disk, so no shell to start. Nothing more to do.\n");
        for (;;) task_sleep_ticks(0xFFFFFFFFu);
    }
    for (;;) {
        const char* argv[] = { "saklas" };
        uint32_t start = timer_ticks();
        // Held until it has the terminal: a shell that ran first would
        // read as a background job, fail, and end at once.
        int id = loader_spawn(DISKFS_ROOT, SHELL, 1, argv, 0, 0, 1);
        if (id < 0) {
            kprintf_color(COLOR_ERROR, "init: cannot start %s: %s\n", SHELL, strerror(id));
            task_sleep_ticks(5 * TIMER_HZ);
            continue;
        }
        tty_set_foreground(id);
        task_release(id);
        int status = task_join(id);
        task_wait_job(id);
        tty_set_foreground(current->job);  // clears up after a shell that died in raw mode
        kprintf_color(COLOR_DIM, "[the shell ended (status %d); starting a new one]\n", status);
        // A shell that dies at once would otherwise restart in a tight loop.
        if (timer_ticks() - start < TIMER_HZ) task_sleep_ticks(TIMER_HZ);
    }
}
