// init: the one kernel task that runs programs on its own. Once DHCP
// has had its chance, it sets the clock with `sntp -b` if the network
// is up. Then it starts the shell from the disk, gives it the terminal
// and waits; when the shell ends, for any reason, it starts a new one.
// So `exit`, a crash or Ctrl+backslash at the prompt cost the shell's
// history, nothing more.
#include "kernel.h"
#include "console.h"
#include "drivers.h"
#include "task.h"
#include "tty.h"
#include "fs.h"
#include "net.h"

#define SHELL "SAKLAS"
#define SNTP  "SNTP"

// Runs prog in the foreground and waits for it: 0 with its exit status
// in *status, or -errno if it could not be started.
static int run(const char* prog, int argc, const char* const* argv, int* status) {
    // Held until it has the terminal: a program that ran first would
    // read as a background job, fail, and end at once.
    int id = loader_spawn(DISKFS_ROOT, prog, argc, argv, 0, 0, 1);
    if (id < 0) return id;
    tty_set_foreground(id);
    task_release(id);
    *status = task_join(id);
    task_wait_job(id);
    tty_set_foreground(current->job);  // clears up after one that died in raw mode
    return 0;
}

void init_main(void* arg) {
    (void)arg;
    if (!diskfs_ready()) {
        kprintf_color(COLOR_ERROR, "No disk, so no shell to start. Nothing more to do.\n");
        for (;;) task_sleep_ticks(0xFFFFFFFFu);
    }
    if (net_boot_wait()) {
        // One line either way, and two seconds at most once it has an
        // address to ask (looking pool.ntp.org up can take longer when
        // DNS does not answer). A disk without sntp just goes without.
        const char* argv[] = { "sntp", "-b" };
        int status;
        run(SNTP, 2, argv, &status);
    }
    for (;;) {
        const char* argv[] = { "saklas" };
        uint32_t start = timer_ticks();
        int status, e = run(SHELL, 1, argv, &status);
        if (e < 0) {
            kprintf_color(COLOR_ERROR, "init: cannot start %s: %s\n", SHELL, strerror(e));
            task_sleep_ticks(5 * TIMER_HZ);
            continue;
        }
        kprintf_color(COLOR_DIM, "[the shell ended (status %d); starting a new one]\n", status);
        // A shell that dies at once would otherwise restart in a tight loop.
        if (timer_ticks() - start < TIMER_HZ) task_sleep_ticks(TIMER_HZ);
    }
}
