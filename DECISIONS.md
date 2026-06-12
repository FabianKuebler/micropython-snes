# Decisions log

## 2026-06-12 — Repo location

The plan says `~/Development/micropython-snes`; the agent was invoked in
`~/Development/micropython-sne` (no trailing "s") which contained only the
plan file. Used that directory as the repo root rather than creating a
confusingly-similar sibling.

## 2026-06-12 — Calypsi 5.17, installed locally, license

Downloaded `calypsi-65816-5.17.deb` (Linux x86-64) from
https://github.com/hth313/Calypsi-tool-chains/releases/tag/5.17 and extracted
it (no root) to `toolchain/` (gitignored, 140+ MB; re-fetch with
`tools/fetch_toolchain.sh`). License (`toolchain/usr/local/lib/calypsi-65816-5.17/LICENSE`):

- Permitted: personal non-commercial use, hobby, education, producing
  software for vintage/retro systems. This project qualifies.
- Forbidden: redistribution of the toolchain (hence gitignored) and
  **publishing benchmark comparisons of Calypsi against other compilers**.
  M5 numbers describe MicroPython-on-SNES performance only, never
  compiler-vs-compiler comparisons.

## 2026-06-12 — HiROM instead of LoROM (deviation from plan)

The plan suggested LoROM banking. Calypsi 5.17 ships first-class SNES
support: `--target=SNES`, prebuilt SNES C libraries
(`clib-lc-ld-snes[-noppu].a`), and a `linker-rules/snes-HiROM.scm` memory
description. That stock support is **HiROM** (banks $C0+, contiguous 64 KB
banks, near code scattered into $C0:8000-FFAF so it appears in the bank-0
mirror at $00:8000+). Fighting this to make LoROM would mean rebuilding the
C library and rewriting the linker rules for zero benefit — HiROM gives
*larger* contiguous banks, which is strictly better for the large code
model. Decision: HiROM. Mesen auto-detects it from the header at $FFB0/$FFC0.

## 2026-06-12 — Code/data model, PPU multiplier

- `--code-model=large --data-model=large`: data pointers are 24-bit (stored
  as 32-bit), matching the planned `mp_obj_t` representation; all of WRAM
  $7E2000-$7FFFFF is far data, GC heap fits there.
- `--no-ppu-mul` for now (library `clib-lc-ld-snes-noppu.a`): the SNES target
  otherwise uses the PPU multiplier registers for speed. Correctness before
  speed — revisit after M1 validates the toolchain, the PPU-mul variant can
  be swapped in later for free perf.
- Startup code: Calypsi's stock `cstartup.s` (in the C library) already does
  emulation→native, 16-bit M/X, stack init from `.sectionEnd stack`, direct
  page setup, `.data` copy/`.bss` clear via `__initialize_sections`, heap
  init, `main()` call. We do NOT write our own crt0; we only provide the
  SNES cartridge header + interrupt vectors (`snes/header.s`) which the
  stock linker rules expect as sections `snesheader`/`snesheaderextended`,
  and the reset vector comes from cstartup's `reset` section at $FFFC.

## 2026-06-12 — Mailbox vs Calypsi RAM map

Stock `snes-HiROM.scm` puts stack+near data at $0100-$1FFF (bank-0 mirror of
$7E0100-$7E1FFF) — exactly over the planned mailbox at $7E0100. Our
`snes/linker.scm` moves near RAM to $1000-$1FFF (stack $800 + near data) and
leaves $0100-$0FFF unplaced; the mailbox lives there at fixed addresses with
no linker symbol needed:

```
$7E0100 u16 magic       0xCAFE = ROM alive
$7E0102 u16 status      0 running, 1 all passed, 0xDEAD panic, else fail code
$7E0104 u16 write_index monotonic, wraps at 65536
$7E0110 u8[3824] ring   stdout ring buffer ($7E0110-$7E0FFF)
```

Mesen exit code == status (pytest asserts == 1 for pass). Test runner
timeout → Lua exits 99, ROM never boots → 98, ring overflow → 97.

Caveat: exit code 1 is also a generic "Mesen crashed" code, so pytest
asserts the captured ring-buffer text as well, never the exit code alone.

## 2026-06-12 — M1 findings (compiler trust-but-verify)

First run of the self-test ROM produced 8 failures; triage found two genuine
Calypsi 5.17 bugs, one documented-behavior misunderstanding, and one
arithmetic error in our own expected value (u64.shl — the compiler was
right). All verified in the emulator, all worked around; `pytest -k selftest`
is green (41 checks). Consider reporting the two bugs upstream (hth313).

### Far pointers do not cross banks (documented, not a bug)

Manual §5.3.3: far objects are limited to 64K-1 and pointer arithmetic does
not carry into the bank byte; `__huge` would, but is disabled in the large
data model (enabling it requires rebuilding the C library with 32-bit
size_t). Consequences for the port:

- **The GC heap must live inside a single 64K bank** — plan: bank $7F
  ($7F0000-$7FFFFF), which comfortably fits the planned 48-64 KB heap.
- No C object, ever, may straddle $7E/$7F. Linker sections won't create such
  an object on their own (far section placement respects this), but
  fixed-address tricks must respect it manually.

### Calypsi bug 1: variadic 16-bit last named parameter clobbered

In a variadic function whose last named parameter is 16-bit (passed in the
accumulator), the va_start/frame-setup codegen executes `tsc` before the
parameter is saved, destroying it (all -O levels with a frame; repro:
`bugs/calypsi-varargs-int-param-clobber.c`, compare `vsum_int` vs
`vsum_copy`). Pointer and 32-bit last parameters are passed in pseudo
registers and are safe — which covers MicroPython's dominant
`(..., const char *fmt, ...)` shape. Workaround where a 16-bit count is
wanted: `int count = n;` before `va_start` (forces a register save).
Emulator-verified by va.long/va.longlong (workaround) and va.fmt (safe
pointer shape).

### Calypsi bug 2: volatile stores to stack locals dropped

In a multi-branch setjmp function shaped like our setjmp test, assignments
to a `volatile` *stack* local between `setjmp` and `longjmp` are silently
not emitted — at -O0, -O1 and -O2 (repro:
`bugs/calypsi-volatile-auto-store-dropped.c`: `m = 1` compiles to `lda ##1`
with no store; simpler functions like `sj2`-style single-branch are
correct). Volatile statics/globals are always correct. Consequences:

- Our code: never rely on volatile autos to survive longjmp; use static
  volatile (done in m1_selftest).
- MicroPython (M2/M3): NLR call sites that keep state in volatile locals
  across `nlr_push` must be audited; the bug is shape-dependent, so each
  nlr-using function we actually execute gets verified behavior-level by
  the M3/M4 tests anyway. setjmp/longjmp control flow itself (including
  longjmp across 3 intermediate frames, distinct return values) is correct.

## 2026-06-12 — M0 green

`pytest tests/ -k hello` passes: Calypsi-built HiROM image boots in Mesen
testrunner, stock C-library startup runs (data init verified via initialized
global, BSS clear verified via probe array), mailbox protocol works
end-to-end. Mesen logs "Uninitialized memory read $000008-$00000F" at boot —
that is cstartup touching the direct-page pseudo registers (`_Vfp`/`_Dp`)
before writing all 4 bytes; harmless.
