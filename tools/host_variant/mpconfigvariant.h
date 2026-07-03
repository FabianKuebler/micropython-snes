// Host reference build mirroring the SNES port's feature level
// (port/mpconfigport.h): used by tools/run_upstream_tests.py to generate
// expected outputs for the on-target upstream-test run.
#define MICROPY_CONFIG_ROM_LEVEL (MICROPY_CONFIG_ROM_LEVEL_CORE_FEATURES)

#define MICROPY_EMIT_X86 (0)
#define MICROPY_EMIT_X64 (0)
#define MICROPY_EMIT_THUMB (0)
#define MICROPY_EMIT_ARM (0)

#define MICROPY_PY_BUILTINS_COMPLEX (1)
#define MICROPY_PY_CMATH (1)
#define MICROPY_FLOAT_IMPL (MICROPY_FLOAT_IMPL_FLOAT)
#define MICROPY_PY_MATH (1)
#define MICROPY_ERROR_REPORTING (MICROPY_ERROR_REPORTING_NORMAL)
#define MICROPY_PY_FRAMEBUF (1)

// unix main.c's REPL needs these helpers (the SNES port sets it too)
#define MICROPY_HELPER_REPL (1)
#define MICROPY_KBD_EXCEPTION (1)
// match the target: no arbitrary-precision ints on the SNES (yet)
#define MICROPY_LONGINT_IMPL (MICROPY_LONGINT_IMPL_NONE)
// match the target: no io module
#define MICROPY_PY_IO (0)
