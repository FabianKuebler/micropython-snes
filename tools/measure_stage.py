#!/usr/bin/env python3
"""Measure the Stage demo's real frame rate in Mesen.

Runs build/mpystage.sfc under a harness that stamps every mailbox stdout
line with the emulated frame number (NTSC: 60.1 frames/sec of real console
time) and screenshots the final frame. FPS = game frames elapsed between
the demo's progress prints, divided by emulated time.

Usage: measure_stage.py [output.png]
"""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MESEN = os.environ.get("MESEN_PATH", os.path.expanduser("~/bin/Mesen"))

LUA = """
local MB_MAGIC, MB_STATUS, MB_WINDEX, MB_RING = 0x1E000, 0x1E002, 0x1E004, 0x1E010
local RING_SIZE = 7664
local frames = 0
local readIndex = 0
local line = ""
local logf = io.open("@LOG@", "wb")
local shot = false

local function word(addr)
  return emu.readWord(addr, emu.memType.snesWorkRam, false)
end

local function drain()
  local wi = word(MB_WINDEX)
  while readIndex ~= wi do
    local b = emu.read(MB_RING + (readIndex % RING_SIZE), emu.memType.snesWorkRam)
    local c = string.char(b)
    if c == "\\n" then
      logf:write(string.format("%d\\t%s\\n", frames, line))
      logf:flush()
      line = ""
    else
      line = line .. c
    end
    readIndex = (readIndex + 1) % 65536
  end
end

local sawMagic = false

local function onFrame()
  frames = frames + 1
  if not sawMagic then
    if word(MB_MAGIC) == 0xCAFE then
      sawMagic = true
    elseif frames > 600 then
      logf:close()
      emu.stop(98)
      return
    else
      return
    end
  end
  drain()
  local status = word(MB_STATUS)
  if status ~= 0 then
    if not shot then
      shot = true
      local png = emu.takeScreenshot()
      local f = io.open("@SHOT@", "wb")
      f:write(png)
      f:close()
    end
    logf:close()
    emu.stop(status)
    return
  end
  if frames >= 40000 then
    logf:close()
    emu.stop(99)
  end
end

emu.addEventCallback(onFrame, emu.eventType.endFrame)
"""


def main():
    shot = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "docs" / "img" / "stage_snes.png"
    work = Path(tempfile.mkdtemp(prefix="stagemeasure"))
    log = work / "timed.log"
    lua = work / "measure.lua"
    lua.write_text(LUA.replace("@LOG@", str(log)).replace("@SHOT@", str(shot)))

    env = dict(os.environ,
               XDG_CONFIG_HOME="/tmp/mbx/cfgroot",
               DOTNET_SYSTEM_GLOBALIZATION_INVARIANT="1",
               SDL_VIDEODRIVER="dummy")
    r = subprocess.run([MESEN, "--testrunner", str(ROOT / "build/mpystage.sfc"),
                        str(lua)], env=env, capture_output=True, timeout=1200)
    print(f"mesen exit: {r.returncode}")
    if not log.exists():
        sys.exit("no log produced")

    marks = {}
    for row in log.read_text(errors="replace").splitlines():
        emuframe, _, text = row.partition("\t")
        print(f"  [{int(emuframe):6d}] {text}")
        m = re.fullmatch(r"stage: frame (\d+)", text)
        if m:
            marks[int(m.group(1))] = int(emuframe)
        elif text == "stage: done":
            marks["done"] = int(emuframe)
        elif text == "stage: init ok":
            marks["init"] = int(emuframe)

    if 0 in marks and "done" in marks:
        game_frames = 119  # prints after iteration 0 and after iteration 119
        emu_frames = marks["done"] - marks[0]
        fps = game_frames * 60.0988 / emu_frames
        print(f"\n{game_frames} game frames in {emu_frames} emulated frames "
              f"({emu_frames / 60.0988:.1f}s of console time): {fps:.2f} fps")
    if "init" in marks:
        print(f"boot to first frame: {marks['init'] / 60.0988:.1f}s")
    if shot.exists():
        print(f"screenshot: {shot}")


if __name__ == "__main__":
    main()
