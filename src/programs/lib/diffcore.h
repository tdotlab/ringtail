// The heart of diff: which lines changed between two files, and the three
// ways of printing that (normal, unified, context). No system calls, so the
// host tests can run it against GNU diff's answers.
//
// The edit script is the one GNU diff picks, not just any shortest one:
// the same Myers search (middle snakes, with GNU's way of breaking ties),
// the same discarding of lines that match nothing in the other file, the
// same sliding of each change up or down to line up with others. Two
// diffs that are both "right" can look very different, so matching it
// is what makes the output comparable.
#ifndef RINGTAIL_DIFFCORE_H
#define RINGTAIL_DIFFCORE_H

#include <stdint.h>

#define DF_ICASE    1  // -i: upper and lower case are the same
#define DF_SPACE    2  // -b: any run of blanks is one blank; blanks at the end do not count
#define DF_ALLSPACE 4  // -w: blanks do not count at all
#define DF_MINIMAL  8  // -d: discard nothing early (can give a smaller diff, slower)

typedef struct {
    const char* s;
    uint32_t len;  // without the newline
} df_line_t;

typedef struct {
    df_line_t* line;
    uint32_t n;
    int incomplete;  // the last line has no newline
} df_file_t;

typedef struct df_change {
    uint32_t line0, line1;  // where it starts in each file (0-based)
    uint32_t deleted, inserted;
    struct df_change* link;
} df_change_t;

// Splits buf (kept, not copied) into lines. 0, or -1 when memory runs out.
int df_split(df_file_t* f, const char* buf, uint32_t len);
void df_free_file(df_file_t* f);

// The edit script that turns f[0] into f[1], in order, or 0 when the files
// are the same (under flags). horizon is how many lines of the identical
// start and end stay in play: the context for -u and -c, 0 otherwise (it
// lets a change slide into them, as GNU's does). 0, or -1 when memory
// runs out.
int df_compare(const df_file_t f[2], unsigned flags, uint32_t horizon, df_change_t** out);
void df_free_script(df_change_t* s);

// Output goes through write(ctx, bytes, n).
typedef void (*df_write_t)(void* ctx, const char* s, uint32_t n);
void df_print_normal(df_write_t write, void* ctx, const df_file_t f[2], const df_change_t* script);
// The hunks only; the caller prints the two header lines first.
void df_print_unified(df_write_t write, void* ctx, const df_file_t f[2], const df_change_t* script, uint32_t context);
void df_print_context(df_write_t write, void* ctx, const df_file_t f[2], const df_change_t* script, uint32_t context);

#endif
