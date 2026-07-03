# A Fable About MicroPython on the SNES — source notes for the article

Raw material compiled by Claude Fable 5 on 2026-07-03, from the git log,
DECISIONS.md, and this session's transcript. Everything dated/hashed below is
verifiable in the repo. Model attribution for the June sessions is
reconstructed (see §2) — **Fabian: please verify against your own records
before publishing.**

---

## 1. The project in one paragraph

Run real, unmodified-where-possible MicroPython (v1.28.0) on a Super
Nintendo: a 3.58 MHz 65816 CPU, 128KB of WRAM, a 56KB GC heap squeezed into
one memory bank, 16-bit `int` and `size_t`, no data alignment guarantees, no
filesystem, and a C compiler (Calypsi 5.17) that nobody has ever pushed this
hard. Development is emulator-driven: every claim is verified headless in
Mesen2 via a "mailbox" protocol (a ring buffer in WRAM that a Lua harness
drains into a log file, byte-for-byte asserted by pytest). The rule from day
one: *"End every claim about on-target behavior with an actual emulator run,
never 'it compiles'."*

## 2. Cast and timeline

Attribution reconstruction: commit `fe36b27` (M3 WIP) is hand-marked
"[last fable commit]" in its subject line; the project brief for the June 13
continuation session specified `Co-Authored-By: Claude Opus 4.8` trailers;
the July 2 session began with `/model → Fable 5` and "Please make shit work
now!". Note all commits *mechanically* carry Fable trailers because the
trailer text was copy-pasted between sessions — the subject-line marker and
the session briefs are the real evidence.

| Date | Commits | Model (reconstructed) | What happened |
|---|---|---|---|
| Jun 12, ~18:35–19:36 | `2a1bb3f`…`83de810` | **Fable 5** (session 1) | M0 hello-world ROM, M1 compiler self-test (41 checks, found 2 Calypsi bugs), M2 all of `py/` compiles and `mp_init` runs on the SNES |
| Jun 13, ~09:16 | `fe36b27` | **Fable 5** | M3 WIP: Python bytecode executes, but blocked on the vm.c miscompile — session ends here ("[last fable commit]") |
| Jun 13, 12:18–18:11 | `b8b9ce8`…`a70b330` | **Opus 4.8** | M3 green (later revealed: by layout luck). Root-caused M4 blocker as "caller-saved pseudo-register loss" (later revealed: wrong). Ported enough of py/ to a *second* compiler (vbcc) as an escape route — vbcc turned out layout-sensitive too. Ran negative spikes: shrinking the VM, -O sweeps, `volatile ip` — all "shuffle the lottery." Final recorded conclusion: building the per-opcode VM split is "very unlikely to pay off"; remaining options "report upstream, spike llvm, or hold at M3." Project effectively stuck. |
| *3-week gap* | | | |
| Jul 2, 13:44 | `703bcd7` | **Fable 5** (session 2, one continuous run to Jul 3) | **M4 GREEN + the real root cause** (see §3). Split VM shipped. |
| Jul 2, 18:06 | `05d4eec` | Fable 5 | **M5 GREEN**: interactive REPL — MicroPython's compiler runs *on the 65816* (+2 more compiler bugs found) |
| Jul 2, 18:43 | `2999a32` | Fable 5 | **M6 GREEN**: REPL on the TV — PPU text console, on-screen keyboard, joypad input; a scripted controller types `1+2` and the SNES answers `3` |
| Jul 2, 19:58 | `2de53f6` | Fable 5 | Floats + framebuf (+ the bitfield-miscompile discovery) |
| Jul 3, 00:28 | `e8a1411` | Fable 5 | nano-gui groundwork + "a compiler-bug safari" (GC heap bank-loss, root-scan, FAM-drop, super() alignment…) |
| Jul 3, 01:07 | `5e1efe5` | Fable 5 | **The negative-Y bug** — arguably the deepest single find of the project |
| Jul 3, 07:44 | `b7d010b` | Fable 5 | The ora-clobber class + framebuf overflow fixed; nano-gui demo renders Label and Meter on target; 6/7 tests green |

