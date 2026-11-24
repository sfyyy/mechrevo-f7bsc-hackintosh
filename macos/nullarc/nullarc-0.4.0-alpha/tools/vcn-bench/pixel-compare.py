#!/usr/bin/env python3
"""Per-frame pixel comparison: hardware decode versus software decode.

The hardware path is VideoToolbox with hwdownload, exactly what the player does;
the reference is libavcodec's software H.264 decoder. Both write framemd5, so a
mismatch names the frame without storing 1.7 GB of NV12.

usage: pixel-compare.py CLIP [CLIP...]
Exit code 0 only when every frame of every clip matches.
"""
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE
FFMPEG = ROOT / "results/vt-stock-ffmpeg/ffmpeg"


def framemd5(clip, hardware, out):
    args = [str(FFMPEG), "-nostdin", "-hide_banner", "-loglevel", "error", "-threads", "1"]
    if hardware:
        args += ["-hwaccel", "videotoolbox", "-hwaccel_output_format", "videotoolbox_vld"]
    else:
        args += ["-hwaccel", "none"]
    args += ["-f", "h264", "-i", str(clip), "-map", "0:v:0",
             "-vf", "hwdownload,format=nv12" if hardware else "format=nv12",
             "-fps_mode", "passthrough", "-c:v", "rawvideo", "-f", "framemd5", str(out)]
    return subprocess.run(args, capture_output=True, text=True, timeout=600)


def lines(path):
    return [line for line in path.read_text().splitlines() if not line.startswith("#")]


def main():
    clips = [pathlib.Path(a) for a in sys.argv[1:]]
    if not clips:
        print(__doc__, file=sys.stderr)
        return 2
    failures = 0
    for clip in clips:
        with tempfile.TemporaryDirectory() as tmp:
            hw, sw = pathlib.Path(tmp) / "hw.md5", pathlib.Path(tmp) / "sw.md5"
            hardware = framemd5(clip, True, hw)
            software = framemd5(clip, False, sw)
            if hardware.returncode or software.returncode or not hw.exists() or not sw.exists():
                print(f"{clip.name}: decoder failed (hw={hardware.returncode} sw={software.returncode})")
                print((hardware.stderr or software.stderr)[-300:])
                failures += 1
                continue
            a, b = lines(hw), lines(sw)
            mismatched = [i for i, (x, y) in enumerate(zip(a, b)) if x != y]
            if len(a) != len(b):
                print(f"{clip.name}: frame count differs hw={len(a)} sw={len(b)}")
                failures += 1
            if mismatched:
                print(f"{clip.name}: {len(mismatched)} of {len(a)} frames differ, first at {mismatched[0]}")
                failures += 1
            else:
                print(f"{clip.name}: {len(a)} frames identical")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
