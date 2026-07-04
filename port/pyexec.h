// Shared exec/REPL engine for the interactive ROMs (mpyrepl, mpyos).
// Extracted from repl_main.c so the workstation ROM can reuse the exact
// proven compile-and-run path and the dual mailbox+console I/O.
#ifndef PORT_PYEXEC_H
#define PORT_PYEXEC_H

#include <stddef.h>

#include "py/parse.h"

// dual-channel output: mailbox ring (harness) + PPU console (TV)
void pyexec_putc(char c);
void pyexec_puts(const char *s);

// blocking merged input: mailbox stdin first, then the on-screen keyboard,
// paced one poll per frame by console_flush()
int pyexec_getc(void);

// compile + run a buffer in the CURRENT interpreter, nlr-protected;
// exceptions are printed, not propagated
void pyexec_str(const char *src, size_t len, mp_parse_input_kind_t kind);

// banner + interactive loop (incl. the raw mode used by the upstream-test
// runner). RETURNS on ^D at the start of an empty line; the caller decides
// what happens next (mpyrepl: mb_finish; mpyos: back to the file manager).
// pad_eof: honor a joypad ^D (oskb Select) as exit too. Pass 0 when there
// is nothing to return to (mpyrepl) — Select is then ignored and only a
// scripted stdin ^D ends the loop; the banner drops the exit hint.
void pyexec_repl(int pad_eof);

#endif
