"""M6: the on-console REPL driven by the JOYPAD.

The REPL boots with a demo block preloaded at the continuation prompt
(pyexec_repl's preload — skipped for scripted-stdin sessions, so only this
joypad test sees it). The harness scripts controller 1 through Mesen's
inputPolled event: press Start to run the pending demo, then navigate the
on-screen keyboard grid, type "1+2" (page-0 grid: digits row, symbol row),
press Start to run it. The mailbox transcript is asserted byte-for-byte.

In the standalone REPL ROM a joypad Select (oskb ^D) must be IGNORED —
exiting would leave real hardware on a dead screen; only a scripted stdin
^D ends the loop. So the tail of this test holds Select, asserts no "bye"
appears, and expects the run to end at the frame cap (EXIT_TIMEOUT).
"""

from conftest import EXIT_TIMEOUT, ROOT, build, run_rom


def press(*buttons):
    seq = []
    for b in buttons:
        seq += [b, "", ""]  # 1 frame held, 2 released: clean edges
    return seq


# grid starts at 'a' (0,0). '1' is (1,2); '+' is (10,3); '2' is (2,2).
JOYPAD = ([""] * 30                                       # let the preload echo
          + press("start")                                # run the boot demo
          + [""] * 420                                    # ...and let it execute
          + press("down", "down", "right", "a")           # 1
          + press("down", *["right"] * 9, "a")            # +
          + press("up", *["left"] * 8, "a")               # 2
          + press("start")                                # run
          + ["select"] * 240)  # held: must be ignored (no exit path here)

EXPECTED = "\n".join([
    "MicroPython on SNES",
    ">>> for i in range(3):",
    '...   print(f"hello SNES {i}")',
    "... ",
    "hello SNES 0",
    "hello SNES 1",
    "hello SNES 2",
    ">>> 1+2",
    "3",
    ">>> ",  # still at the prompt: Select did NOT exit
])


def test_joypad_repl(mesen_env, tmp_path):
    build("mpyrepl")
    code, text = run_rom(ROOT / "build" / "mpyrepl.sfc", mesen_env, tmp_path,
                         max_frames=3600, joypad=JOYPAD)
    assert text == EXPECTED, f"exit {code}, log:\n{text}"
    assert code == EXIT_TIMEOUT
