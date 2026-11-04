// Spin forever without a system call, so only the timer can take the
// CPU away. The test suite runs it in the background to check that
// preemption keeps the shell responsive.
int main(void) {
    volatile unsigned n = 0;
    for (;;) n++;
}
