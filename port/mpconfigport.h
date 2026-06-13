// MicroPython configuration for the SNES port (Calypsi 65816, large/large).
//
// Target facts that shape everything here (see DECISIONS.md):
//  - int is 16-bit, long/void*/intptr_t are 32-bit, size_t is 16-bit
//  - the compiler imposes NO data alignment, so tagged mp_obj_t pointers
//    need MICROPY_OBJ_REPR_B (only bit 0 must be clear) plus a forced
//    2-byte alignment on mp_obj_base_t — same trick as ports/pic16bit,
//    whose xc16 compiler has the same quirk
//  - far pointers cannot cross 64K banks: the GC heap lives entirely in
//    WRAM bank $7F
#ifndef MICROPY_INCLUDED_MPCONFIGPORT_H
#define MICROPY_INCLUDED_MPCONFIGPORT_H

#include <stdint.h>

#define MICROPY_CONFIG_ROM_LEVEL (MICROPY_CONFIG_ROM_LEVEL_MINIMUM)

// Bytecode VM only: Python is compiled on the host with mpy-cross and
// frozen into the ROM
#define MICROPY_ENABLE_COMPILER (0)
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
#else
#define MICROPY_OBJ_BASE_ALIGNMENT __attribute__((aligned(2)))
#endif

// heap is < 64K so 16-bit GC mark-stack entries suffice
#define MICROPY_GC_STACK_ENTRY_TYPE uint16_t

#define MICROPY_ALLOC_PATH_MAX (64)
#define MICROPY_NO_ALLOCA (1)
#define MICROPY_ENABLE_EXTERNAL_IMPORT (0)
#define MICROPY_ERROR_REPORTING (MICROPY_ERROR_REPORTING_TERSE)
#define MICROPY_ROM_TEXT_COMPRESSION (0)
#define MICROPY_FLOAT_IMPL (MICROPY_FLOAT_IMPL_NONE)
#define MICROPY_LONGINT_IMPL (MICROPY_LONGINT_IMPL_NONE)

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
