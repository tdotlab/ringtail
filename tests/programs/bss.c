// Program image sections: initialized data, zeroed .bss, read-only
// constants, and a .bss big enough to need many pages.
#include "ulib.h"

static int initialized = 12345;
static const char constant[] = "read-only";
static uint8_t zeroed[256 * 1024];

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    check(initialized == 12345, "initialized data has its value");
    check(strcmp(constant, "read-only") == 0, "read-only data is intact");
    int clean = 1;
    for (uint32_t i = 0; i < sizeof zeroed; i++) if (zeroed[i]) { clean = 0; break; }
    check(clean, "256 KB of .bss starts zeroed");
    for (uint32_t i = 0; i < sizeof zeroed; i++) zeroed[i] = (uint8_t)(i ^ (i >> 8));
    int same = 1;
    for (uint32_t i = 0; i < sizeof zeroed; i++) if (zeroed[i] != (uint8_t)(i ^ (i >> 8))) { same = 0; break; }
    check(same, ".bss is writable and keeps what we write");
    initialized++;
    check(initialized == 12346, "data is writable");
    return check_summary("bss");
}
