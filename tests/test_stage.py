"""M8: the Stage game library (python-ugame) runs on the SNES PPU.

The mpystage ROM freezes the ported stage.py + generated assets +
port/main_stage.py: a tile bank uploaded to VRAM, a brick-arena Grid on
the BG1 tilemap, and six bouncing 16x16 hardware sprites moved through
the OAM shadow for 120 game frames. The mailbox transcript is asserted;
the screen shows the game running.
"""

from conftest import ROOT, STATUS_PASS, build, run_rom

EXPECTED = """\
stage: init ok
stage: frame 0
stage: frame 30
stage: frame 60
stage: frame 90
stage: done
"""


def test_stage_demo(mesen_env, tmp_path):
    build("mpystage")
    code, text = run_rom(ROOT / "build" / "mpystage.sfc", mesen_env, tmp_path,
                         max_frames=36000)
    assert text == EXPECTED, f"exit {code}, log:\n{text}"
    assert code == STATUS_PASS
