"""M7: peterhinch/micropython-nano-gui runs on the SNES.

The mpygui ROM freezes the vendored nano-gui package tree plus
port/main_gui.py: a Meter, LED, Dial (complex-number pointer via cmath),
Labels and a CWriter with the arial10 font, rendered through the snesfb
unique-tile framebuffer (256x192, 16 colors) over three refresh cycles.
The mailbox transcript is asserted; the screen shows the widgets.
"""

from conftest import ROOT, STATUS_PASS, build, run_rom

EXPECTED = """\
nanogui: init ok
nanogui: frame 0 value 0.1
nanogui: frame 1 value 0.45
nanogui: frame 2 value 0.8
nanogui: done
"""


def test_nanogui_demo(mesen_env, tmp_path):
    build("mpygui")
    code, text = run_rom(ROOT / "build" / "mpygui.sfc", mesen_env, tmp_path,
                         max_frames=36000)
    assert text == EXPECTED, f"exit {code}, log:\n{text}"
    assert code == STATUS_PASS
