# Plan: MicroPython bytecode VM on the SNES (Calypsi port)

A plan for an AI agent. Goal: run the MicroPython **bytecode VM** on a real
SNES memory map, compiled with the Calypsi 65816 C toolchain, verified
headless in an emulator. No on-device compiler/REPL at first: Python is
compiled to bytecode on the host (`mpy-cross`), frozen into the ROM, and the
SNES only executes it.

Philosophy: **correctness before speed, verification before features.**
This is a hostile target (16-bit int, 3.58 MHz, 128 KB RAM, banked ROM) and
we have already been burned by compiler codegen bugs on this platform — every
milestone ends with an emulator-verified, scriptable test, never "it compiles".

## Project location

Create a NEW repository at `~/Development/micropython-snes` (do not grow this
game repo). Vendor MicroPython as a git submodule or subtree
(`https://github.com/micropython/micropython`); put the port in
`ports/snes/` inside it, or keep a thin repo that references it — agent's
choice, document it in `DECISIONS.md`.

## Toolchain: Calypsi

- Overview: https://www.calypsi.cc/ and
  https://github.com/hth313/Calypsi-tool-chains (binary releases, ~v5.14).
  Download the 65816 cross-compiler for Linux x86-64. Check the bundled
  license terms and PDF manuals (compiler, assembler, linker) in the release.
- It is an ISO C99 optimizing compiler, integers up to 64-bit `long long`,
  reentrant code, ELF/DWARF and raw/hex output, plus a command-line debugger.
- Expected binaries: compiler/assembler/linker for the 65816 target plus a
  C library. There is **no SNES board support package** — startup code,
  vectors and the linker memory description must be written by us. Reference
  material for what SNES init must do: `pvsneslib/pvsneslib/source/crt0_snes.asm`
  and `hdr.asm` in `~/Development/ibb_and_obb` (memory map, vectors,
  cartridge header at $00:FFB0-FFFF), plus
  https://bumbershootsoft.wordpress.com/2023/09/09/memory-layouts-and-linking-scripts-on-the-snes/
  for LoROM layout reasoning.
- Memory model: use the large code model (banked 32 KB LoROM banks) and place
  all data in WRAM ($7E0000-$7FFFFF, 128 KB). ROM up to 4 MB is available —
  code size is not the constraint, RAM and CPU are.

## Tools already available on this machine (use them)

### Headless SNES emulator: Mesen2 (`~/bin/Mesen`)

The complete working recipe (battle-tested in the ibb_and_obb project):

```bash
# One-time setup: Mesen's real config is read-only from the agent sandbox.
# Copy it, patch the copy, and point Mesen at it:
mkdir -p /tmp/mesencfg && cp -r ~/.config/Mesen2 /tmp/mesencfg/
python3 - <<'EOF'
import json
p = '/tmp/mesencfg/Mesen2/settings.json'
s = json.load(open(p, encoding='utf-8-sig'))   # note the BOM
s['Debug']['ScriptWindow']['AllowIoOsAccess'] = True   # enables io.* in Lua
s['Debug']['ScriptWindow']['ScriptTimeout'] = 30
json.dump(s, open(p, 'w', encoding='utf-8-sig'), indent=2)
EOF

# Run a ROM headless with a Lua test script:
env XDG_CONFIG_HOME=/tmp/mesencfg \
    DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 \
    SDL_VIDEODRIVER=dummy \
    ~/bin/Mesen --testrunner rom.sfc test.lua
# Exit code == the value passed to emu.stop(n) in Lua. Never pipe through
# `tail` etc. when you need the exit code.
```

Lua API facts (verified):
- `emu.addEventCallback(fn, emu.eventType.endFrame)` and
  `emu.eventType.inputPolled` work.
- `emu.read(addr, emu.memType.snesWorkRam)` /
  `emu.readWord(addr, emu.memType.snesWorkRam, signed)` read WRAM
  (addr = offset from $7E0000). `emu.write*` works too — useful to inject
  test inputs into the VM. `emu.memType.snesMemory` reads the CPU bus.
