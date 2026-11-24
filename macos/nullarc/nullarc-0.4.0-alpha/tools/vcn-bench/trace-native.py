#!/usr/bin/env python3
"""Trace the kernel's side of VideoToolbox hardware decode for the native path.

Runs fence-sleep.d (which filters on the VTDecoder XPC service, so it does not
care who the client is) while vt-metal decodes a clip. Writes one trace log and
one summary per clip; see fence_summary.summarize for the groups.

usage: trace-native.py --out DIR --vt-metal PATH CLIP[:NAME] [CLIP[:NAME] ...]
"""
import argparse
import json
import pathlib
import signal
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from fence_summary import load_frames, summarize  # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE.parent / "results/build75-playback-audit.mUaoWr/fence-sleep.d"


def run(clip, name, out, binary, loops):
    logfile = out / f"native-{name}-fence-trace.log"
    client_log = out / f"native-{name}-metal.log"
    trace = subprocess.Popen(["sudo", "-n", "/usr/sbin/dtrace", "-s", str(SCRIPT)],
                             stdout=logfile.open("w"), stderr=subprocess.STDOUT)
    try:
        for _ in range(60):
            if "ready" in logfile.read_text():
                break
            if trace.poll() is not None:
                raise RuntimeError(logfile.read_text())
            time.sleep(0.1)
        else:
            raise RuntimeError("DTrace did not signal readiness")
        command = [binary, "--hardware", str(clip)]
        if loops > 1:
            command.append(str(loops))
        with client_log.open("w") as out_log:
            subprocess.run(command, stdout=out_log, stderr=out_log, timeout=120, check=True)
    finally:
        child = subprocess.run(["pgrep", "-P", str(trace.pid), "-x", "dtrace"],
                               capture_output=True, text=True)
        if child.returncode == 0:
            subprocess.run(["sudo", "-n", "/bin/kill", "-INT", *child.stdout.split()], check=True)
        elif trace.poll() is None:
            trace.send_signal(signal.SIGINT)
        trace.wait(timeout=15)
    frames = load_frames(logfile)
    summary = summarize(frames)
    (out / f"native-{name}-fence-summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"{name}: {len(frames)} frames, all.ioctl p50={summary.get('all', {}).get('ioctl_ms', {}).get('p50')} ms, "
          f"gap p50={summary.get('all', {}).get('gap_ms', {}).get('p50')} ms, "
          f"ioctl wall share={summary.get('all', {}).get('ioctl_share_of_wall')}%", flush=True)
    return summary


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    parser.add_argument("--vt-metal", required=True)
    parser.add_argument("--loops", type=int, default=1)
    parser.add_argument("clips", nargs="+")
    arguments = parser.parse_args()
    out = pathlib.Path(arguments.out)
    out.mkdir(parents=True, exist_ok=True)
    for spec in arguments.clips:
        clip, _, name = spec.partition(":")
        run(pathlib.Path(clip), name or pathlib.Path(clip).stem, out,
            arguments.vt_metal, arguments.loops)


if __name__ == "__main__":
    main()
