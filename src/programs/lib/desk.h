// What the desktop's programs (kenoma, wfiles) share: which programs
// make windows of their own, and how a program or a document is opened
// from a window: a window program is started as it is, a text program
// in a terminal window that stays until a key is pressed, a document in
// Octavo in a terminal window.
#ifndef RINGTAIL_DESK_H
#define RINGTAIL_DESK_H

#include <stdint.h>

typedef struct {
    const char* prog;  // its name on the disk
    const char* name;  // what the desktop calls it
} desk_app_t;

extern const desk_app_t desk_apps[];
extern const int desk_napps;

// The window program with this file name (any case), or 0.
const desk_app_t* desk_app(const char* prog);
// Programs the launcher leaves out: the desktop's own.
int desk_hidden(const char* prog);
// Whether a name is a document Octavo opens from the desktop.
int desk_document(const char* name);
// Whether the file at path is something to run: an ELF program or a
// script (#!). A file with no dot in its name may be either, or data.
int desk_runnable(const char* path);

// Each a new job; the task id, or what refused it.
int desk_start(const char* prog);
int desk_open_document(const char* path);

// The desktop's colours, kept in DESK_COLOURS as two hex numbers, top
// and bottom: 0 when they were read, else why not.
#define DESK_COLOURS "/DESKTOP.CFG"
int desk_colours_load(uint32_t* top, uint32_t* bottom);
int desk_colours_save(uint32_t top, uint32_t bottom);

#endif
