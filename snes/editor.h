// Full-screen text editor for the workstation ROM: rows 0-20 viewport,
// row 21 status, rows 22-27 the on-screen keyboard. Typing via oskb;
// Select (the only non-typing oskb key) opens the menu, which also toggles
// cursor-movement focus. Pure C, edits a flat 8KB WRAM buffer, saves to
// the battery-SRAM file store.
#ifndef SNES_EDITOR_H
#define SNES_EDITOR_H

#include <stdint.h>

enum {
  ED_EXIT = 0, // saved-or-discarded, back to the manager
  ED_RUN = 1,  // buffer was saved; caller runs it, then re-enters
};

// load name from SRAM if it exists (else start empty) and run the UI loop
uint8_t editor_session(const char *name);

// the shared 8KB buffer (also reused by os_main to load files for running)
char *editor_buffer(uint16_t *len_out);

// prompt for a new file name (row 19 + oskb); 1 = name filled (".py"
// appended if missing), 0 = cancelled
uint8_t editor_prompt_name(char *out, uint8_t maxlen);

#endif
