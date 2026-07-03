# Decisions log

## 2026-07-04 — M9: mpyos, the all-in-one workstation ROM (13/13 green)

`build/mpyos.sfc` boots into a C file manager over **32KB battery SRAM**
(persists as the emulator's .srm), edits Python files in a C full-screen
editor typed on the joypad keyboard, runs them (or the frozen Stage demo)
in a fresh interpreter per run, and drops into the shared REPL on Select.
Key structure:

- **SRAM**: snes/header_sram.s (separate header variant, cart type $02 /
  RAM $05, so the six existing ROM images stay byte-identical) +
  snes/sram_fs.c: 16-dirent table + compacted data area, all access via
  window-aware helpers (HiROM SRAM = 8KB windows at $30-$33:$6000-$7FFF;
  far pointers must not cross banks). Mesen's testrunner persists
  Mesen2/Saves/<rom>.srm — proven by running sramtest twice (a file that
  spans the window-0/1 boundary survives write, compaction, and reboot).
- **pyexec extraction**: repl_main.c's engine (dual mailbox/console I/O,
  compile-and-run, interactive loop incl. raw mode) moved verbatim to
  port/pyexec.c, shared by mpyrepl and mpyos. mpyrepl behavior unchanged
  (suite + raw-mode smoke green).
- **PPU handoff round-trip**: console_enable() (the disable latch was
  one-way) + snesstage_hw_reset() (drops the stage lazy-init latch);
  recover() = reset + enable + console_init + pad_wait_release. Proven by
  running the full Stage demo from the manager and returning to a live
  list (test_run_frozen_stage_stdin).
- **Per-run interpreter**: gc_init + mp_init before, mp_deinit after every
  run; no C-held mp_obj_t across runs (raw-mode precedent).

Bugs found on the way:
- **sfs_write's int8_t slot test** hit ledger #21 (1-byte values tested
  with 16-bit reads): `existing >= 0` misread slot 1 as negative, the
  replace-delete was skipped, and every re-save DUPLICATED the file. The
  sram_fs API now uses plain int for find/write results.
- **Console tilemap overlapped the highlight font tiles** since M5: the
  font is 0x600 VRAM words but the tilemap sat at word 0x400, so inverse-
  video tiles 128-191 (every letter) rendered as garbage — the oskb's
  "block cursor" was actually this bug wearing a costume. Tilemap moved to
  word 0x800; the file manager's selection bar made it visible.
- **Mesen testrunner hard-kills Lua at ~100s wall time** regardless of the
  ScriptTimeout setting: long scripted sessions must either fit (the stage
  run does) or be split; the joypad-plane test runs hello.py instead of
  the 3-minute demo. The harness gained run-length JOYSEQ entries
  ("b*3000") so long holds don't bloat the Lua source.

## 2026-07-03 (late) — M8: Stage game library on the PPU; two build-system bites

The python-ugame Stage library (Bank/Grid/Sprite tile-game API) ported to
hardware: port/pylib/stage.py keeps the upstream API, but the whole
per-pixel `_stage` C compositor is replaced by port/modsnesstage.c driving
the PPU — Bank = 4bpp VRAM charset (a Stage bank is 2048 bytes, exactly 64
SNES 8x8 chars; ONE upload serves BG and OBJ, since both read the same
32-byte tile format and 16x16 composition and OBSEL/BG12NBA can share char
base 0), Grid = BG1 tilemap in 16x16-tile mode (Grid.move = scroll
registers), Sprite = an OAM entry (Stage rotations 0/2/4/6 = H/V flip
bits; the 90-degree ones raise). All PPU state lives in WRAM shadows that
flip() DMAs in one vblank (CGRAM 512 + tilemap 2048 + OAM 544 bytes ≈
3.1KB, inside the ~6KB budget). No heap objects in the C module at all —
buffers are read per-call, so no GC/layout interactions by construction.

Bring-up cost three root-caused bugs — two ours, one new Calypsi entry:

**MP_TUPLE_ITEMS_BOUND(4) vs frozen tuples (ours).** The earlier tuple-FAM
fix (Calypsi emits NOTHING for FAM-initialized arrays) bounded items[] at
4 — enough for modsys, but the demo froze 5/6-element tuples and Calypsi
DROPS excess initializer elements (with only a warning). Fix: mpy-tool.py
emits each frozen tuple with an exact-size items array (layout-compatible;
allocation is offsetof-based). Patch regenerated.

**Order-only $(GENERATED) = stale qstr ids (ours).** Adding the first new
qstrs since the objects were built exposed that every %.o rule had
`| $(GENERATED)` (order-only): the frozen content was compiled against the
NEW qstr numbering, the core against the OLD — off by one slot, so
`import _snesstage` reported "no module named 'acos'" (its table
neighbor). Fix: generated headers are real prerequisites, with a cmp-guard
so an unchanged table doesn't rebuild the world. Second act of the same
bug: the extmod %.o rule sat ABOVE the `GENERATED :=` definition, and make
expands prerequisite lists at rule-read time — its $(GENERATED) had always
been empty. The stale modframebuf.o answered framebuf method lookups with
math/set/_snesstage qstrs (`dir(FrameBuffer)` listed 'fabs', 'issubset',
'bank'; ssd.fill -> AttributeError). Rule moved below the definition.

**Calypsi bug 23: aligned() ignored on ANONYMOUS struct types, even at
variable position.** The exact-size tuple structs above were first emitted
as `static const struct {...} MICROPY_OBJ_BASE_ALIGNMENT name = ...` — the
attribute position that works for every named type produces NO `.align 2`
when the type is an anonymous struct, so frozen tuples landed at whatever
parity section packing gave them. An odd-placed object's MP_ROM_PTR has
bit 0 set = a small int under OBJ_REPR_B: the demo's ball table iterated
as "'int'/'bool' object isn't iterable" or unpacked the wrong tuple,
morphing with every layout change (2- and 3-row tables happened to land
even, 4+ odd). Diagnosed by diffing .lst files: mp_obj_str_t consts get
`.align 2`, the anonymous structs don't. Fix: mpy-tool emits a typedef per
tuple arity (named type -> attribute honored). Fence: check_obj_align.py
now also scans frozen_content*.lst and fails the link if any const_obj_*
label is not preceded by `.align 2` — the map-based check can never see
these (file statics aren't in the map).

Triage of the suite residue took the pass rate from 87% to **91.9%
(430/468 runnable; 571 total, 103 reference-skips)**. pytest 7/7 throughout.
Fixed this round:

**Calypsi frontend bug, 4-line reproducer:** `if (!((10) >= (10)) || f(x))`
compiles to unconditional TRUE — f never called, else branch deleted from
the TU. Plain `0 || f(x)` is fine; the trigger is a NEGATED FOLDED
RELATIONAL OR'd with anything. MicroPython's feature-test idiom
`!MICROPY_PY_BUILTINS_SET || cond` (SET = `ROM_LEVEL >= CORE`) expands to
exactly this shape, so: set literals compiled as dicts, set comprehensions
emitted as dict comprehensions (emitbc.c), and STORE_COMP dict-stored into
sets (vm_split.c — the set_comprehension hang). Fixed at 4 sites with
#if/#else. NOTE for the ledger: py/vm.c:893 has the same idiom (unused —
we build vm_split) — patch if the fallback VM is ever revived.

The hunt itself burned three workaround attempts, each defeated by a
DIFFERENT bug, all now in the ledger: (a) 1-byte bool locals are tested
with 16-bit reads (garbage neighbor byte made false truthy); (b) &&-chain
early-exits can reach the join with the accumulator holding `pn & 3`
instead of 0 (materialized short-circuit values are unsafe); (c) even a
volatile-function-pointer call in the condition got folded. The surviving
form: cross-TU predicate (mp_parse_node_is_struct_kind_helper in parse.c)
+ preprocessor branching for the feature test.

Also: host reference now mirrors MICROPY_PY_IO=0 (io_* tests honestly
skip instead of failing the comparison).

Remaining 38 (build/upstream_final.json): ~11 legit platform (int width,
sizeof(int)==2 endian family, 56KB MemoryError), string_format family
(~5), sys feature gaps (2), and ~20 worth hunting — likely clusters:
builtin_bin/hex/oct (one int-formatting issue?), iter1/iter2 +
gen_yield_from_* (iterator protocol edge?), class_bases/bind_self/
staticclassmethod, int_small (!), string_endswith/startswith,
string_format_modulo (hang). Next session.


## 2026-07-03 (evening) — the SNES passes 87% of upstream tests/basics

