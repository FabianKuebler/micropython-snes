#!/usr/bin/env bash
# Idempotently fix two non-conformances in vbcc65816 r2's SNES target headers
# that break the MicroPython port. The toolchain is gitignored and re-fetchable
# (see vbcc_spike/README.md), so this must be re-run after a fresh fetch; the
# vbcc Makefile invokes it automatically.
#
#  1. stdint.h hardcodes `typedef int intptr_t;` (16-bit) — its own comment even
#     says "FIXME: depends on memory model". In the far/huge model a data
#     pointer is 24-bit, so a 16-bit intptr_t/uintptr_t TRUNCATES the bank byte
#     ((uintptr_t)0x7FE000 == 0xE000), breaking mp_int_t and every pointer<->int
#     round-trip. Make them 32-bit (long).
#  2. assert.h's NDEBUG branch expands assert(x) to *empty* instead of the
#     C-required ((void)0), which turns MicroPython's
#     `(MP_STATIC_ASSERT(...), assert(...), ...)` comma chains into a syntax
#     error (double comma).
set -eu
VBCC="${VBCC:-$(cd "$(dirname "$0")/.." && pwd)/vbcc-toolchain/vbcc65816/vbcc65816_linux/vbcc}"
INC="$VBCC/targets/65816-snes/include"

SH="$INC/stdint.h"
sed -i \
  -e 's/^typedef int intptr_t;/typedef long intptr_t;/' \
  -e 's/^#define INTPTR_MIN INT_MIN/#define INTPTR_MIN LONG_MIN/' \
  -e 's/^#define INTPTR_MAX INT_MAX/#define INTPTR_MAX LONG_MAX/' \
  -e 's/^typedef unsigned int uintptr_t;/typedef unsigned long uintptr_t;/' \
  -e 's/^#define UINTPTR_MAX UINT_MAX/#define UINTPTR_MAX ULONG_MAX/' \
  "$SH"

AH="$INC/assert.h"
sed -i 's|^#define assert(exp)$|#define assert(exp) ((void)0)|' "$AH"

echo "vbcc toolchain headers patched (intptr_t=32-bit, NDEBUG assert=((void)0))"
