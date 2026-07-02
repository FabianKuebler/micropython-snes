import json
import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
HARNESS_LUA = Path(__file__).parent / "mailbox_harness.lua.in"

# Mailbox protocol (see DECISIONS.md). Status word doubles as Mesen exit code.
STATUS_PASS = 1
STATUS_PANIC = 0xDEAD
EXIT_TIMEOUT = 99
EXIT_NO_MAGIC = 98
EXIT_RING_OVERFLOW = 97


def find_mesen():
    for candidate in (os.environ.get("MESEN_PATH"), shutil.which("Mesen"),
                      os.path.expanduser("~/bin/Mesen")):
        if candidate and os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    return None


MESEN = find_mesen()


@pytest.fixture(scope="session")
def mesen_env(tmp_path_factory):
    """Mesen's real config is read-only from the sandbox: copy it, enable
    Lua io/os access and a script timeout, point Mesen at the copy."""
    if MESEN is None:
        pytest.skip("Mesen emulator not found (set MESEN_PATH)")
    cfgroot = tmp_path_factory.mktemp("mesencfg")
    src = Path.home() / ".config" / "Mesen2"
    if not src.is_dir():
        pytest.skip(f"{src} not found")
    shutil.copytree(src, cfgroot / "Mesen2")
    settings = cfgroot / "Mesen2" / "settings.json"
    s = json.loads(settings.read_text(encoding="utf-8-sig"))
    s["Debug"]["ScriptWindow"]["AllowIoOsAccess"] = True
    s["Debug"]["ScriptWindow"]["ScriptTimeout"] = 120
    settings.write_text(json.dumps(s, indent=2), encoding="utf-8-sig")
    env = dict(os.environ,
               XDG_CONFIG_HOME=str(cfgroot),
               DOTNET_SYSTEM_GLOBALIZATION_INVARIANT="1",
               SDL_VIDEODRIVER="dummy")
    return env


def build(*targets):
    res = subprocess.run(["make", *targets], cwd=ROOT, capture_output=True, text=True)
    assert res.returncode == 0, f"build failed:\n{res.stdout[-2000:]}\n{res.stderr[-2000:]}"


def lua_bytes(data):
    """Encode bytes as a Lua short-string literal body (decimal escapes)."""
    return "".join("\\%d" % b for b in data)


def run_rom(rom, mesen_env, tmp_path, max_frames=1800, stdin_data=b"",
            joypad=()):
    """Run a mailbox-protocol ROM under Mesen; return (exit_code, stdout_text).

    stdin_data is fed into the ROM's stdin ring by the harness (REPL input).
    joypad is a per-frame button-name sequence for controller 1 ("" =
    released); see mailbox_harness.lua.in.
    """
    log = tmp_path / "mailbox.log"
    lua = (HARNESS_LUA.read_text()
           .replace("@LOGFILE@", str(log))
           .replace("@MAXFRAMES@", str(max_frames))
           .replace("@STDIN@", lua_bytes(stdin_data))
           .replace("@JOYSEQ@", ",".join('"%s"' % b for b in joypad)))
    script = tmp_path / "harness.lua"
    script.write_text(lua)
    res = subprocess.run([MESEN, "--testrunner", str(rom), str(script)],
                         env=mesen_env, capture_output=True, text=True, timeout=300)
    text = log.read_text() if log.exists() else ""
    return res.returncode, text
