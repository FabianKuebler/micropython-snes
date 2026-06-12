"""M0: the Calypsi-built hello ROM boots in Mesen and talks via the mailbox."""

from conftest import ROOT, STATUS_PASS, build, run_rom


def test_hello(mesen_env, tmp_path):
    build("hello")
    code, text = run_rom(ROOT / "build" / "hello.sfc", mesen_env, tmp_path)
    assert code == STATUS_PASS, f"exit {code}, log: {text!r}"
    assert text == "Hello from Calypsi on SNES\nM0 done\n"
