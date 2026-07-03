#!/usr/bin/env python3
"""Run upstream MicroPython tests (micropython/tests/) on the SNES.

For each test file:
  - expected output comes from the host reference interpreter
    (ports/unix VARIANT=snesref, feature level mirroring the SNES port);
  - actual output comes from build/mpyrepl.sfc running in Mesen: tests are
    fed in batches through the mailbox stdin ring using the REPL's raw mode
    (^A <source> ^D), each test's output bracketed in STX/EOT. The REPL
    resets the interpreter (mp_deinit/mp_init) between tests.

Categories: pass, fail (diff), skip (reference itself can't run it or the
test asks to be skipped), timeout (batch aborted; test marked, rest rerun).

Usage: run_upstream_tests.py [--limit N] [--batch N] [testdir_or_file ...]
Writes a report to build/upstream_results.txt and prints a summary.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HOST = ROOT / "micropython/ports/unix/build-snesref/micropython"
ROM = ROOT / "build/mpyrepl.sfc"
HARNESS_IN = ROOT / "tests/mailbox_harness.lua.in"
MESEN = Path.home() / "bin/Mesen"
WORK = Path("/tmp/mbx")
EXP_CACHE = ROOT / "build/upstream_exp"

# emulated frames per batch: generous; a hung test costs one batch timeout
FRAMES_PER_TEST = 10800  # 3 emulated minutes
LINE_RE = re.compile(rb", line \d+")
FILE_RE = re.compile(rb'File "[^"]*"')  # host: test filename, target: <stdin>


def reference_output(test: Path) -> bytes | None:
    """Expected output, cached. None => reference cannot run it (skip)."""
    exp = EXP_CACHE / (test.name + ".exp")
    if exp.exists():
        data = exp.read_bytes()
        return None if data == b"\x00SKIP\x00" else data
    try:
        r = subprocess.run([str(HOST), test.name], capture_output=True,
                           timeout=30, cwd=test.parent)
        out = r.stdout + r.stderr
    except subprocess.TimeoutExpired:
        out = None
    if out is None or out.startswith(b"SKIP") or b"SyntaxError" in out and r.returncode:
        # reference can't run it -> nothing to compare against
        exp.write_bytes(b"\x00SKIP\x00")
        return None
    exp.write_bytes(out)
    return out


def normalize(b: bytes) -> bytes:
    b = b.replace(b"\r\n", b"\n").replace(b"\r", b"\n")
    b = LINE_RE.sub(b"", b)  # host tracebacks carry line numbers, target's don't
    b = FILE_RE.sub(b'File ""', b)  # and the filename (target compiles from <stdin>)
    return b.strip() + b"\n"


def run_batch(tests: list[Path], mesen_env: dict) -> dict[Path, bytes | None]:
    """Feed a batch through the REPL raw mode. None value => no output seen."""
    stdin = bytearray()
    for t in tests:
        stdin += b"\x01" + t.read_bytes() + b"\x04"
    stdin += b"\x04"  # exit REPL

    log = WORK / "upstream.log"
    lua = HARNESS_IN.read_text()
    lua = (lua.replace("@LOGFILE@", str(log))
              .replace("@MAXFRAMES@", str(FRAMES_PER_TEST * len(tests)))
              .replace("@STDIN@", "".join("\\%d" % b for b in stdin))
              .replace("@JOYSEQ@", ""))
    script = WORK / "upstream_harness.lua"
    script.write_text(lua)
    log.unlink(missing_ok=True)

    subprocess.run([str(MESEN), "--testrunner", str(ROM), str(script)],
                   env=mesen_env, capture_output=True,
                   timeout=60 + 30 * len(tests))
    data = log.read_bytes() if log.exists() else b""
    outs = re.findall(rb"\x02(.*?)\x04", data, re.S)
    result = {}
    for i, t in enumerate(tests):
        result[t] = outs[i] if i < len(outs) else None
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="*",
                    default=[str(ROOT / "micropython/tests/basics")])
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--batch", type=int, default=10)
    args = ap.parse_args()

    tests = []
    for p in args.paths:
        p = Path(p)
        tests += sorted(p.glob("*.py")) if p.is_dir() else [p]
    if args.limit:
        tests = tests[:args.limit]

    EXP_CACHE.mkdir(parents=True, exist_ok=True)
    import os
    mesen_env = dict(os.environ,
                     XDG_CONFIG_HOME=str(WORK / "cfgroot"),
                     DOTNET_SYSTEM_GLOBALIZATION_INVARIANT="1",
                     SDL_VIDEODRIVER="dummy")

    results = {}   # path -> (category, detail)
    queue = []
    for t in tests:
        exp = reference_output(t)
        if exp is None:
            results[t] = ("skip", "reference cannot run it")
        else:
            queue.append((t, exp))

    i = 0
    while i < len(queue):
        chunk = queue[i:i + args.batch]
        got = run_batch([t for t, _ in chunk], mesen_env)
        advanced = 0
        for t, exp in chunk:
            out = got[t]
            if out is None:
                break  # this test hung/crashed the batch
            advanced += 1
            if normalize(out) == normalize(exp):
                results[t] = ("pass", "")
            else:
                results[t] = ("fail", diff_head(normalize(exp), normalize(out)))
        if advanced < len(chunk):
            t, _ = chunk[advanced]
            results[t] = ("timeout", "no output before batch end")
            advanced += 1  # skip the offender, rerun the rest
        i += advanced
        done = sum(1 for r in results.values() if r)
        print(f"  {done}/{len(tests)} done "
              f"(pass {count(results, 'pass')}, fail {count(results, 'fail')}, "
              f"skip {count(results, 'skip')}, timeout {count(results, 'timeout')})",
              flush=True)

    report = ROOT / "build/upstream_results.txt"
    with report.open("w") as f:
        for t in tests:
            cat, detail = results[t]
            f.write(f"{cat:8} {t.name}\n")
            if cat == "fail":
                f.write(indent(detail))
        f.write(f"\nTOTAL {len(tests)}: "
                f"pass {count(results, 'pass')}, fail {count(results, 'fail')}, "
                f"skip {count(results, 'skip')}, timeout {count(results, 'timeout')}\n")
    print(f"report: {report}")


def count(results, cat):
    return sum(1 for c, _ in results.values() if c == cat)


def diff_head(exp: bytes, got: bytes, n=6) -> str:
    import difflib
    e = exp.decode("utf-8", "replace").splitlines()
    g = got.decode("utf-8", "replace").splitlines()
    d = list(difflib.unified_diff(e, g, "expected", "target", lineterm=""))
    return "\n".join(d[:2 + n * 2]) + "\n"


def indent(s: str) -> str:
    return "".join("         | " + l + "\n" for l in s.splitlines())


if __name__ == "__main__":
    main()
