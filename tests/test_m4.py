"""M4: the decisive constructs run on the emulated SNES.

Everything that used to break the port -- recursion, Python-to-Python calls,
list iteration, method calls -- plus classes, closures, generators and nested
exception unwinding. Requires the split VM (VM_SPLIT=1, the default) and the
ROM-object alignment fix; see DECISIONS.md. port/main_m4.py is frozen into
build/mpy4.sfc and its output asserted byte-for-byte.
"""

from conftest import ROOT, STATUS_PASS, build, run_rom

EXPECTED = """\
hello from micropython on snes
fact(10): 3628800
is_even(9): False
list sum: 100 len: 5
join: a,b,c
lower: hello
dict c: 3
counter: 10
closure: 42
gen sum: 30
caught nested, finally ran: 1
done
"""


def test_frozen_m4(mesen_env, tmp_path):
    build("mpy4")
    code, text = run_rom(ROOT / "build" / "mpy4.sfc", mesen_env, tmp_path,
                         max_frames=3600)
    assert text == EXPECTED, f"exit {code}, log:\n{text}"
    assert code == STATUS_PASS