Built the official-suite harness and ran all 571 files of
micropython/tests/basics on the target. **Final: 414 pass, 62 fail/hang,
95 skip (reference can't run them) = 87.0% of runnable tests pass on the
SNES.** Machinery (tools/run_upstream_tests.py): REPL ROM raw mode
(^A source ^D, output bracketed STX/EOT, mp_deinit/mp_init between tests),
expected outputs from a unix reference build (VARIANT_DIR=
tools/host_variant) mirroring the port config (CORE, float32, no bignum);
batches of 10 through the mailbox; a hung test costs its batch and is
skipped on the rerun. Results: build/upstream_final.json.

The suite immediately found three bugs our own 7 suites never could:

1. **emitbc.c jump offsets (UPSTREAM BUG, 16-bit size_t):**
   `label_offsets[label] - bytecode_offset - 2` subtracts in unsigned
   16-bit size_t and then widens to 32-bit ssize_t — no sign bit to wrap
   into, so a backward jump of -20 arrived as +65516 and EVERY loop
   compiled on target raised "bytecode overflow" (the parked REPL
   mystery). Fix: widen operands to ssize_t before subtracting.

2. **Calypsi stack-slot mismatch (new codegen class):** parse.c's constant
   fold computed `op = POSITIVE + (tok - PLUS)`, stored it to stack slot
   1,s and passed slot 3,s to mp_unary_op — every folded negative literal
   became ~x instead of -x (print(-1) -> -2; abs(-1) == 2). Invisible in
   frozen code (mpy-cross folds on the host). Dodge: explicit branches
   passing constant op values.

3. **Calypsi _Mod32 clobbers caller _Dp scratch:** the compiler parked
   'divisor' in _Dp+4..7 across the % helper call; _Mod32 leaves
   |divisor| there, so the Python sign fix saw a sign-stripped divisor
   (7 % -3 == 1, -7 % -3 == 2). Floor division was unaffected (adjusts
   before dividing). Fix: volatile STATIC copy of the divisor (volatile
   stack locals are ignored by Calypsi — bug 2's asymmetry, exploited
   deliberately this time).

Remaining 62: ~6 int-width (host 64-bit folds literals the target can't,
no bignum), 5 sizeof(int)==2 (array/bytes item sizes), 3 hangs
(set_comprehension, string_format_modulo, syntaxerror), ~48 unclassified
(mix of string-format feature gaps, heap limits, scope features — next
session's triage list; many will be legit platform categories).

Also this round: objarray.h dual layout (Calypsi variant keeps len/items
at mp_obj_str_t offsets — the misc.h aliasing assert caught my earlier
pad-byte layout violating it; upstream bitfields restored for other
compilers), memview_offset_max made conditional, and the host reference
variant lives in tools/host_variant/.


## 2026-07-03 — M7 GREEN: nano-gui on the SNES (7/7). THE GC marker bug.

pytest 7/7. The demo renders pixel-faithfully: Meter, red LED, cyan Dial
with red pointer, Labels, CWriter/arial10 — black background, correct
palette. Screenshot: nanogui_snes.png.

**THE systemic find — GC marker misses unaligned interior pointers.**
gc_mark_subtree scans heap block children at aligned 4-byte strides. With
16-bit size_t and packed structs, heap objects hold pointers at 2-mod-4
offsets (inline tuple items after the 2-byte len, qstr pool entry arrays)
and even ODD offsets (mp_obj_array_t items at 9, after a uint8_t typecode).
Any object whose ONLY reference lived at such an offset was collected while
alive. Observed kills, each with a distinct symptom:
- the current qstr chunk (reachable only via pool entries at odd phase +
  a root deliberately excluded upstream) — reused as a code_state; the next
  qstr intern memcpy'd string bytes over a frame's closure cell -> the
  "NameError: local variable referenced before assignment" at
  super().__init__ in frozen runs (the LED/label hunt);
- every bytearray's backing store (items ptr at offset 9 — invisible even
  to a dual-phase scan) -> SSD.lut read back recycled qstr text ("palette"
  fragments) -> psychedelic CGRAM.
Fixes: gc_mark_subtree scans each block twice, phase-shifted by 2 (like the
root-section scan; conservative over-marking only), guarded by
MICROPY_GC_UNALIGNED_ROOT_SECTION; root scan extended to include
qstr_last_chunk itself (fresh chunk has no pool entry yet); objarray.h gets
one explicit pad byte so items sits at a scanned offset. Rule for the port:
**no pointer field may sit at an odd struct offset in a heap object** —
pad after lone uint8_t members.

Debug chain worth remembering: python-level probe (which local is unbound)
-> C-level probe (vm_load_check dump: slot value bank-stripped-looking) ->
closure trace (cell pointer perfect at every stage) -> bc.c end-of-setup
tripwire (slot correct after setup!) -> Mesen write-trap on the slot
address -> PCs symbolized to qstr_from_strn+memcpy -> the GC.

**snesfb palette now vblank-deferred.** CGRAM writes during active display
land at whatever address the PPU is rendering from (hardware + accurate
emulators). palette() writes a shadow; show() applies it at the start of
its first vblank window before the VRAM DMA chunks.

Minor: main_gui prints "%.2f" values (mpy-cross double->float32 truncation
made repr ragged: 0.44999984); test_gui EXPECTED updated (0.10/0.45/0.80).

OPEN (parked): on-target REPL raised "RuntimeError: bytecode overflow"
compiling a 2-line for loop (`for i in range(16): snesfb.palette(...)`)
— worked around by unrolling; compiler-side, not runtime; investigate
someday. The 4 ora-clobber WARNs (bytes_make_new, str_encode, mp_compile
region, mp_obj_is_true internal) remain under watch.


## 2026-07-03 (later) — ora-clobber class fixed; nano-gui one widget away

Continuing the safari with the checker-driven method (6/7 pytest green; only
test_gui red, and the demo now reaches the LED widget after init, colors,
version check, Label and Meter all work on target).

**Calypsi bug: the ora-clobber.** `while ((x = f(...)) != 0)` (assignment
inside a null test) sometimes compiles to `jsl f; stx t; ora t; beq ...;
stx hi; sta lo` — the accumulator holds low|high from the test, and the
codegen stores it as the value's low word. list(map(str, (7,))) yielded
[True] because the perfectly good str object's low word got OR-mangled into
an odd (tagged) value. Whether a site compiles broken is register-pressure
roulette per build.

Fixes:
- `mp_store_obj_result` (py/obj.c/h): un-inlinable store helper called via a
  volatile function pointer (Calypsi ignores noinline AND ignores volatile
  on locals — a plain `volatile` receiver changed nothing). Flagged sites
  rewritten to `store(f(...), &x); if (x == SENTINEL) break;` so the caller
  tests a clean memory copy: objlist list_extend_from_iter, modbuiltins
  all/any/+1, objset x5, obj.c mp_obj_is_true, objmodule module_print, and
  **mp_get_buffer** — the last one was heap-corrupting the Writer/framebuf
  path (a Label rendering made the next print emit garbage). Lesson: do NOT
  assume a flagged site is benign ("only truthiness") — mp_get_buffer was
  first triaged as harmless and was actually the worst.
- checker rule 2 in tools/check_neg_index.py: flags `ora dp` → branch →
  `sta` with A unrefreshed. Negative-Y hits stay fatal; ora hits warn
  (4 remain: compile.c, mp_obj_is_true internal, objstr bytes_make_new,
  str_encode — re-triage against failures, see the lesson above).

**modframebuf 16-bit overflow (upstreamable):** pixel index `x + y*stride`
computed in int overflows at y>=128 on a 256-wide buffer (16-bit int!) —
out-of-bounds writes corrupted the heap. All 14 index expressions now use
size_t arithmetic.

**Calypsi FAM-initializer silent drop, tuple edition:** const
mp_rom_obj_tuple_t initializers emitted a 6-byte header and NO items
(sys.implementation read the neighbouring object as its first element).
Type-object slots had the same bug fixed in an earlier session (obj.h
slots[12]); tuples now share the vbcc fixed-bound fix
(MP_TUPLE_ITEMS_BOUND=4 for __CALYPSI__ too, objtuple.h).

**mp_getiter alignment guard** (runtime.c): a C-stack iter_buf with an odd
address is replaced by heap allocation (REPR_B would misread the object).

OPEN: test_gui — demo stops at LED.__init__ with a bogus arity error
("function takes 1 positional arguments but 9 were given"); Label/Meter
pass the identical super().__init__ shape, so this is the next roulette
site (not among current checker warnings). Method for next session: REPL
repro (the REPL ROM freezes the gui tree; `import main` reproduces), then
python-level bisect, then .lst microscope on the C function it lands in.


## 2026-07-03 — THE negative-Y bug: Calypsi far indexing crosses banks (6/6 green)

**Calypsi bug (the big one): `p[-1]` on a far pointer sometimes compiles to
`ldy ##-2` + `lda/sta [dp],y`. The 65816 adds Y to the 24-bit base as an
UNSIGNED 16-bit value, so the access lands in the NEXT bank** — reads return
garbage from +64KB away, writes vanish (into ROM). Whether the compiler
picks the negative-Y form or the safe adjust-base-then-positive-Y form is
register-pressure roulette per compilation, which finally explains the
"every constituent op works in isolation, the combination fails, and the
failure moves when you touch anything" pattern that has haunted this port.

Found via the VM stack tracer: `0 <= 5 <= 15` (chained comparison) showed
ROT_THREE turning stack [5,5,0] into [5,GARBAGE,0] — the generated code read
`sp[-2]` from bank+1 and wrote `sp[-2]` into bank+1 (listing: `ldy ##-8;
lda [(_Dp+8)],y`). 18 such sites existed in vm_split.o alone (rot ops,
dup_top_two, store_subscr, with_cleanup, the unwind helpers' CANCEL_ACTIVE_
FINALLY). test_m4's regression was the same bug landing in the FAST_N
locals indexing (`fastn[-unum]`).

Fixes:
- `port/vm_split.c`: every below-pointer access goes through
  `vm_ptr_at(base, off)` — the adjusted pointer is materialized via a
  volatile temp (low-word pointer arithmetic preserves the bank; the
  optimizer cannot fold it back into an indexed form). MULTI opcode
  handlers take the opcode from a new `vm_ctx_t.opcode` field instead of
  reading `ip[-1]`.
- **`tools/check_neg_index.py`**: scans every generated .lst for
  `ldy ##<negative>` followed by long-indexed addressing and FAILS the
  build. Wired into all four ROM links next to check_obj_align. Current
  py/ core compiles clean; the checker guards the roulette.
- Report upstream to Calypsi (hth313) with the rot_three listing.

Suite back to 6/6 (test_m4 recovered). Chained comparisons, swaps,
multi-assignment all verified in the emulator.

## 2026-07-03 — WIP: nano-gui groundwork — a compiler-bug safari (5/6 green)

Goal: run peterhinch/micropython-nano-gui. Status: the Python side runs
VERBATIM on the host testbed; on target the demo boots deep into nano-gui
(imports, color LUT setup) before hitting one remaining VM/runtime landmine.
pytest 5/6 (test_m4 red, same landmine — see OPEN below). This entry records
an extraordinary chain of root-caused Calypsi 65816 bugs, each with a
minimal reproducer methodology (REPL sessions as probes, pure-C probe ROMs
linked against real build objects, Mesen Lua write/exec traps + PC sampling
with map-file symbolization).

Infrastructure added:
- `port/modsnesfb.c`: mode-1 4bpp 256x192 unique-tile framebuffer module
  (init/show/palette/vsync); GS4_HMSB nibbles -> bitplanes in a 24KB WRAM
  staging buffer -> vblank-chunked DMA. Linked into every ROM.
- `port/pylib/`: vendored nano-gui subset (MIT; one documented deviation:
  writer.py falls back to copying glyphs when uctypes is absent) + our
  `color_setup.py` (SSD driver: nano-gui's 4-bit LUT model maps 1:1 onto
  SNES CGRAM). `make mpygui` freezes the whole package tree (mpy-cross -s
  with package-relative names); the REPL ROM now freezes it too, so the GUI
  is importable interactively.
- Config: CORE_FEATURES ROM level (slice/property/enumerate/str.format...,
  MICROPY_PY_IO=0), EXTERNAL_IMPORT=1 (frozen packages; mp_import_stat
  stubs), complex+cmath. main.c now runs frozen main via mp_import_name
  (the direct proto_fun path miscompiled in some ROM layouts; the importer
  path is exercised by every frozen package import and proven).
- Host testbed recipe: unix minimal + CFLAGS mirroring the port config runs
  the vendored tree unmodified — validates Python-side before target work.

Calypsi bugs found & fixed this session (all fixes in patches/0001):
1. **gc_setup_area received `end` with the bank byte stripped** — the THIRD
   pointer parameter of the call lost its top half in marshalling (cousin of
   the M1 varargs bug). pool_start/pool_end were therefore bank-0 pointers;
   every allocation > ~3KB (first-fit reaching fresh pool area... in truth
   ALL allocations were handed bank-0 addresses whenever this codegen
   variant was active) zero-filled the C STACK -> wild jumps into ROM
   padding/zeroed RAM. Symptom history: bytearray(4000) hangs, layout-
   dependent. Fix: derive pool pointers from `start` (arrives intact) plus
   scalar offsets; `end` is only used for the length. Found via a 30-line
   probe ROM linking the real gc.o; proven by byte-level field dumps.
   NB: codegen varies per FILE COMPILATION — the same function compiled
   correctly for months, then my unrelated edits elsewhere in gc.c flipped
   it. Trust nothing; test behavior.
2. **gc root scan missed roots**: mp_state's root-pointer section contains
   pointers at 2-mod-4 offsets (packed structs, 16-bit size_t), and
   upstream's `ptrs + root_start / sizeof(void *)` also truncates. Fix:
   byte-exact base + a phase-shifted second scan behind new config
   `MICROPY_GC_UNALIGNED_ROOT_SECTION` (over-marking is safe for a
   conservative GC). Symptom: gc.collect() erased live globals.
3. **gc_alloc split** (gc_alloc_attempt called via a VOLATILE function
   pointer — Calypsi ignores noinline like it ignores noreturn): each
   attempt re-derives all state so nothing is cached across the internal
   gc_collect() call (vm_split philosophy). Defensive after seeing stale
   `_Dp`-cached pointers around that call shape.
4. **mp_obj_array_t bitfields de-bitfielded** (size_t typecode:8/free:8):
   Calypsi miscompiles bitfield RMW (established M5-era with
   mp_float_union_t); also free:8 could only count 255 spare elements with
   16-bit size_t. memoryview offset limit lifted accordingly.
5. **Stack-allocated mp_obj structs can be ODD** (no stack alignment!) —
   zero-arg super() built mp_obj_super_t on the C stack; at odd addresses
   REPR_B read it as a small int ("'int' object has no attribute
   '__init__'"). Fix: manual 2-alignment inside mp_load_super_method.
   LATENT RISK noted: C-stack mp_obj_iter_buf_t users (sum(), str.join
   internals via mp_getiter with stack buffers) have the same exposure —
   audit/fix when it bites.
6. Alignment attributes added for newly-enabled const objects
   (mp_module_framebuf, float pi/e/tau/inf/nan, mp_const_empty_dict_obj) —
   the map checker caught every one at link time, exactly as designed.

OPEN (next session): `0 <= 5 <= 15` (chained comparison) hangs inside the
BINARY_OP LE dispatch after DUP_TOP/ROT_THREE — every constituent op passes
in isolation (all relops, zero operands, ROT_TWO/THREE, DUP_TOP, multiple
assignment); runtime.o at -O0/-O1/-O2/force-switch identical. This blocks
test_m4 (fact()'s `n <= 1`... actually M4 red may be its own thing — it
hangs, unverified which line) and nano-gui's create_color range check.
Debug next: VM_TRACE plus operand-value dump inside op_binary_op_multi;
suspect another shape-specific miscompile in vm_split.o or objbool/runtime
compare path. The mpygui ROM otherwise reaches gui/core/colors.py — i.e.
frozen package imports, snesfb, framebuf and the driver __init__ all work.

Debug tooling worth remembering (tests/ + this entry):
- Mesen Lua: PC sampling via emu.getState() per frame; write/exec traps via
  emu.addMemoryCallback(fn, emu.callbackType.write|exec, start, end,
  emu.cpuType.snes, emu.memType.snesMemory) — the 6-ARG form is mandatory
  (4-arg silently never fires); symbolize against build/*.map (NB: library
  symbols use a one-line "sym in section 'x' placed at..." format).
- /tmp/mbx gets cleaned by systemd-tmpfiles — recreate cfgroot (Mesen2
  config copy with AllowIoOsAccess) before trusting any run.

## 2026-07-02 — M6 GREEN: REPL on the TV with joypad input

The REPL ROM (`make mpyrepl`) now drives a real screen and takes controller
input — it would work on actual hardware from a flashcart. pytest 6/6; the
new `tests/test_joypad.py` scripts controller 1 to navigate the on-screen
keyboard, type `1+2`, run it (SNES prints 3) and exit via Select.

- **Text console** (`snes/console.c`): mode 0, BG1 2bpp, 32x28 cells, white
  on dark blue. Font: vendored public-domain `snes/font8x8_basic.h`
  (dhepper/font8x8, IBM VGA lineage), converted at build time by
  `tools/font2snes.py` into 192 SNES tiles (96 normal + 96 highlighted, the
  highlight variants use plane1=0xFF so palette c2/c3 give inverted cells —
  needed because BG color 0 is transparent, so a "swap palette" trick can't
  paint a filled background). No interrupts: the REPL is synchronous, so
  console_flush() polls $4212 for vblank and DMA's the whole 1792-byte WRAM
  shadow tilemap — dead simple, no dirty tracking, one frame per flush.
  Rows 0-20 scroll as a terminal (block cursor overlaid during DMA); rows
  21-27 are static (separator, keyboard, hints). DMA source addresses are
  derived via a pointer/byte-array union, avoiding pointer->int casts.
- **On-screen keyboard** (`snes/oskb.c`): name-entry style 13x4 grid, two
  pages (Y toggles lower+digits+code-symbols / upper+rest — all 95 printable
  ASCII covered), D-pad with hold-autorepeat (14-frame delay, 4-frame rate),
  A=type, B=backspace, X=space, Start=Enter, Select=Ctrl-D. Auto-joypad
  read is enabled WITHOUT NMI ($4200=0x01) and polled at $4218/9.
- **Dual channels**: repl_main writes every byte to both the mailbox ring
  (pytest asserts it, unchanged) and the console; input is merged from the
  mailbox stdin ring (scripted) and oskb_poll() (human/scripted joypad). So
  ONE ROM serves CI and the TV. Backspace edits the vstr and the screen but
  is never emitted to the (append-only) mailbox log.
- **Harness**: `run_rom(..., joypad=[...])` scripts controller 1 — MUST be
  applied inside Mesen's `inputPolled` event (setInput from the endFrame
  callback is overwritten by the real input system and does nothing; that
  cost an hour). Screenshots via `emu.takeScreenshot()` in a Lua callback
  were used to verify rendering headlessly.
- Banner shortened to "MicroPython on SNES; ^D exits" (<= 32 cols, no wrap).

Next ideas parked: input-line cursor movement + history on L/R, SNES mouse
support, and the `snesfb` framebuffer module (unique-tile trick) as the
bridge toward a MicroPython GUI library.

## 2026-07-02 — M5 GREEN: interactive REPL, Python compiled ON the 65816

`make mpyrepl` builds a REPL ROM (`port/repl_main.c`): lines arrive through a
new mailbox **stdin ring** ($7FE006/8 index words + $7FFE00 u8[512]; stdout
ring shrank to 7664 bytes — see snes/mailbox.h), are lexed/parsed/compiled by
the MicroPython compiler running on target (`MICROPY_ENABLE_COMPILER 1`) and
executed by the split VM. Multi-line compound statements work via
`mp_repl_continue_with_input` ("..." prompts); bare expressions print their
value (`MP_PARSE_SINGLE_INPUT` + is_repl); Ctrl-D exits PASS. The Lua harness
feeds a scripted session (`run_rom(..., stdin_data=...)`);
`tests/test_repl.py` asserts the session byte-for-byte — including defining
`sq(x)` interactively and calling it (144), and a ZeroDivisionError traceback.
pytest now 5/5 (M0 hello, M1 selftest, M3 mpy, M4, M5 repl).

Two new toolchain/port findings on the way (both in patches/0001 + Makefile):

1. **Calypsi ICE on py/lexer.c at any -O >= 1**: "ControlFlowOptimize.hs:
   Non-exhaustive patterns in function go". Only surfaced once the compiler
   code was actually enabled. Workaround: `$(MPBUILD)/py/lexer.o: MPOPT = -O0`
   (Makefile now routes -O flags through `MPOPT`). Report upstream.

2. **py/parse.c chunk allocator breaks on 16-bit size_t + unaligned structs**
   (the REPL's "NameError: name 'micropython' isn't defined" mystery):
   `mp_parse_chunk_t { size_t alloc; union { size_t used; ptr next; }; byte
   data[]; }` puts `data[]` at offset 6 when size_t is 2 bytes and the
   compiler inserts no padding — so every parse node lands at address ≡2
   (mod 4) and `MP_PARSE_NODE_IS_STRUCT` (low two bits == 0) misclassifies
   every struct node as a leaf; the "tree" becomes heap-layout-dependent
   garbage while lexer, qstr interning, maps and codegen are all perfectly
   fine (each was probe-verified in isolation on target). Fix: make
   `alloc`/`used` uint32_t so data[] starts at offset 8. Upstreamable —
   this bites any port with 16-bit size_t and packed structs.

   Debug pattern that cracked it: on-target unit probes over the mailbox
   (qstr_find/parse-node-roundtrip/map-with-qstr-key all OK) then dumping
   lexer tokens (OK) and the parse tree (single drifting leaf → allocator).

Also: `MICROPY_ERROR_REPORTING` switched TERSE → NORMAL (fits ROM easily and
the REPL needs "name 'foo' isn't defined"-grade messages), and
`MICROPY_STACK_CHECK 1` with 1KB margin so deep parser recursion raises
RuntimeError instead of smashing the 7.4KB bank-0 stack. eval/exec now exist
as builtins in ALL ROMs (compiler is enabled globally); M3/M4 stay green.
`mp_lexer_new_from_file` stubbed to raise OSError (no filesystem), like
ports/minimal.

## 2026-07-02 — M4 GREEN: real root cause found (ROM object alignment), VM split shipped

**The "layout-sensitive VM miscompile" was never (primarily) a code-gen bug.
It was data alignment.** `MICROPY_OBJ_REPR_B` requires every object to sit at
an even address (bit 0 is the small-int tag). Our port set
`MICROPY_OBJ_BASE_ALIGNMENT __attribute__((aligned(2)))` on the
`mp_obj_base_t` member — the documented ports/pic16bit trick — but **Calypsi
silently ignores `aligned()` on struct members and struct types; it only
honors it on variable definitions** (guide §11.7 describes data as unaligned
by default; verified with .lst: member/type attribute emits no `.align`,
variable-position attribute emits `.align 2`). So every ROM object
(`mp_builtin_print_obj`, types, exception instances, frozen strings, module
objects, …) had *random parity determined by layout*. Shift anything by an
odd byte count → a different set of objects becomes odd → REPR_B misreads
them as tagged small ints → the exact observed zoo: `TypeError: object not
callable`, garbage exceptions, `NotImplementedError: opcode`, hangs — all
"deterministic per layout, chaotic across layouts", on BOTH compilers (for
vbcc the macro was defined empty, so its ROM objects were never aligned
either). Every prior workaround (O-levels, volatile ip, stubbing, save/
restore) merely re-rolled the parity dice — matching every recorded result.

**Fix** (all in `patches/0001`, gitlink untouched):
- `MICROPY_OBJ_BASE_ALIGNMENT` injected at *variable position* into every
  ROM-object definition macro: `MP_DEFINE_CONST_FUN_OBJ_*`,
  `MP_DEFINE_CONST_DICT_WITH_SIZE`, `MP_DEFINE_CONST_OBJ_TYPE_NARGS_*`,
  `MP_DEFINE_CONST_{STATIC,CLASS}METHOD_OBJ`, `MP_DEFINE_STR_OBJ`,
  `MP_DEFINE_ATTRTUPLE`, plus the direct definitions (none/bool/ellipsis/
  NotImplemented singletons, empty tuple/bytes, `mp_const_GeneratorExit_obj`,
  `mp_sys_*` tuples, all `mp_obj_module_t` instances) and `mpy-tool.py`'s
  frozen `mp_obj_str_t`/int/float/complex/tuple emissions.
- **Safety net:** `tools/check_obj_align.py` parses the link map and FAILS
  the build if any object symbol is odd (runs automatically after every mpy
  link; 131 symbols checked; static frozen objects are covered by the
  mpy-tool patch but invisible to the map). Never trust parity to luck again.

**VM split** (`port/vm_split.c`, `VM_SPLIT=1` default in Makefile): the
per-op decomposition of `mp_execute_bytecode` was built anyway — one tiny
handler function per opcode dispatched via a 256-entry table, ALL VM state
(ip/sp/exc_sp/…) in a `vm_ctx_t` accessed only through a pointer, the
setjmp-containing function (`vm_run`) keeps zero mutable locals (immune to
Calypsi bug 2), shared labels became status-code helpers. `py/vm.c` is not
compiled at all when VM_SPLIT=1 (it only contains `mp_execute_bytecode`), so
no submodule patch was needed. Logic was validated on host first (unix port
minimal variant with vm.c swapped for vm_split.c — both test programs
byte-perfect), which cleanly separated "transformation bug" from "Calypsi
bug" during bring-up.

**Is the split needed, or was alignment everything?** Controlled test: with
the alignment fix, the ORIGINAL monolithic vm.c (VM_SPLIT=0) runs recursion,
mutual recursion, list methods, str methods, dicts, classes, closures and
generators correctly — but **hangs in the nested try/finally + cross-frame
exception test** (exit 99). That residue is consistent with Calypsi bug 2
(volatile-auto stores dropped in setjmp functions — vm.c keeps `exc_sp` in a
volatile stack local of the setjmp function). The split VM passes everything.
So: alignment was ~95% of the mystery; the split dodges the remaining
setjmp-function fragility by construction. VM_SPLIT=1 stays default.

**Verification** (all in Mesen, byte-for-byte):
- `pytest tests/` 4/4: M0 hello, M1 selftest, M3 mpy, **new M4**
  (`tests/test_m4.py` / `port/main_m4.py` / `make mpy4`): fact(10)=3628800,
  mutual recursion, list iteration+append+len, ",".join, .lower, dict
  store/load, class with `__init__`+method+attribute, closure, generator
  loop, nested try/finally with exception across a Python call frame.
- **Layout-perturbation robustness** — the bar every earlier "fix" failed:
  pad objects of 1, 3, 17 and 257 bytes linked ahead of everything shift the
  whole image by odd amounts; all four perturbed M4 ROMs PASS byte-perfect
  and `check_obj_align` stays clean in each.

Follow-ups: report the member-alignment silent-ignore to Calypsi upstream
(hth313); vbcc ROM objects are still unaligned (macro empty under `__VBCC__`
— vbcc rejects the attribute syntax; find its alignment mechanism if the
vbcc path is ever revived); consider upstreaming a REPR_B alignment note to
MicroPython for compilers without member-alignment support.

## 2026-06-13 — `volatile ip` (the per-op-split core mechanism) only shuffles

Tested the per-op-split hypothesis the cheap way BEFORE building the split:
the split's whole value is "`ip` lives in memory, not a register lost across
calls", so `const byte * volatile ip` is a one-line proxy for it. On Calypsi:
- First build (a recursion+method program — `fact(5)`, `",".join(...)`, things
  Calypsi fails in EVERY normal layout): **it printed correct output**
  (`fact 120`, `join a,b,c`). Briefly looked like a one-line win.
- But it does NOT hold: the committed M3 program then HANGS, and the very same
  recursion+method program FAILS (hang / `NotImplementedError: opcode`) after a
  rebuild or a tiny layout perturbation (padding a function). So the "success"
  was a lucky layout — `volatile ip` just shuffles the lottery like everything
  else.
- `volatile sp` is strictly worse (immediate hang) — `sp` is written constantly
  and Calypsi drops volatile-auto stores (bug 2); DECISIONS already noted saving
  sp made it worse.

Conclusion: forcing `ip` to memory — the exact thing a full per-op split would
do — does not robustly fix the bug; it relocates it. So building the split
(hours, intricate: `load_check` is a label nested inside a `case`, 11 shared
jump-targets) is very unlikely to pay off. This closes the structural-workaround
avenue on Calypsi too. The bug is pervasive layout-sensitive codegen fragility
in the 21KB function, not a single localizable register-spill. Remaining paths
unchanged: report upstream (best leverage), spike llvm, or hold at M3.

## 2026-06-13 — Spikes: splitting the VM and tuning vbcc -O do NOT fix it

Two cheap experiments to see if the vbcc VM layout-sensitivity is curable
without switching compilers. Both NEGATIVE. (Emulator ruled out as the cause:
Mesen runs Calypsi M0-M3 and some vbcc layouts correctly, failures are
deterministic and coherent — the VM cleanly raising real exceptions after
reading one wrong byte. vbcc's bytecode reads ARE 24-bit far `lda [dp]` — not a
bank-loss bug. A definitive cross-check on vbcc's 65816-sim was offered but not
run.)

1. **Split / shrink `mp_execute_bytecode`.** Stubbed cold opcodes to shrink the
   function ~21% then ~39% (33KB -> 20KB code). Shrinking MOVES the symptom
   (AttributeError -> TypeError as size changed) — so the function size IS the
   lever — but does NOT fix it: even at 39% it still failed, still
   layout-sensitively, and the FULL-size function actually passed M3 in one
   lucky layout. So smaller is not reliably better; shrinking just relocates the
   lottery (exactly what Calypsi's save/restore did). A full extraction (2-4
   sessions) would very likely give the same "fixes some, breaks others".
2. **vbcc -O bitmask sweep.** -O is a bitfield (bit0=regalloc, bit5=global opts,
   bit8=place-vars-at-same-address, etc.). Tried 1023/767/1022/1021/895/1019/511
   on vm.o; all failed both test layouts. Disabling register allocation (bit0)
   and same-address placement (bit8) did NOT help.
3. **Robustness harness.** Swept the -O2 baseline across 7 marker layouts:
   **0/7 pass**. The one earlier-passing layout no longer passes even with
   identical markers — because passing depends on the alignment of the ENTIRE
   link (all ~100 objects); rebuilding anything shifts it. So vbcc "working" is a
   needle-in-haystack alignment, not a usable state.

Conclusion: vbcc cannot robustly compile this VM, same as Calypsi — a real
register-allocation/spill bug in both compilers triggered by the 21KB re-entrant
interpreter loop. No source split or compiler flag found that makes it
deterministically correct. Realistic paths: (a) report to the vbcc author
(Volker Barthelmann is responsive) and/or the Calypsi author with the project as
a minimal-ish reproducer; (b) spike a more mature compiler (llvm-mos/llvm-C65),
accepting its immature 65816 + absent SNES banking; (c) the 65816-sim cross-
check to 100% exonerate Mesen first. The project stands at M3-on-Calypsi (green)
with a fully-built-but-layout-fragile vbcc path as a documented dead-ish end.

## 2026-06-13 — vbcc runtime: VM runs, but is layout-sensitive like Calypsi

The decisive vbcc runtime test is in. Headline: **vbcc can execute the exact
constructs that break Calypsi M4 — recursion, list iteration, method calls, and
(with a fixed setjmp) exception raise/catch — and it produced byte-correct
output for all of them.** BUT this is **not robust**: like Calypsi, vbcc
miscompiles the 21KB re-entrant `mp_execute_bytecode` in a *layout-sensitive*
way. Some link layouts run the program perfectly; others raise a garbage
exception (TypeError/RuntimeError/NameError/AttributeError "no such attribute",
or a corrupted traceback) — and the symptom moves when you perturb *anything*
(add a debug print to main.c, change STACKLEN, change the test program). This is
the same class of bug Calypsi has, just at a different opt level. So vbcc is
**not the clean escape we hoped**; the callee-saved-register theory did not
deliver robustness in practice.

What it took to get the VM running at all (all folded into the build; the
Calypsi path stays green — `pytest tests/` is 3/3):

### vbcc 65816 r2 gaps found and worked around
- **No 64-bit runtime at all.** Even a bare `long long` multiply fails to link
  (no `___muluint64`/`___addint64`/… in any lib; operands wanted in zp symbols
  `x`/`y`/`s`). MicroPython sprinkles `long long` through misc.h (overflow + clz
  helpers, emitted into *every* TU because vbcc keeps unused static inlines),
  runtime_utils.c, and binary.c. Neutralised all of it `__VBCC__`-guarded:
  software clz/ctz/popcount; the unused `mp_*_ll_overflow` helpers stubbed to
  emit no 64-bit ops; `mp_binary_get_int` returns/accumulates in 32-bit
  (mp_binary_int_t); the long-long compares in mp_binary_get_val skipped.
- **`intptr_t`/`uintptr_t` are 16-bit** in vbcc's `<stdint.h>` (its own comment
  says "FIXME: depends on memory model"). In the far model a data pointer is
  24-bit, so `(uintptr_t)0x7FE000 == 0xE000` — the bank byte is DROPPED. That
  silently breaks `mp_int_t`/`mp_uint_t` (== intptr_t) and every pointer<->int
  round-trip (gc.c, obj.h MP_OBJ_TO_PTR). `(unsigned long)`/`uint32_t` casts
  round-trip fine. Fix: patch the toolchain header to 32-bit (the config's
  hardcoded `-I<target>/include` is searched before our `-I`, so a shim header
  is NOT picked up — must patch in place). Automated idempotently in
  `tools/vbcc_patch_toolchain.sh`, invoked by the vbcc Makefile.
- **Non-conforming NDEBUG `assert`.** vbcc's assert.h expands `assert(x)` to
  *empty* under NDEBUG instead of `((void)0)`, which turns MicroPython's
  `(MP_STATIC_ASSERT(...), assert(...), ...)` comma chains into a double-comma
  syntax error. Also patched by the toolchain-patch script.
- **Broken far-model setjmp/longjmp.** libvc's setjmp.o saves/restores a soft
  stack pointer at zp symbol `sp`, but the far model keeps the C stack in the
  *hardware* stack (verified: prologues push with `phy`, address locals `N,s`)
  and never maintains `sp`. So stock longjmp wedges and nlr_push corrupts
  re-entrant calls. **Wrote a correct replacement** (`snes/vbcc_setjmp.s`) that
  saves the hardware S + the 3-byte far return address (jmp_buf is 5 bytes).
  Verified in isolation (setjmp returns 0, longjmp(buf,42) returns 42) and it
  makes exceptions work on-device. Linked before -lvc so the stock object is
  never pulled. ABI: ptr arg in A(offset)/X(bank); 2nd arg at 4,s; int ret in A.

### Memory model + build (all working)
- Far/huge model (`+snes-hi`); type sizes match Calypsi (int=2, long/void*=4,
  size_t=2) — verified on hardware. Custom linker `snes/vbcc-snes.cmd`: STACKLEN
  enlarged for mp_init's ~6KB C stack, WRAMSIZE shrunk so far data ends at
  $7F0000, reserving bank $7F for the 56KB GC heap ($7F0000-$7FDFFF) + mailbox
  ($7FE000). Near data is 0 bytes (all data far), so all of $0100-$1FFF is C
  stack. Frozen ROM data lands mid-bank (no bank-straddle of the bytecode).
- `Makefile.vbcc` builds all of py/ (minus the native-code emitters, which the
  bytecode VM doesn't need and which left unresolved cross-refs) + port + a
  vbcc-specific frozen program (`port/test_vbcc.py`) and links a full ROM.
- **`vm.o` must be built at -O2.** At -O1 the VM miscompiles even harder; -O0
  hits a vbcc assembler bug (`unknown mnemonic divw`). -O2 is the most-correct
  level (the inverse of Calypsi, where -O1 was correct and -O2 broke it) — but
  even at -O2 the layout-sensitivity remains.

### Decisive evidence (both directions)
- POSITIVE: in a working layout, `fact(5)=120`, `for v in [..]` sum `=100`,
  `",".join([...])="a,b,c"`, `"HELLO".lower()="hello"`, and (with the fixed
  setjmp) `try/except` all produced exact correct output. Calypsi produces NONE
  of these in any tested layout.
- NEGATIVE: most layouts fail with a shifting garbage exception; adding a
  `mb_puts` to main.c (a different translation unit from vm.o!) flips pass<->fail.
  Not root-caused on vbcc to the precision Calypsi's `_Dp[0-7]` loss was; the
  trigger is link/stack/data layout, i.e. register-allocation/spill pressure in
  the giant re-entrant function.

### Where this leaves the project
The blocker is now understood as compiler-agnostic: a 21KB re-entrant
interpreter loop overwhelms *immature 65816 register allocators* (Calypsi and
vbcc both). Options going forward:
1. **Split `mp_execute_bytecode`** (hoist cold opcode handlers into separate
   functions) so no allocator faces a 21KB body — helps Calypsi, vbcc, and any
   future toolchain. Highest-leverage, compiler-agnostic.
2. **A more mature compiler.** llvm-mos has the best allocator, but its 65816
   support is a proof-of-concept (PR llvm-mos-sdk#415 runs a degraded 16-bit-
   address/8-bit-data mode; maintainer won't merge until "proper 65816 support"
   exists) and SNES banking is essentially absent. The separate llvm-C65 backend
   is real 16-bit 65816 but research-grade. A timeboxed spike (compile vm.c, run
   a mailbox ROM) would de-risk before committing to a third port.
3. Report both compilers' layout-sensitivity upstream with the project as repro.

The vbcc infrastructure (compile fixes, toolchain patches, correct setjmp,
build system, memory model) is committed and reusable regardless of direction.

## 2026-06-13 — vbcc: ALL py/ files compile (103/103 subset, 132/132 whole tree)

Cleared the last 6 compile holdouts. Every fix is `__VBCC__`-guarded except the
obj.h variadic sentinel (benign to all compilers); Calypsi `pytest tests/` stays
3/3 green after a clean `make mpy` rebuild. Build invocation now uses `+snes-hi`
(far/huge model, 32-bit pointers — matches our `mp_obj_t` assumptions) rather
than `+snes-his` (tiny). Helper: `tools/vbcc_env.sh` (`. tools/vbcc_env.sh; vbcc_cc <src> <out>`).

Root causes found (each reduced to a standalone repro before fixing):
- **vbcc rejects embedded/initialised flexible-array-member structs** (strict
  C11 6.7.2.1p3). This is ONE root cause behind three holdouts:
  - objnamedtuple.h embeds `mp_obj_tuple_t` (ends in `items[]`).
  - modsys.c statically initialises `mp_rom_obj_tuple_t` FAMs (sys.version_info).
  Fix: give `mp_obj_tuple_t`/`mp_rom_obj_tuple_t` `.items` a fixed bound
  `MP_TUPLE_ITEMS_BOUND` (=4, covers the largest static init) under `__VBCC__`.
  Allocation is offsetof(items)-based so the bound never changes runtime layout,
  only sizeof of the rare static instances. (objtuple.h)
- **objgenerator.c** was NOT a FAM problem: `mp_code_state_t.state` is `[0]`
  (→ vbcc treats as `[1]`, non-FAM). The real bug: our `offsetof` override in
  mpconfigport.h was being clobbered by a *later* system `#include <stddef.h>`
  (from py/obj.h), so `offsetof(t, code_state.state)` hit vbcc's builtin, which
  can't do nested members. Fix: `#include <stddef.h>` in mpconfigport.h BEFORE
  the override, so the guard is set and no later include can re-define it.
- **objcell.c**: a type with zero slots passes exactly 29 args to
  `MP_DEFINE_CONST_OBJ_TYPE_NARGS`, leaving its `...` empty — which vbcc (and
  strict C99) reject. Fix: append a trailing `_INV` sentinel to the dispatcher
  so `...` is always non-empty (N stays the 29th arg). (obj.h)
- **objtype.c**: vbcc errors ("invalid operand type") casting a *multi-term*
  integer expression to void, e.g. `(void)(1 + n_args + 2 * n_kw)` from the
  `m_del` macro. Minimal repro: `(void)(a + 2 * b)` ICEs, `(void)(a + b)` is
  fine. Fix: under `__VBCC__`, `m_del`/`m_del_var` use `(void)sizeof(num)`
  (num is a side-effect-free size expr). (misc.h)
- **binary.c ICE** (machines/65816/machine.c:3293): vbcc r2 ICEs converting a
  `long long` *value* to a 32-bit `mp_int_t`/`mp_uint_t` (both signed and
  unsigned; intermediate locals and masking don't help — only union punning or
  memcpy dodge it). Fix: `MP_LL_TO_I32`/`MP_LL_TO_U32` union-reinterpret the low
  32 bits (== the value when it fits, which is the only case used here), applied
  to the four conversion sites in `mp_binary_get_val`. (binary.c)

All folded into `patches/0001-compiler-workarounds.patch`. Next: vbcc memory
model + custom linker (56KB heap in WRAM, mailbox at $7FE000), standalone vbcc
Makefile, frozen module, then the decisive recursion/iteration/methods run.

## 2026-06-13 — vbcc evaluation: promising alternative to Calypsi for M4

Since the M4 blocker is a Calypsi codegen bug we can't work around (see
below), evaluating **vbcc 65816 (release 2, Oct 2025)** — a different
optimizing compiler, free for non-commercial use, with a real SNES target
(startup, HiROM linker, C library, sim). Toolchain in `vbcc-toolchain/`
(gitignored; fetch http://www.ibaug.de/vbcc/vbcc65816_r2.zip). Spike in
`vbcc_spike/`. The Calypsi build remains the primary, green path; vbcc work
is strictly additive (config is `__VBCC__`-guarded).

Why vbcc could dodge our bug: it has **callee-saved registers (r16-r27
preserved across calls)** — exactly the feature Calypsi's caller-saved
_Dp[0-7] model lacks for our bug class. It also offers near/far/**huge**
pointer models (huge crosses 64K banks, which Calypsi's large model can't).

Results so far (all positive):
- vbcc -> SNES -> Mesen pipeline validated: `vbcc_spike/hello.c` prints to
  the $7FE000 mailbox and passes our harness (exit 1).
- `vm.c` — the exact file Calypsi miscompiles — compiles cleanly on vbcc.
- Batch compile of py/ core: **91 of 103 files compile** after two small
  fixes (both folded into the workarounds patch / mpconfigport, vbcc-guarded):
  - extend the flexible-array-member `slots[12]` fix to `__VBCC__` (vbcc also
    rejects FAM initializers — same bug-4 class; cleared ~24 files)
  - `#define mp_hal_ticks_ms` in mphalport.h so py/mphal.h doesn't emit a
    prototype that clashes with our static inline (vbcc is stricter)
  - mpconfigport.h: neutralize `__attribute__((aligned))` and `MP_NORETURN`
    for vbcc.
- Remaining 11 compile failures are individual, mostly 1-3 line issues:
  missing `SEEK_SET` (stream.c), storage-class strictness on a few
  `mp_obj_new_*_iterator` (objlist/objstr), a couple of attribute/macro
  spots (obj.c, objcell.c, gc.c, objgenerator.c, objtype.c), modsys/
  objnamedtuple FAM, and ONE vbcc internal-compiler-error (binary.c:379,
  long-long->unsigned int conversion) that will need an expression rewrite.

Assessment: the vbcc compile phase is tractable and Calypsi-M2-like in
effort. Still ahead before the decisive runtime test: finish the ~11 compile
fixes, set up a vbcc memory model + linker for a big GC heap (the stock
snes-his config is tiny-model with a bank-0 heap — need far/huge model with
the heap in WRAM bank $7E/$7F), wire the frozen module, then run the programs
that break Calypsi. That last step answers the real question. Effort is a
fresh M2+M3-sized chunk.

### Update: 96 of 103 py/ files compile with vbcc (-c99)

Build invocation: `vc +snes-his -c99 -c -O1 -Ivbcc_spike/shim -I. -Imicropython
-Iport -Ibuild/mpy <file>`. Fixes folded into the workarounds patch /
mpconfigport.h (all __VBCC__-guarded or portable):
- offsetof override (vbcc builtin can't do nested members a.b)
- MP_NOINLINE / MP_NORETURN neutralized; SEEK_SET/CUR/END defined
- two iterator definitions made `static` to match their forward decls
  (objlist/objstr — portable, helps both compilers)
- `#define mp_hal_ticks_ms` to suppress py/mphal.h's clashing prototype

7 files still fail, each needing individual surgery:
- **binary.c:379** — genuine vbcc INTERNAL COMPILER ERROR (machine.c:3293) on
  a long long -> unsigned int conversion in mp_binary_set_int. Needs the
  expression rewritten to dodge the codegen bug. (vbcc bug #1 for us.)
- **modsys.c:63** — `static const mp_rom_obj_tuple_t = {{...},3,{...}}`. vbcc
  rejects ALL flexible-array-member *initializers* even with -c99 (verified
  with a 6-line standalone repro). This is the only core file with an inline
  FAM-init tuple; fix is the local-struct-with-fixed-items[N]+cast trick.
- **objgenerator.c:62** — mp_obj_malloc_var on `code_state.state` (offsetof of
  a nested FAM member); needs the size computed without offsetof-of-FAM.
- **objnamedtuple.h:40 / objcell.c:53 / objtype.c:326** — a member-after-FAM
  rejection, a variadic-macro (empty CELL_TYPE_PRINT) count issue, and an
  "invalid operand type" on an m_del/memcpy line respectively.

Note vbcc's FAM-init rejection is stricter than Calypsi (which silently
mis-emitted them). For the core it only bites modsys, but extmod/other code
would need the same treatment.

### Remaining roadmap to the decisive vbcc runtime test
1. Fix the 7 compile holdouts (above) — ~half a day incl. the ICE workaround.
2. vbcc memory model + custom linker: far/huge model, 56KB GC heap in WRAM,
   mailbox at $7FE000, frozen ROM data placement.
3. Standalone vbcc Makefile path (parallel to the Calypsi one).
4. Frozen module (reuse host mpy-cross/mpy-tool output — compiler-agnostic).
5. Run recursion / list iteration / method calls. THIS answers whether vbcc
   dodges the Calypsi bug. (Strong prior: yes, because vbcc has callee-saved
   registers r16-r27 — see the M4 root-cause entry.)

## 2026-06-13 — M4 root cause localized; no simple workaround exists

Followup to the M4-blocker entry below. Two important results:

### Root cause (localized)

Calypsi's calling convention (manual §"Calling convention", the line
"Registers A, X, Y and pseudo registers _Dp[0-7] are destroyed by a function
call. Pseudo registers _Dp[8-15] [are preserved]") means _Dp[0-7] are
caller-saved. The VM caches hot locals (notably `ip`, the bytecode pointer)
in these. In the very large, **re-entrant** `mp_execute_bytecode` (21 KB;
calling a Python function re-enters it), the compiler fails to preserve such
a caller-saved value across certain calls in some layouts — so after a call
`ip` reads garbage and the next dispatch sees an invalid opcode
(`NotImplementedError: opcode`) or wanders off (hang). This matches every
observed symptom and why builtins-only code (`print`) often survives while
Python-function calls / iteration / methods break.

### A simple save/restore workaround does NOT work — it only shuffles layout

Tried: `code_state->ip = ip; <call>; ip = code_state->ip;` around the CALL
opcodes (memory round-trip to dodge the dropped register). With **clean
builds**, this fixes some programs and BREAKS others (e.g. the 1-site variant
fixes a two-call program but hangs the M3 program). The "fix" merely changes
vm.o's size/layout, moving the lottery. Saving `sp` too made it strictly
worse. Conclusion: per-call-site C-level save/restore cannot robustly defeat
this; it's layout-sensitive codegen, not a missing save we can add.

### CRITICAL METHODOLOGY NOTE for future work

`make mpy` does NOT always rebuild `build/mpy/py/vm.o` after editing
`micropython/py/vm.c` (timestamp/rule interaction), which produced
**false-positive "fixes"** during this investigation. ALWAYS `rm -f
build/mpy/py/vm.o build/mpy/main.mpy build/mpy/frozen_content.*` (or `make
clean`) before re-testing a vm.c change. The emulator itself is fully
deterministic (verified: identical ROM → identical result across runs).

### Where this leaves M4

No quick win. Realistic paths, in order of preference:
1. Report to the Calypsi author (Håkan) with this characterization. A truly
   minimal standalone repro has been elusive (a 200-case switch wrapped in
   setjmp/longjmp does NOT reproduce), so the most useful report is likely
   the precise symptom + "_Dp[0-7] not preserved across a re-entrant call in
   a large function" + offer the project as a reproducer.
2. Structurally shrink `mp_execute_bytecode` (split cold opcode handlers into
   separate functions) so the triggering codegen doesn't occur — invasive and
   payoff uncertain (the exact trigger threshold is unknown).
3. Hold at M3 (done) and revisit when (1) lands.
M0–M3 remain green and committed; repo is at the clean M3 state.

## 2026-06-13 — M4 BLOCKED: pervasive VM miscompile on non-trivial programs

Starting M4 (run real `tests/basics/` files) immediately hit the wall flagged
at the end of M3: most non-trivial Python miscompiles. Findings from a long,
careful investigation (all on the emulator):

What WORKS (stable, at -O1): `print` of constants/ints, integer arithmetic,
`for i in range(n)` loops, `try/except`, defining and calling a simple
function once (`def f(x): return x+1; f(5)` → 6).

What FAILS (each a *different* symptom, and the symptom moves with link
layout — instrument anything and it changes):
- list iteration `for v in [..]:` → `NotImplementedError: opcode` (an opcode
  reaches the VM's ENTRY_DEFAULT, i.e. dispatch read a garbage opcode byte)
- recursion `g(n)=n+g(n-1)` → hangs (infinite loop / wedge)
- method calls `",".join([..])`, `"x".lower()` → `AttributeError` OR
  `NotImplementedError` OR empty-message exception, depending on layout

Crucially these are NOT distinct bugs — they are one underlying
layout-sensitive miscompilation that surfaces wherever a program exercises
enough of the giant `mp_execute_bytecode` (21 KB) function and/or deep
re-entrant call paths. The M3 program passes only because its particular
bytecode happens to land on a working layout.

Verified NON-causes (ruled out with evidence):
- Method lookup itself is correct: instrumented, `mp_map_lookup` returns the
  right element; the searched qstr id for `join` is 0x68 = 104, which is
  exactly join's computed qstr id. So qstr resolution + map lookup are fine.
- C stack overflow: hardware SP peaks ~600 B into the 7 KB bank-0 stack.
- Bank crossing: `mp_execute_bytecode` is 21 KB but wholly within bank $C0;
  `ip` is read as a 24-bit far pointer (`lda [dp]`), so bytecode reads are
  bank-safe.
- Heap pointer truncation alone: `print(w[1])` on a heap list WORKS.

Levers TRIED that did NOT robustly fix it:
- Optimization: -O0, -O1, -O2 global (each fails at a different point; -O1 is
  the most correct and is what M3 ships).
- Switch strategy on vm.o: if-else / jump-table / value-table (each
  miscompiles differently).
- `--no-cross-call`, `--no-interprocedural-cross-jump`, `--no-inline`
  globally (conservative opt) — still fails.
- vm.o at -O0 while rest at -O1 — still fails.
- Computed-goto dispatch (`MICROPY_OPT_COMPUTED_GOTO`): Calypsi 5.17 does NOT
  support GNU `&&label` (ICE "unexpected expr tag 83 TT_EndDeclStmt") — bug 7.

Assessment: this is a hard Calypsi codegen bug (likely register
allocation/spill or stack-slot management in very large and/or re-entrant
functions). Resolving it needs one of:
  (a) a minimal C reproduction + report to the Calypsi author (the PLAN notes
      he is responsive) and a compiler fix;
  (b) a structural workaround — split `mp_execute_bytecode` into smaller
      functions to dodge whatever size/pressure threshold trips the bug
      (invasive: the function is one body with cross-cutting gotos and an
      nlr landing pad);
  (c) bisecting vm.c / helpers to localise the exact miscompiled construct
      into a minimal repro, which feeds (a) or (b).
This is beyond a quick fix; M4 is paused here pending a direction decision.
M0–M3 remain green and committed.

## 2026-06-13 — M3 DONE: MicroPython bytecode runs on the SNES 🎉

`pytest tests/ -k mpy` is green: `port/main.py` is compiled to bytecode by
host `mpy-cross`, frozen into the ROM via `mpy-tool`, and executed by the
MicroPython VM on the emulated 65816. Exact output asserted:

```
hello from micropython on snes
sum of squares: 285
caught exception
done
```

That is print, 32-bit integer arithmetic, a `for` loop, and exception
raise/catch — the full M3 smoke set from the PLAN. Tag this commit.

### The fix: build py/ at -O1, not -O2

The decisive finding after deep triage (below): at **-O2** the VM is
miscompiled in a layout-sensitive way (symptom moves with any code/data
shift); at **-O1** the core VM is correct and stable. So `MPCFLAGS` uses
`-O1 --no-cross-call`, with `vm.o` additionally forced to `--force-switch
if-else`. Speed cost is acceptable (the PLAN says kHz-level is fine;
correctness first). Revisiting -O2 with a minimal repro is an upstream-report
task, not a blocker.

### Known limitation deferred to M4: method calls

At -O1, basic Python (print/arith/loop/exception) is rock-solid, but
attribute/method calls (`str.join`, `sorted(...)`, `"x".lower()`) fail with
`AttributeError: no such attribute` — the method *name* resolves correctly
(plain `print` via the same qstr path works), but the lookup in the type's
`locals_dict` map does not return the method. Root cause not yet pinned
(candidates: the bug-4 qstr/FAM restructure interacting with map lookup, or
another -O1 codegen issue in `mp_map_lookup`/`mp_load_method_maybe`).
`main.py` avoids methods; M4 will need this fixed to run real test files.

### Triage trail (how we got here), for the upstream report

- Symptom was wildly layout-sensitive: identical `vm.o` (byte-identical, same
  link address $C00000, no bank cross) gave correct output for one `main.py`
  and `NotImplementedError: opcode` for another. The variable is the frozen
  bytecode's address, i.e. data the VM reads — pointing at -O2 codegen that
  depends on absolute layout.
- Ruled OUT: C stack overflow (hardware stack peaked at ~600 B of the 7 KB
  bank-0 stack); bank-boundary crossing of `mp_execute_bytecode` (21 KB but
  sits wholly in bank $C0); the switch jump table per se (it's a 16-bit
  RTS-trick table, correct within one bank; `if-else` avoids it anyway).
- -O0 global: fails *earlier* (in exception handling) — fatter frames, more
  codegen surface, not better. -O1 global: correct VM, only methods fail.
- Tools built during triage: `tools`-free Lua probes under `/tmp/mbx`
  (PC histogram, stack low-water paint, opcode trace, function-entry traps)
  and synthetic switch+setjmp probe ROMs in `/tmp/probe` (a 200-case switch,
  even wrapped in setjmp/longjmp, does NOT reproduce — so the trigger is
  specific to the real 21 KB function, likely size/register-spill).

## 2026-06-13 — M3 earlier notes (superseded by the DONE entry above)

Frozen `port/main.py` is compiled by host `mpy-cross`, frozen via `mpy-tool`
into `frozen_content.c`, and executed by the VM on the 65816. Real Python
output has been observed on the emulator (loops, integer arithmetic,
exception raise+catch):

```
hello from micropython on snes
sum of squares: 285
caught: ValueError
```

Getting this far surfaced FOUR more Calypsi 5.17 codegen bugs beyond M1's two
(workarounds live in `patches/0001-calypsi-workarounds.patch`, applied by
`make patch-micropython`, and in the Makefile's MPCFLAGS):

### Calypsi bug 4: flexible-array-member initializers silently dropped

`const T x = { ..., .fam = { a, b } }` for a struct ending in `T fam[];`
emits the fixed fields but ZERO bytes for the FAM contents. MicroPython's
type system is built on this: every `mp_obj_type_t` ends in `const void
*slots[]` holding the method pointers, so every type came out with an
all-zero method table — the first method dispatch jumped to address 0.
qstr pools (`qstr_pool_t.qstrs[]`) have the same shape. Repro:
`bugs/calypsi-flexible-array-member-init-dropped.c`. Fix (patch): under
`__CALYPSI__`, give `mp_obj_type_t.slots` a fixed bound `[12]` (the macros
never pass more, prefix layout identical), and convert qstr pools to a
named-array + pointer field; `qstr_pool_new` and `mpy-tool.py`'s frozen
pool emitter updated to match.

### Calypsi bug 5: --cross-call corrupts indirect-call arguments

With cross-call optimization on (default), calls through the type's call
slot (`fun_builtin_var_call` -> builtin) passed a wrong arguments pointer —
`print` received a return address where the args array should be. Traced by
single-stepping: a clean indirect-call convention probe in `/tmp/probe`
passed, but the full build failed. Workaround: `--no-cross-call` in
MPCFLAGS. Cheap; cross-call is a size optimization.

### Calypsi bug 6 (OPEN — current blocker): mp_execute_bytecode miscompiles

`vm.c`'s `mp_execute_bytecode` is a ~17 KB function with a 256-case
dispatch switch and a setjmp/NLR landing pad. It is miscompiled in a
layout-sensitive way: every code/config perturbation breaks it
*differently* — wrong opcode handler, garbage indirect jump, hang, or a
bogus exception ("NotImplementedError: opcode", "ImportError", "negative
shift count" depending on switch strategy/-O level). `--force-switch
if-else` got the furthest (full correct output through the exception
handler) but is not robust. Synthetic probes so far do NOT reproduce it:
a 200-case switch is correct under every strategy, and a 200-case switch
wrapped in setjmp/longjmp is also correct (`/tmp/probe/switch200*.c`). So
the trigger is something else about this specific function — likely its
sheer size interacting with register spilling, or a specific opcode
handler's codegen. Next: bisect vm.c by `#if`-ing out opcode groups, or
split the dispatch loop into smaller functions.

Note: an earlier "final bug" turned out NOT to be a compiler bug —
`except ValueError as e` emits `DELETE_NAME`, which the minimal ROM config
legitimately doesn't implement (raises NotImplementedError at runtime).
`port/main.py` avoids `as e` and the test expects `caught: ValueError`.
`MICROPY_CPYTHON_COMPAT` would enable it but is left off to keep the ROM
minimal.

### Current state at this commit

`make mpy` builds; `pytest -k mpy` FAILS (exit 173, "NotImplementedError:
opcode") because of bug 6 with the committed build flags. M0/M1/M2 tests
still pass. The known-furthest recipe was `-O2 --no-cross-call` globally +
`vm.o` with `--force-switch if-else`; the committed Makefile has the
per-file `vm.o` if-else override. Heap/stack/mailbox layout from M2 unchanged.

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

## 2026-06-12 — M2: MicroPython core compiles, links, and mp_init runs

MicroPython is vendored as a git submodule pinned to **v1.28.0**; the port
lives in `port/` in this repo (thin-repo option from the plan) so the
submodule stays pristine except for tiny patches in `patches/`, applied
idempotently by `make patch-micropython`.

Port configuration (port/mpconfigport.h), the load-bearing choices:

- **`MICROPY_OBJ_REPR_B` + `MICROPY_OBJ_BASE_ALIGNMENT aligned(2)`**:
  Calypsi imposes NO data alignment (manual §11.7), which breaks tagged
  mp_obj_t pointers. Repr B only needs bit 0 clear; the aligned(2) on
  mp_obj_base_t forces evenness of every object struct. Precedent:
  ports/pic16bit hit the identical problem with xc16 ("doesn't seem to
  respect alignment (!!)") and chose repr B.
- mp_int_t = intptr_t = 32-bit (mpconfig.h default — Calypsi intptr_t is 4
  bytes even though int is 2). size_t is 16-bit; fine, no object exceeds 64K.
- `typedef int32_t ssize_t` in mpconfigport.h: Calypsi has no POSIX ssize_t.
- `MP_LIKELY/MP_UNLIKELY` overridden to plain (x): no __builtin_expect.
- NDEBUG defined; `__attribute__((noreturn))` is unknown to Calypsi (warns,
  harmless).
- Minimal ROM level, compiler off, NLR setjmp, no float, no longint.

### Calypsi bug 3: ICE "internal error: unable to label"

Eight py/ files ICE'd at every -O level. Delta-debugging landed on
MP_STATIC_ASSERT_NONCONSTEXPR — `sizeof(char[1 - 2 * !(&a != &b)])` with
extern-address comparisons. A couple per function are fine; the ~8 that
mp_obj_is_type expands into one expression tree ICE the code generator.
Patch `patches/0001-calypsi-no-nonconstexpr-static-assert.patch` adds
`|| defined(__CALYPSI__)` to the existing MSVC/C++ opt-out in py/misc.h
(1 line; the macro is a compile-time-only sanity check, losing it costs
nothing at runtime). Bisect false-starts worth remembering: an invalid
flag makes cc65816 print usage and exit — which greps as "no internal
error" — and prefix-truncation bisects converge on function boundaries,
not the true trigger, because syntax errors mask the ICE.

### qstr pipeline

As planned: host `gcc -E` does all preprocessing for makeqstrdefs.py /
makeqstrdata.py / makemoduledefs.py / make_root_pointers.py (type sizes are
irrelevant for identifier extraction); only real compiles use cc65816.
Worked unchanged on the first try.

### Stack: mp_init needs ~6KB (!)

First boot wrote "mp_init ok / M2 done" into the ring and then the harness
reported "ROM never booted": the C stack (2KB, top $17FF) had plunged ~5.9KB
deep during mp_init — straight through the old mailbox at $7E0100, zeroing
the magic word. Measured low-water mark via Lua sentinel paint: $0102.
Calypsi frames push 32-bit pseudo-registers, so C stacks here are fat.
Consequences:
- Mailbox moved to the top of bank $7F ($7FE000 header, $7FE010 ring of
  8176 bytes); bank-0 $0100-$1FFF (7.25KB) is now all C stack.
- GC heap: $7F0000-$7FDFFF (56KB), within the planned 48-64KB.
- mp_cstack_init_with_top gets the true stack size so Python-level depth
  checks trip before hardware overflow.

Tree-shaking note: the M2 ROM (67KB raw) only pulls in what mp_init
references; the full VM lands with M3's frozen code.

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
