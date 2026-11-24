#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the selector 121 test client and prepare two software-verified IDR/P fixtures."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def build_client(output):
    tests = Path(__file__).resolve().parent
    avc = tests.parents[1] / "avc"
    program = output / "check-avc-decode"
    with tempfile.TemporaryDirectory() as tmp:
        obj = Path(tmp) / "h264.o"
        subprocess.run(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-c", avc / "avd_h264.c", "-o", obj], check=True)
        subprocess.run(["xcrun", "clang++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                        tests / "check_avc_decode.cpp", avc / "avc_stream_parser.cpp", obj,
                        "-framework", "IOKit", "-framework", "CoreFoundation", "-o", program], check=True)
    return program


def make_case(output, ffmpeg, width, height):
    case = output / f"{width}x{height}"
    case.mkdir()
    stream = case / "stream.h264"
    reference = case / "reference.nv12"
    frames = 14
    params = ("keyint=12:min-keyint=12:scenecut=0:bframes=0:ref=1:weightp=0:"
              "aud=1:repeat-headers=1:slices=4:threads=1")
    common = [ffmpeg, "-v", "error", "-nostdin", "-n"]
    subprocess.run(common + ["-f", "lavfi", "-i", f"testsrc2=size={width}x{height}:rate=30",
                             "-frames:v", str(frames), "-pix_fmt", "yuv420p", "-c:v", "libx264",
                             "-profile:v", "high", "-level:v", "4.0", "-crf", "24",
                             "-x264-params", params, "-f", "h264", stream], check=True)
    # Explicit software decoder and hwaccel setting keep the reference independent of VideoToolbox.
    subprocess.run(common + ["-hwaccel", "none", "-threads", "1", "-c:v", "h264", "-f", "h264",
                             "-i", stream, "-pix_fmt", "nv12", "-f", "rawvideo", reference], check=True)
    data = stream.read_bytes()
    # libx264 emits one four-byte start code and an AUD NAL at the start of each AU.
    offsets = [m.start() for m in re.finditer(b"\x00\x00\x00\x01\x09", data)] + [len(data)]
    if offsets[0] != 0 or len(offsets) != frames + 1:
        raise RuntimeError(f"{stream}: expected {frames} complete AUD-delimited access units")
    visible = width * height * 3 // 2
    if reference.stat().st_size != frames * visible:
        raise RuntimeError(f"{reference}: unexpected decoded length")
    manifest = []
    with reference.open("rb") as raw:
        for i in range(frames):
            au = data[offsets[i]:offsets[i + 1]]
            filename = f"frame-{i:04d}.au"
            (case / filename).write_bytes(au)
            manifest.append({"au": filename, "au_bytes": len(au),
                             "au_sha256": hashlib.sha256(au).hexdigest(), "nv12_bytes": visible,
                             "nv12_sha256": hashlib.sha256(raw.read(visible)).hexdigest()})
    if len({row["nv12_sha256"] for row in manifest}) != frames:
        raise RuntimeError(f"{case}: generated frames must have distinct visible pixels")
    (case / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return case


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("output", type=Path, help="new or empty output directory")
    cli.add_argument("--ffmpeg", default="ffmpeg", help="An FFmpeg binary with libx264 and testsrc2")
    args = cli.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        cli.error("output directory contains files")
    program = build_client(output)
    cases = [make_case(output, args.ffmpeg, w, h) for w, h in [(864, 480), (1920, 1080)]]
    inspection = subprocess.run([program, "--inspect", *cases], check=True, stdout=subprocess.PIPE, text=True)
    (output / "inspect.log").write_text(inspection.stdout)
    print(inspection.stdout, end="")
    print("Hardware verification command")
    print(shlex.join(["sudo", str(program), "--hardware", *map(str, cases)]))


if __name__ == "__main__":
    main()
