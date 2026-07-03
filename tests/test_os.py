"""M9: the all-in-one workstation ROM (build/mpyos.sfc).

Boots into the C file manager over battery SRAM. Scripted sessions drive
it over two planes: mailbox stdin control bytes (0x05=A 0x06=B 0x0E=Y
0x10-0x13=dpad, 0x04=quit/menu; text bytes where text is accepted) and
JOYSEQ joypad frames. The manager/editor print breadcrumbs (fm:/ed:) to
the mailbox for assertions.
"""

from conftest import ROOT, STATUS_PASS, build, clear_srm, run_rom


def _run(mesen_env, tmp_path, **kw):
    return run_rom(ROOT / "build" / "mpyos.sfc", mesen_env, tmp_path, **kw)


def test_boot_list_repl_delete(mesen_env, tmp_path):
    build("mpyos")
    clear_srm(mesen_env)
    # boot -> 4 entries (starter hello.py + 3 frozen); Select -> REPL,
    # evaluate 1+1, ^D back to the manager; menu on hello.py, Delete
    # (down x2), confirm; quit
    stdin = (b"\x0f" + b"1+1\n" + b"\x04"
             + b"\x05" + b"\x11\x11" + b"\x05" + b"\x05" + b"\x04")
    code, text = _run(mesen_env, tmp_path, max_frames=14000, stdin_data=stdin)
    assert code == STATUS_PASS, f"exit {code}, log:\n{text}"
    assert "fm: 4 entries" in text
    assert "fm: repl" in text
    assert ">>> 1+1\n2\n" in text
    assert "fm: del hello.py" in text
    assert "fm: quit" in text


def test_run_frozen_stage_stdin(mesen_env, tmp_path):
    build("mpyos")
    clear_srm(mesen_env)
    # down to [ROM] demo_stage.py, A -> Run; B back after the demo; quit.
    # The stage demo runs ~120 game frames at <1 fps: ~3 minutes emulated.
    stdin = b"\x11" + b"\x05" + b"\x05" + b"\x06" + b"\x04"
    code, text = _run(mesen_env, tmp_path, max_frames=36000, stdin_data=stdin)
    assert code == STATUS_PASS, f"exit {code}, log:\n{text}"
    assert "fm: run demo_stage" in text
    assert "stage: init ok" in text
    assert "stage: done" in text
    # the manager came back after the run (PPU recovered, list redrawn)
    assert text.rindex("fm: 4 entries") > text.index("stage: done")


def hold(button, frames=6, gap=10):
    """One deliberate press: the manager's full-screen redraw spans a few
    frames, so a 1-frame tap (M6 style) can fall into a redraw gap."""
    return [button] * frames + [""] * gap


def test_run_file_joypad(mesen_env, tmp_path):
    build("mpyos")
    clear_srm(mesen_env)
    # pure joypad plane: A on hello.py (selected at boot) -> menu -> Run,
    # hold B through the run's end, Start quits. NB Mesen's testrunner
    # hard-kills the Lua script at ~100s of WALL time (regardless of
    # ScriptTimeout), so joypad tests must stay short — the long stage-demo
    # run is covered by the stdin variant above, which fits under the cap.
    # (run-length entries: the harness expands "b*3000" at poll time)
    joy = ([""] * 60 + hold("a") + hold("a")
           + ["b*3000", "*30"] + hold("start"))
    code, text = _run(mesen_env, tmp_path, max_frames=12000, joypad=joy)
    assert code == STATUS_PASS, f"exit {code}, log:\n{text}"
    assert "fm: run hello.py" in text
    assert "hello from SRAM" in text
    assert "os: back" in text
    assert "fm: quit" in text


def test_editor_create_save_run_persist(mesen_env, tmp_path):
    build("mpyos")
    clear_srm(mesen_env)
    stdin = (
        b"\x0e"            # Y: new file
        + b"hi\n"          # name (becomes hi.py)
        + b"print(123)\n"  # type the program
        + b"\x04"          # editor menu
        + b"\x11\x11\x11\x05"  # down x3 -> Save+Run, A
        + b"\x06"          # B: back from the run
        + b"\x04"          # menu again (editor re-entered after run)
        + b"\x11\x11\x11\x11\x05"  # down x4 -> Save+Exit, A
        + b"\x04"          # quit from the manager
    )
    code, text = _run(mesen_env, tmp_path, max_frames=18000, stdin_data=stdin)
    assert code == STATUS_PASS, f"exit {code}, log:\n{text}"
    assert "fm: name hi.py" in text
    assert "ed: saved hi.py 11" in text
    assert "\n123" in text  # the program ran
    assert "fm: 5 entries" in text  # hi.py listed after save+exit

    # battery persistence: reboot (same .srm), hi.py still there
    code, text = _run(mesen_env, tmp_path, max_frames=7200, stdin_data=b"\x04")
    assert code == STATUS_PASS, f"exit {code}, log:\n{text}"
    assert "fm: 5 entries" in text
