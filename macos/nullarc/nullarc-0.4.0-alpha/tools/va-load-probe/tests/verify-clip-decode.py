#!/usr/bin/env python3
"""Compare a short H.264 clip in presentation order and verify the Phoenix XPC route."""
import argparse
from datetime import datetime
import json
from pathlib import Path
import re
import runpy
import subprocess
import time

verify = runpy.run_path(str(Path(__file__).with_name("verify-stream-decode.py")))


def decode(ffmpeg, clip, folder, hardware):
    mode = "phoenix" if hardware else "software"
    cmd = [ffmpeg, "-nostdin", "-n", "-hide_banner", "-loglevel", "verbose", "-threads", "1"]
    if hardware:
        cmd += ["-hwaccel", "videotoolbox", "-hwaccel_output_format", "videotoolbox_vld"]
    else:
        cmd += ["-hwaccel", "none"]
    cmd += ["-c:v", "h264", "-i", clip, "-map", "0:v:0"]
    if hardware:
        cmd += ["-vf", "hwdownload,format=nv12"]
    hashes = folder / (mode + ".framehash")
    cmd += ["-fps_mode", "passthrough", "-threads:v", "1", "-c:v", "rawvideo",
            "-pix_fmt", "nv12", "-f", "framehash", hashes]
    with (folder / (mode + ".log")).open("w") as log:
        with subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT) as process:
            try:
                status = process.wait(timeout=60)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise
    rows = hashes.read_text().splitlines() if hashes.exists() else []
    dimension = next((line.split()[-1] for line in rows if line.startswith("#dimensions 0:")), None)
    frames = [[value.strip() for value in line.split(",")] for line in rows if line and not line.startswith("#")]
    if any(len(row) != 6 or not re.fullmatch("[0-9a-f]{64}", row[-1]) for row in frames):
        raise ValueError(f"Unexpected framehash record in {hashes}")
    return process.pid, status, dimension, frames


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("clip", type=Path)
    cli.add_argument("--ffmpeg", type=Path, required=True)
    cli.add_argument("--output", type=Path, required=True)
    args = cli.parse_args()
    if verify["sha"](args.ffmpeg.read_bytes()) != verify["STOCK"]:
        cli.error("Expected the stock FFmpeg used for hardware validation")
    args.output.mkdir()
    _, sw, dimension, reference = decode(args.ffmpeg, args.clip, args.output, False)
    if sw or not dimension or not reference:
        raise RuntimeError("Software decoder did not produce the reference")
    state = verify["live_state"](verify["BUNDLE"])
    previous = verify["existing_services"]()
    start = datetime.now().astimezone()
    pid, hw, hw_dimension, actual = decode(args.ffmpeg, args.clip, args.output, True)
    time.sleep(1)
    end = datetime.now().astimezone()
    time.sleep(1)
    events = verify["collect_events"](start, end, args.output)
    xpc = verify["check_events"](events, pid, "default", state, start, end, previous, dimension, len(reference))
    different = next((i for i, (a,b) in enumerate(zip(reference,actual)) if a != b), None)
    if different is None and len(actual) != len(reference):
        different = min(len(actual),len(reference))
    passed = (not hw and dimension == hw_dimension and different is None and xpc["passed"] and
              verify["check_route"]("default", state["preferences"]) and state == verify["live_state"](verify["BUNDLE"]))
    report = dict(clip=str(args.clip.resolve()), clip_sha256=verify["sha"](args.clip.read_bytes()),
                  ffmpeg_pid=pid, dimensions=dimension, software_frames=len(reference), hardware_frames=len(actual),
                  first_mismatch=different, hardware_exit=hw, xpc=xpc, state=state, passed=passed)
    print(json.dumps(report, indent=2))
    (args.output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
