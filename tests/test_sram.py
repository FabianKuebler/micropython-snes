"""M9.1: the battery-SRAM file store works and persists.

build/sramtest.sfc (C only, no MicroPython) formats fresh SRAM, writes a
file that spans the 8KB HiROM SRAM window boundary, verifies checksums
through delete/compaction and the error paths, and leaves two files
behind. A second run of the same ROM against the same .srm (Mesen
persists it under Mesen2/Saves/) must mount instead of format and read
the survivor back intact.
"""

from conftest import ROOT, STATUS_PASS, build, clear_srm, run_rom


def test_fs_roundtrip_and_persistence(mesen_env, tmp_path):
    build("sramtest")
    clear_srm(mesen_env)

    code, text = run_rom(ROOT / "build" / "sramtest.sfc", mesen_env, tmp_path,
                         max_frames=3600)
    assert code == STATUS_PASS, f"exit {code}, log:\n{text}"
    assert "sram: formatted" in text
    assert "sram: b.bin len 200 sum 29924" in text
    assert "sram: b readback len 200 sum 29924" in text
    assert "sram: after delete b len 200 sum 29924" in text
    assert text.rstrip().endswith("sram: done")

    # second run, same .srm: mounts, does not format, data intact
    code, text = run_rom(ROOT / "build" / "sramtest.sfc", mesen_env, tmp_path,
                         max_frames=3600)
    assert code == STATUS_PASS, f"exit {code}, log:\n{text}"
    assert "sram: mounted" in text
    assert "sram: files 2" in text
    assert "sram: persisted b len 200 sum 29924" in text