- `emu.takeScreenshot()` returns a PNG string; with AllowIoOsAccess you can
  `io.open(...):write(...)` it (not needed for this project — no PPU use).
- `emu.setInput({a=true,right=true}, 0)` works for controller port 0 only
  (port 1 is broken in testrunner — irrelevant here).
- Reference harness to copy: `~/Development/ibb_and_obb/tests/test_rom_smoke.py`
  and `tests/smoke_test.lua.in` (pytest wrapper, exit-code table, sym lookup).

### Host tools

- `python3` + `pytest` (test harness), `gcc` (build `mpy-cross` natively),
  ImageMagick/Pillow (irrelevant here), `git`.
- MicroPython's build needs `make` and python3 for qstr generation — present.

### Reference codebase

- `~/Development/ibb_and_obb` — working LoROM game: cartridge header layout,
  WRAM usage, Mesen test patterns. Its compiler (tcc-816) is NOT used here.
- The original game install (irrelevant for this project).

## Console/exit protocol (design, milestone 0 deliverable)

The SNES has no terminal. Define a fixed-address WRAM mailbox the Lua harness
polls; place it via the linker at a FIXED address so no symbol lookup is
needed:

```
$7E0100: u16 magic (0xCAFE = ROM alive)
$7E0102: u16 status (0 = running, 1 = all tests passed, 0xDEAD = panic,
                     others = test failure codes)
$7E0104: u16 write_index (monotonically increasing)
$7E0110: char ring buffer [3824 bytes] (mp_hal_stdout writes here)
```

Lua harness: each frame, drain new bytes from the ring into a host-side log
file; on status != 0, `emu.stop(status)`. pytest asserts exit code and
compares captured text against expected output. This gives MicroPython a
working `print()` from day one.

## Milestones

Each has a Definition of Done (DoD) that is a command an agent can run.

### M0 — Toolchain bring-up: C hello world on SNES
- Install Calypsi; write SNES startup (`crt0.s`: emulation→native mode,
  16-bit M/X, stack at $7E1FFF init, .data copy from ROM, .bss clear, jump to
  main; cartridge header + vectors), linker memory description (LoROM banks,
  WRAM data), and a `main()` that writes magic+"Hello from Calypsi" to the
  mailbox and sets status=1.
- Build the Lua harness + pytest wrapper (adapt from ibb_and_obb).
- DoD: `pytest tests/ -k hello` passes; captured log contains the string.

