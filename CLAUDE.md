# RingTail: notes for agents

Read these before changing code:

- `docs/ARCHITECTURE.md`: the map. What each part is, where it lives,
  how a system call, a key press, a packet and a window travel, the
  locking rules, and which file to start in and which test covers it.
- `TODO.md`, "House rules" and "Things that bit before": how work is
  done here and the mistakes already made once.
- `README.md`, "Design in brief" and "Limits": how each subsystem behaves.
- `PROGRESS.md`: the history, release by release.

`make test` runs everything (host tests, then the QEMU suite); it takes
about ten minutes. Run it before committing.

When a change moves a boundary the map describes (a new subsystem, a
new lock, a file that changes job), update `docs/ARCHITECTURE.md` in
the same commit.
