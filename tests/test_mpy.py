"""M2: MicroPython core, compiled with Calypsi, boots on the emulated SNES.

The DoD for M2 is compile+link; this test goes one step further and proves
mp_init()/mp_deinit() execute on the 65816 (GC heap setup, qstr pools,
module table init) — the foundation M3 builds on.
"""

from conftest import ROOT, STATUS_PASS, build, run_rom


def test_mp_init(mesen_env, tmp_path):
    build("mpy")
    code, text = run_rom(ROOT / "build" / "mpy.sfc", mesen_env, tmp_path)
    assert text == "mp_init ok\nM2 done\n", f"exit {code}, log:\n{text}"
    assert code == STATUS_PASS