### M1 — Trust-but-verify the compiler
tcc-816 taught us: codegen bugs masquerade as logic bugs. Build a self-test
ROM exercising what MicroPython leans on: 32-bit and 64-bit arithmetic
(add/sub/mul/div/mod/shifts, signed and unsigned), pointer arithmetic across
banks, struct copy, switch statements, function pointers (the VM dispatch!),
`setjmp/longjmp` (MicroPython NLR fallback), varargs, recursion depth ~32,
`memcpy/memset/strcmp` from the Calypsi libc. Each test writes pass/fail to
the mailbox.
- DoD: `pytest tests/ -k selftest` → all subtests pass on the emulator.
- Any failure here: minimal repro, document in `DECISIONS.md`, work around
  (or report upstream — Calypsi's author is responsive).

### M2 — MicroPython compiles
- New port `ports/snes` modeled on `ports/minimal`, with `mpconfigport.h`:
  `MICROPY_ENABLE_COMPILER=0` (VM only), `MICROPY_NLR_SETJMP=1`,
  `MICROPY_FLOAT_IMPL_NONE`, `MICROPY_ENABLE_GC=1`, no filesystem, no
  modules beyond builtins; `mp_int_t` = 32-bit `long`; minimal
  `MICROPY_CONFIG_ROM_LEVEL_MINIMUM`.
- Key port decisions to research as you go: `mp_obj_t` representation
  (`MICROPY_OBJ_REPR_A`, 32-bit words — pointers on this target are 24-bit
  inside 32-bit longs; verify alignment/tagging assumptions hold), GC heap
  (~48-64 KB of WRAM bank $7E/$7F), C stack limit (`mp_stack_ctrl`) — the
  65816 hardware stack lives in bank 0; Calypsi likely uses a software
  parameter stack — find out and configure stack checking accordingly.
- Build system: don't fight `py.mk` — write a standalone Makefile listing
  `py/*.c` sources explicitly; run MicroPython's qstr/codegen scripts with
  the host preprocessor configured with target-matching defines.
- DoD: all of `py/` compiles and links with the Calypsi toolchain (warnings
  allowed, documented).

### M3 — First bytecode runs (the actual milestone)
- Host side: build `mpy-cross`; freeze `main.py` (`print("hello from
  micropython on snes")` plus arithmetic/loop/exception smoke lines) via the
  frozen-module mechanism (`mpy-tool.py`) into C arrays.
- Target side: `mp_init()` → execute frozen module → on uncaught exception
  write 0xDEAD + traceback text to mailbox.
- DoD: pytest asserts exit 1 and exact expected stdout. THIS is the
  "micropython runs on a SNES" moment — tag it.

### M4 — Test suite subset
- Curate ~30 files from `micropython/tests/basics/` that fit no-float/no-fs
  constraints; freeze them with their `.exp` expected outputs; a frozen
  runner executes each and diffs output on-device or ships output to the
  host for diffing (prefer host diffing — less code on target).
- DoD: pytest reports N/N passing; failures triaged in `DECISIONS.md`.

### M5 — Measure & document
- Benchmark: bytecodes/sec (tight integer loop), GC pause time, RAM
  watermark, ROM size, boot-to-first-print frames. Write `README.md` with
  honest numbers and build instructions.
- DoD: README exists; numbers reproduced by `pytest -k bench`.

### M6 — Stretch: make it visible
- A tiny `snes` builtin module in C: `snes.poke_vram()`, `snes.oam()`,
  `snes.pad()`, `snes.wait_vblank()`. Frozen Python draws something and reacts
  to input at whatever frame rate it manages. Screenshot via the Mesen
  harness for the README. This is the demo that justifies "heroic".

## Risk register (ranked)

1. **`mp_obj_t`/word-size assumptions** — MicroPython is tested on 32/64-bit;
   16-bit `int` with 32-bit `mp_int_t` is exotic. Expect subtle breakage where
   the code assumes `int` ≥ 32 bits despite using `mp_int_t` mostly. Mitigate:
   compiler warnings at max, M1-style targeted tests of `py/objint.c`,
   `py/binary.c`, `py/smallint.h` behavior.
2. **qstr build pipeline with a non-GCC compiler** — preprocessing tricks in
   `makeqstrdefs.py`. Mitigate: run preprocessing with host gcc using
   `-D`-matched target config; only final compile uses Calypsi.
3. **Calypsi C library gaps** (printf variants, assert) — Mitigate: MicroPython
   needs little libc; provide stubs; route all output through `mp_hal_stdout`.
4. **Code size vs banked calls** — large model trampolines may bloat; if a
   bank overflows, split `py/*.c` across sections explicitly in the linker
   file. 4 MB budget means this is annoying, not fatal.
5. **Performance** — accept up front: this will be ~kHz-level bytecode rates.
   Not a goal to fix in this plan; document it.
6. **Calypsi licensing** — binary freeware for hobby use; confirm the license
   text in the release before publishing the repo.

## Working agreements for the agent

- Keep `DECISIONS.md` (dated entries: what was decided, why, what failed).
- Commit at every green DoD; tag M3.
- Every claim about target behavior must come from an emulator run, not from
  reading code. When C behavior looks impossible, disassemble (Calypsi can
  emit assembly listings) before changing logic — see the tcc-816 `||`
  miscompile incident in this repo's history (commit e96b510) for why.
- Ask the user only when blocked on: Calypsi license acceptance, large
  upstream-MicroPython forks, or abandoning a milestone.
