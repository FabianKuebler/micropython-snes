// MicroPython configuration for the SNES port (Calypsi 65816, large/large).
//
// Target facts that shape everything here (see DECISIONS.md):
//  - int is 16-bit, long/void*/intptr_t are 32-bit, size_t is 16-bit
//  - the compiler imposes NO data alignment, so tagged mp_obj_t pointers
//    need MICROPY_OBJ_REPR_B (only bit 0 must be clear) plus 2-byte
//    alignment on every object definition. CRITICAL (root cause of the
//    long M4 hunt): Calypsi honors __attribute__((aligned)) ONLY at
//    variable position (guide 11.7); on struct members/types it is
//    silently ignored, so the ports/pic16bit trick of aligning
//    mp_obj_base_t does nothing here. The patches in patches/ therefore
//    put MICROPY_OBJ_BASE_ALIGNMENT at variable position inside every
//    ROM-object definition macro (MP_DEFINE_CONST_*, MP_DEFINE_STR_OBJ,
//    module/singleton definitions, mpy-tool.py frozen objects), and
//    tools/check_obj_align.py fails the link if any object lands odd.
//  - far pointers cannot cross 64K banks: the GC heap lives entirely in
//    WRAM bank $7F
#ifndef MICROPY_INCLUDED_MPCONFIGPORT_H
#define MICROPY_INCLUDED_MPCONFIGPORT_H

#include <stdint.h>

// CORE_FEATURES since nano-gui: slicing, property, enumerate, str.format
// and friends are gated below this level (ROM is 512KB+, there is room)
#define MICROPY_CONFIG_ROM_LEVEL (MICROPY_CONFIG_ROM_LEVEL_CORE_FEATURES)
// ...but no filesystem: io/open would need a stream backend
#define MICROPY_PY_IO (0)
// f-strings are EXTRA-level upstream but cheap (lexer-only) and expected
// at an interactive prompt
#define MICROPY_PY_FSTRINGS (1)

// Frozen modules are compiled on the host with mpy-cross, but the compiler
// also runs ON TARGET for the REPL (build/mpyrepl.sfc) and eval/exec.
#define MICROPY_ENABLE_COMPILER (1)
#define MICROPY_HELPER_REPL (1)
// The recursive-descent parser/compiler must fail soft on deep input: the
// C stack is only ~7.4KB (bank 0)
#define MICROPY_STACK_CHECK (1)
#define MICROPY_STACK_CHECK_MARGIN (1024)
#define MICROPY_MODULE_FROZEN_MPY (1)
#define MICROPY_QSTR_EXTRA_POOL mp_qstr_frozen_const_pool
#define MICROPY_ENABLE_GC (1)
#define MICROPY_NLR_SETJMP (1)

#define MICROPY_OBJ_REPR (MICROPY_OBJ_REPR_B)
#if defined(__VBCC__)
// vbcc rejects __attribute__((aligned)) here; GC block alignment keeps
// objects even-addressed (repr B only needs bit 0 clear)
#define MICROPY_OBJ_BASE_ALIGNMENT
#define MP_NORETURN
#define MP_NOINLINE
// vbcc's builtin offsetof can't handle nested members (a.b); use the classic
// address-of-member-in-null form, which it does handle. Pull in <stddef.h>
// FIRST so its include guard is set — otherwise a later system include of
// <stddef.h> (e.g. from py/obj.h) re-defines offsetof back to the builtin and
// clobbers this override (objgenerator.c's offsetof(t, code_state.state)).
#include <stddef.h>
#undef offsetof
#define offsetof(t, m) ((size_t)((char *)&(((t *)0)->m) - (char *)0))
// vbcc's stdio.h lacks these (used by py/stream.c)
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
#else
#define MICROPY_OBJ_BASE_ALIGNMENT __attribute__((aligned(2)))
#endif

// heap is < 64K so 16-bit GC mark-stack entries suffice
#define MICROPY_GC_STACK_ENTRY_TYPE uint16_t
#define MICROPY_GC_ALLOC_THRESHOLD (0)
// Calypsi packs structs (size_t is 16-bit, no alignment): the mp_state root
// section has pointers at 2-mod-4 offsets, needing the phase-shifted scan
#define MICROPY_GC_UNALIGNED_ROOT_SECTION (1)

#define MICROPY_ALLOC_PATH_MAX (64)
#define MICROPY_NO_ALLOCA (1)
// Full import machinery so frozen PACKAGES resolve (nano-gui is a package
// tree: gui.core.*, gui.widgets.*). There is no filesystem: mp_import_stat
// in the port always answers NO_EXIST, so only frozen modules are found.
#define MICROPY_ENABLE_EXTERNAL_IMPORT (1)
// complex/cmath for nano-gui's Dial widget (clock hands are polar floats)
#define MICROPY_PY_BUILTINS_COMPLEX (1)
#define MICROPY_PY_CMATH (1)
#define MICROPY_ERROR_REPORTING (MICROPY_ERROR_REPORTING_NORMAL)
#define MICROPY_ROM_TEXT_COMPRESSION (0)
// Floats: Calypsi's C library provides IEEE single-precision incl. the
// f-suffixed math functions (sqrtf, sinf, ...) that FLOAT_IMPL_FLOAT uses.
// Needed for nano-gui (widget geometry works in floats).
#define MICROPY_FLOAT_IMPL (MICROPY_FLOAT_IMPL_FLOAT)
#define MICROPY_PY_MATH (1)
#define MICROPY_LONGINT_IMPL (MICROPY_LONGINT_IMPL_NONE)

// nano-gui prerequisites: framebuf for drawing, memoryview + bytearray for
// Writer and the display buffer
#define MICROPY_PY_FRAMEBUF (1)
#define MICROPY_PY_BUILTINS_MEMORYVIEW (1)
#define MICROPY_PY_BUILTINS_BYTEARRAY (1)

#if !defined(__VBCC__)
// Calypsi's math.h/libm lack C99 signbit and nearbyint*; shim them.
// signbit(x)<0 misses negative zero — acceptable (only affects "-0.0" repr).
#define signbit(x) ((x) < 0)
#define nearbyintf roundf
#define nearbyint round
#endif

#define MP_ENDIANNESS_LITTLE (1)

// Calypsi's clang front end has no __builtin_expect
#define MP_LIKELY(x) (x)
#define MP_UNLIKELY(x) (x)

typedef int32_t mp_off_t;

// Calypsi's libc has no ssize_t (POSIX, not ISO C); py/ uses it as a
// signed length hint. 32-bit is roomy (size_t here is only 16-bit).
typedef int32_t ssize_t;

#define MP_STATE_PORT MP_STATE_VM

#define MICROPY_HW_BOARD_NAME "snes"
#define MICROPY_HW_MCU_NAME "65816"

#endif
