"""M3: frozen Python bytecode executes on the emulated SNES.

THIS is the "micropython runs on a SNES" moment: port/main.py is compiled
to bytecode on the host (mpy-cross), frozen into the ROM, and executed by
the MicroPython VM on the 65816. Output asserted byte-for-byte.
"""

from conftest import ROOT, STATUS_PASS, build, run_rom

EXPECTED = """\
hello from micropython on snes
sum of squares: 285
caught: ValueError
65816-on-py
done
"""


def test_frozen_main(mesen_env, tmp_path):
    build("mpy")
    code, text = run_rom(ROOT / "build" / "mpy.sfc", mesen_env, tmp_path,
                         max_frames=3600)
    assert text == EXPECTED, f"exit {code}, log:\n{text}"
    assert code == STATUS_PASS
