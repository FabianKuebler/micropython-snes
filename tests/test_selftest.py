"""M1: compiler trust-but-verify self-test ROM, run on the emulated 65816.

Asserts the exact transcript so a miscompile cannot hide: every check must
report, in order, and any FAIL line (with its got/want dump) breaks the match.
"""

from conftest import ROOT, STATUS_PASS, build, run_rom

CHECKS = [
    "u32.add", "u32.sub", "u32.mul", "u32.div", "u32.mod",
    "u32.shl", "u32.shr", "u32.cmp",
    "i32.div", "i32.mod", "i32.sar", "i32.neg", "i32.cmp",
    "u64.add", "u64.mul", "u64.shl", "u64.shr", "u64.div",
    "i64.div", "i64.mod",
    "ptr.farhi", "ptr.bank7f", "ptr.diff",
    "struct.copy", "struct.byval",
    "switch.dense", "switch.sparse", "switch.long",
    "fptr.table", "fptr.arg",
    "setjmp.basic", "setjmp.deep",
    "va.int", "va.long", "va.longlong", "va.fmt",
    "rec.depth32",
    "libc.memset", "libc.memcpy", "libc.strcmp", "libc.memcpyfar",
]

EXPECTED = ("M1 selftest\n"
            + "".join(f"ok {name}\n" for name in CHECKS)
            + "M1 selftest passed\n")


def test_selftest(mesen_env, tmp_path):
    build("selftest")
    code, text = run_rom(ROOT / "build" / "selftest.sfc", mesen_env, tmp_path)
    assert text == EXPECTED, f"exit {code}, log:\n{text}"
    assert code == STATUS_PASS