Stats for the July session alone: 8 commits, ~8,500 lines added across 67
files (excluding the submodule), a 1,925-line patch file against upstream
MicroPython, 4 ROM targets, 7 pytest suites, 2 automated codegen checkers,
~15 distinct root-caused compiler/port bugs.

## 3. The turning point: what M4 actually was

This is the heart of the Fable-vs-Opus story, and it should be told
carefully because Opus's work was *good* — it just anchored on a wrong
theory.

**The symptom (June):** any nontrivial Python — recursion, method calls,
list iteration — failed with garbage exceptions, `NotImplementedError:
opcode`, or hangs. Failures were deterministic per build but *chaotic across
builds*: change anything (add a print, reorder a function) and the failure
moved. This is the signature that drove everyone mad.

**Opus's theory (June 13, commits `da45583`, `a881634`):** Calypsi's `_Dp`
pseudo-registers are caller-saved, and the giant 21KB `mp_execute_bytecode`
function overwhelms the register allocator, which loses the cached bytecode
pointer across calls. Supporting evidence was real: the docs do describe
`_Dp` as caller-saved, the function is enormous, and disassembly showed
`ip` cached in `_Dp`. Every workaround was tried and honestly recorded as
failing. The conclusion — "even the full split won't help, because
`volatile ip` (its core mechanism) only shuffles" — was logically neat and
wrong, because `volatile ip` was *not* a valid proxy for the split (it was
itself confounded by a second Calypsi bug: volatile stores to stack locals
get dropped inside setjmp functions).

**What it actually was (July 2, ~90 minutes into the Fable session):**
`MICROPY_OBJ_REPR_B` requires every Python object to sit at an even address
(bit 0 is the integer tag). The port had used the documented ports/pic16bit
trick — `__attribute__((aligned(2)))` on the `mp_obj_base_t` struct member.
**Calypsi silently ignores `aligned()` on struct members and types; it only
honors it on variable definitions.** So every ROM object — `print` itself,
exception types, frozen strings — had a memory-layout-dependent *parity*.
Shift the image by one odd byte and a different set of objects becomes
odd → misread as tagged small integers → chaos. On both compilers (vbcc had
the macro defined empty). Every prior workaround had merely re-rolled the
parity dice, which retroactively explains every data point Opus collected.

