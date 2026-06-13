# vbcc spike

Evaluating vbcc 65816 (r2, Oct 2025) as an alternative to Calypsi, which
miscompiles MicroPython's VM (caller-saved pseudo-register loss across
re-entrant calls — see ../DECISIONS.md).

Why vbcc is promising:
- Has callee-saved registers (r16-r27 preserved across calls) — the exact
  thing Calypsi lacks for this bug class.
- Supports near/far/huge pointer models (huge can cross 64K banks, which
  Calypsi's large model can't).
- Free for non-commercial use.

Status:
- [x] toolchain -> SNES -> Mesen pipeline validated: `hello.c` runs, prints
      to the $7FE000 mailbox, passes our harness (exit 1).
- [x] vm.c (the file Calypsi miscompiles) compiles cleanly with vbcc.
- [ ] full MicroPython core build + link with vbcc (M2-equivalent).
- [ ] run the programs that break Calypsi (recursion/iteration/methods).

Toolchain (gitignored): fetch vbcc65816_r2.zip from
http://www.ibaug.de/vbcc/vbcc65816_r2.zip into vbcc-toolchain/.
Build a test: VBCC=.../vbcc PATH=.../vbcc/bin:$PATH vc +snes-his -O2 hello.c -o hello.bin
