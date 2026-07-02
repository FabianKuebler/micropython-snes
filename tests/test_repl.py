"""M5: interactive REPL — Python is COMPILED ON THE 65816.

The harness feeds a scripted session into the mailbox stdin ring; the ROM's
lexer/parser/bytecode-compiler run on target and the split VM executes the
result. Covers expression results (__repl_print__), builtins, multi-line
compound statements with continuation prompts, calling a just-defined
function, and a runtime exception with traceback. Ctrl-D exits cleanly.
"""

from conftest import ROOT, STATUS_PASS, build, run_rom

SESSION = b'1+2\nprint("hi" * 3)\ndef sq(x):\n    return x*x\n\nsq(12)\n1//0\n\x04'

# built line-by-line: bare continuation/exit prompts carry a trailing space
EXPECTED = "\n".join([
    "MicroPython on SNES; Ctrl-D exits",
    ">>> 1+2",
    "3",
    '>>> print("hi" * 3)',
    "hihihi",
    ">>> def sq(x):",
    "...     return x*x",
    "... ",
    ">>> sq(12)",
    "144",
    ">>> 1//0",
    "Traceback (most recent call last):",
    '  File "<stdin>", in <module>',
    "ZeroDivisionError: divide by zero",
    ">>> ",
    "bye",
    "",
])


def test_repl_session(mesen_env, tmp_path):
    build("mpyrepl")
    code, text = run_rom(ROOT / "build" / "mpyrepl.sfc", mesen_env, tmp_path,
                         max_frames=14400, stdin_data=SESSION)
    assert text == EXPECTED, f"exit {code}, log:\n{text}"
    assert code == STATUS_PASS