The discovery chain, for the article: split VM built → validated logically
on a host build (unix port with vm.c swapped) → on target, first `print`
failed with `TypeError: object not callable` → opcode trace showed the
bytecode was fine → so the *object* was wrong → link map showed
`mp_builtin_print_obj = 0xc27e3b` — an odd address → three-line compiler
experiment proved member-attribute alignment is ignored, variable-position
works. Fix: inject the attribute at variable position into every object
definition macro (and mpy-tool's generated objects), plus
`tools/check_obj_align.py`, which parses the link map and *fails the build*
if any object symbol is odd. The lottery was over the same afternoon.

A scientific control worth quoting: with alignment fixed, the *original*
monolithic vm.c was retested — it ran recursion, classes, and generators,
but still hung in nested try/finally (Opus's bug 2, the setjmp/volatile one,
is real!). So the split VM wasn't wasted — it dodges a genuine second bug —
but alignment was ~95% of the mystery.

## 4. The complete bug ledger

Every entry root-caused with an on-target reproducer. (F) = found in the
Fable July session, (O) = found/documented in June.

**Calypsi 5.17 compiler bugs:**
1. (O) Variadic 16-bit last named parameter clobbered by frame setup.
2. (O) Volatile stores to stack locals silently dropped inside setjmp
   functions.
3. (O) Large switch dispatch "lands in the weeds" (needs
   `--force-switch if-else`).
4. (F) `__attribute__((aligned))` silently ignored on struct members/types;
   honored only at variable position. **The M4 root cause.**
5. (F) ICE ("Non-exhaustive patterns in ControlFlowOptimize.hs") compiling
   py/lexer.c at any -O ≥ 1.
6. (F) Bitfields with 32-bit storage units miscompile on read-modify-write
   (`res.p.exp += dec_exp` zeroed a float's exponent → float literals with
   fractions parsed as denormals; `print(float(3))` hung) — and can ICE.
7. (F) Flexible-array-member initializers silently emit *no data* — every
   const tuple was a 6-byte header followed by whatever the linker placed
   next (`sys.implementation[0]` returned `<class 'tuple'>`). (Type-object
   slots had been quietly hit by the same bug earlier.)
8. (F) Third pointer parameter loses its bank byte in call marshalling —
   `gc_setup_area(area, start, end)` received `end = $0000E000` instead of
   `$7FE000`. **The heap handed out bank-0 pointers; `bytearray(4000)`
   zero-filled the C stack.** Codegen varies per whole-file compilation, so
   this function had compiled correctly for weeks.
9. (F) **The negative-Y bug**: `p[-1]` on a far pointer sometimes compiles
   to `ldy ##-2` + `lda/sta [dp],y`. The 65816 adds Y to the 24-bit base as
   an *unsigned* 16-bit value — the access lands one bank (64KB) away.
   Reads return garbage; writes vanish into ROM. Caught red-handed via a VM
   stack tracer showing `ROT_THREE` turn `[5,5,0]` into `[5,garbage,0]`.
   18 instances in the split VM alone.
10. (F) **The ora-clobber**: `while ((x = f(...)) != 0)` can compile to
    `jsl f; stx t; ora t; beq…; stx hi; sta lo` — the accumulator holds
    `low|high` from the null test and gets stored as the value's low word.
    `list(map(str,(7,)))` returned `[True]` because a perfectly good string
    pointer was OR-mangled into an odd (tagged) value. Register-pressure
    roulette decides which call sites are broken in any given build.
11. (F) Calypsi ignores `noinline` (like `noreturn`) *and* ignores
    `volatile` on locals — so both standard mitigations for 8/9/10 fail;
    the working hammer is calling helpers through **volatile function
    pointers** (used for `gc_alloc_attempt` and `mp_store_obj_result`).
12. (F) C stack locals have no alignment: any `mp_obj_*_t` built on the
    stack can be odd → REPR_B misreads it. Bit zero-arg `super()`
    ("'int' object has no attribute '__init__'") and C-stack iter_bufs.

**Upstream MicroPython bugs surfaced by the 16-bit platform (upstreamable):**
13. (F) py/parse.c chunk allocator: `size_t` header fields put parse nodes
    at 2-mod-4 addresses when size_t is 16-bit and structs are packed →
    `MP_PARSE_NODE_IS_STRUCT` misclassifies every struct node → heap-layout-
    dependent garbage parse trees. (The REPL's "NameError: name
    'micropython' isn't defined" mystery.)
14. (F) py/gc.c root scan: `ptrs + root_start / sizeof(void *)` truncates,
    and the root section contains pointers at 2-mod-4 offsets on packed
    16-bit-size_t targets → collector freed live objects
    (`gc.collect()` erased your variables).
15. (F) extmod/modframebuf.c: pixel index `x + y*stride` computed in `int`
    overflows at y ≥ 128 on a 256-wide buffer → out-of-bounds writes. On
    the SNES this corrupted the heap every time text was drawn on the lower
    half of the screen.

## 5. Methodology: what the record shows about the two approaches

*(This is the Fable-vs-Opus material. Both sessions kept the same excellent
hygiene — DECISIONS.md, emulator-verified claims, milestone tests — which is
exactly what made the later reconstruction possible.)*

**The June (Opus) pattern:** careful, incremental, workaround-oriented.
Characterize the symptom, try the standard toolbox (optimization levels,
volatile, code shuffling, a second compiler), record everything honestly,
and when the toolbox is exhausted, conclude the problem is upstream and
recommend escalation. The vbcc port — a second full compiler bring-up in an
afternoon — is genuinely impressive execution *within* the theory. The
failure mode was epistemic: once "register allocator loses ip in the giant
function" was written down as root-caused (`a881634`, "no simple
workaround"), every subsequent experiment was interpreted through it, and
the decisive counter-experiment (does a *small* function mishandle objects
too?) was never run because the theory said function size was the variable.

**The July (Fable) pattern**, as visible in the transcript:
- **Differential debugging as a reflex.** First move on any target failure:
  build the identical logic on the host (unix MicroPython with the split VM
  swapped in, later with matching config flags) — this separated "my
  transformation is wrong" from "the compiler is lying" in minutes, dozens
  of times.
- **Distrust of inherited conclusions.** The session began by re-deriving
  why `volatile ip` was *not* a valid proxy for the split — directly
  contradicting the standing June conclusion — and the user's pushback
  ("splitting… would not fix the problem?!") was taken as a prompt to
  re-examine rather than defend.
- **Byte-level ground truth.** When prints disagreed with theory, the next
  step was dumping raw bytes via unions (which exposed that the pointer-
  printing casts were *themselves* miscompiled — the debugging tools lied).
- **Purpose-built microscopes.** A 30-line C probe ROM linking the real
  gc.o (2-second iteration); a VM opcode tracer; a VM stack-state tracer;
  Mesen Lua PC sampling symbolized against the link map; write/exec memory
  traps that dumped the CPU stack at the moment of corruption.
- **Tripwires over vigilance.** Every root-caused bug class got an
  automated checker wired into the build (`check_obj_align.py` fails odd
  objects; `check_neg_index.py` fails negative-Y indexing and warns on
  ora-clobber shapes). The philosophy: never trust the roulette twice.
- **The REPL as a lab.** Once M5 landed, the interactive REPL (with the
  whole GUI package tree frozen in) became the primary debugging
  instrument for everything after it — Python one-liners as unit probes
  against the real target.

**Fable's own missteps (for an honest article):**
- Misread the link map twice (missed library symbols; wrong nearest-symbol
  logic) and chased a "wiper in a string literal" that was actually
  `memset` doing its job.
- Declared `memcpy` broken from a probe that read uninitialized memory
  (false alarm, retracted after a controlled repro).
- Dismissed truncated pointer values in early debug prints as "probe
  artifacts" — they were the real bank-loss bug, caught properly only a day
  later.
- Triaged the flagged `mp_get_buffer` ora-clobber site as "benign,
  truthiness only" — it was silently corrupting the heap under every glyph
  blit. (The lesson is now written into the checker's output text.)
- Multiple wasted rebuild cycles from stale build artifacts and a
  tmpfiles-cleaned config directory — mundane, but real hours.

## 6. Things built (that didn't exist before July 2)

- `port/vm_split.c` (1,228 lines) — per-opcode VM: every bytecode handler
  its own function, dispatched via a 256-entry table, all VM state in a
  pointer-accessed context struct, zero mutable locals in the setjmp
  function. Immune by construction to three separate compiler bugs.
- `make mpyrepl` — interactive REPL ROM: MicroPython's lexer/parser/
  compiler running on the 65816, stdin over a new mailbox input ring.
- `snes/console.c` + `snes/oskb.c` — mode-0 PPU text console (white on C64
  blue, public-domain font8x8) and a name-entry-style on-screen keyboard;
  joypad-scripted pytest types `1+2`, Start, and asserts the SNES answers
  `3`. One ROM serves CI (mailbox) and a real TV (would boot from a
  flashcart).
- Floats (Calypsi's single-precision libm), `framebuf`, complex/cmath,
  frozen *package* imports.
- `port/modsnesfb.c` — 256×192, 16-color framebuffer on SNES video via the
  unique-tile trick (tilemap of 768 distinct tiles = quasi-linear bitmap),
  GS4-nibbles→bitplanes conversion, vblank-chunked DMA. nano-gui's 4-bit
  LUT driver model maps 1:1 onto SNES hardware palettes.
- Vendored peterhinch/micropython-nano-gui (one documented 2-line deviation
  for the missing `uctypes`), running verbatim on the host testbed and, on
  target, through imports, color setup, fonts, `CWriter`, `Label`, and
  `Meter`.
- Two codegen checkers wired into every link.

## 7. Current state (updated through M8, 2026-07-03 late night)

pytest **8/8 green**: hello, selftest, M3 frozen Python, M4 (recursion/
classes/generators/closures), M5 REPL session, M6 joypad session, M7
nano-gui, M8 Stage. The `super()` failure that ends section 7's earlier
draft was root-caused as **the GC marker bug** (ledger #16): the collector
scanned heap children at 4-byte strides only, so pointers at 2-mod-4
offsets were invisible and live objects were collected — the same bug
later explained a scrambled palette (bytearray items pointer at offset 9).

Since then, the same day:
- **The upstream suite ran on the console**: 571 tests from
  micropython/tests/basics, fed through the REPL's raw mode in Mesen —
  **430 pass = 91.9% of the 468 the reference can run**. The triage arc
  (76→83→87→91.9%) burned one new compiler bug per step, including the
  showstopper `!(k>=k) || f(x)` fold that had silently turned every set
  literal into a dict.
- **M8: the Stage game library** (python-ugame, the CircuitPython-era
  tile/sprite engine) runs with its Python API intact — but its entire
  per-pixel C compositor deleted, because Bank/Grid/Sprite map 1:1 onto
  what the PPU does in silicon (VRAM charsets, the BG1 tilemap, OAM).
  A Stage bank is 2048 bytes; an SNES 4bpp charset of 64 8x8 tiles is
  2048 bytes; one VRAM upload serves both backgrounds and sprites.
  Six bouncing sprites over a brick arena: **0.84 fps of pure Python
  game logic** (a nano-gui frame was ~10s), boot-to-first-frame 20s
  (was 117s), per-frame cost now scales with sprite count, not pixels.
  Screenshot: `stage_snes.png`; measurement: `tools/measure_stage.py`
  (frame-stamped mailbox log under Mesen).
- M8 bring-up added **Calypsi bug #23** to the ledger — `aligned(2)` is
  ALSO silently ignored at variable position when the declared type is an
  anonymous struct. Frozen tuples landed at odd addresses, whose tagged
  pointers decode as small ints under REPR_B: the demo's ball table
  failed as "'int' object isn't iterable", "'bool' object isn't
  iterable", or "too many values to unpack" depending on which build you
  ran. Alignment remains the project's one recurring villain: it was the
  months-long M4 mystery, and it came back wearing a typedef.

## 8. Details and color for the article

- The SNES answers `1+2` on screen at `>>> ` in white-on-blue, typed with a
  D-pad on a name-entry keyboard grid (A=type, B=delete, Y=symbols,
  Start=run). Screenshots exist (Mesen `emu.takeScreenshot` from Lua).
- One build of the REPL ROM had checksum **0xFABE**. Nobody planned that.
- The heap bug's smoking gun: `bytearray(4000)` didn't fail — it *zeroed
  the C stack*, because the allocator had been quietly handing out pointers
  into bank 0 (the CPU's own stack page) all along.
- The negative-Y bug means, on a 65816, `array[-1]` can read memory 65,535
  bytes *forward*. The fix's name in the code is `vm_ptr_at`.
- Total upstream-relevant findings: 3 MicroPython patches worth submitting
  (parse chunk alignment, gc root scan, framebuf 16-bit overflow) and 9+
  Calypsi bugs worth reporting (hth313), several with minimal .lst
  reproducers already in the repo history.
- The port's DECISIONS.md is itself a character in the story: the June
  entries record the wrong theory with complete honesty, which is exactly
  what made it possible to overturn cleanly.
- Timeline compression for the punchline: the problem that ended the June
  sessions ("hold at M3, report upstream") fell in roughly the first 90
  minutes of the July session; by dinner the same day, a controller was
  typing Python into a television.
