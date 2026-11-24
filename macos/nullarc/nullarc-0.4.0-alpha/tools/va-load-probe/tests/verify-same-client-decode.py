#!/usr/bin/env python3
"""Verify two hardware VT sessions in one client. --check-inputs only reads compressed samples."""
import argparse
from pathlib import Path
import runpy
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--check-inputs", action="store_true")
    modes.add_argument("--hardware", action="store_true")
    parser.add_argument("fixtures", type=Path)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    check_fixture = runpy.run_path(str(tests / "verify-stream-decode.py"))["check_fixture"]
    with tempfile.TemporaryDirectory(prefix="phoenix-same-client-") as folder:
        tmp = Path(folder)
        binary = tmp / "same-client-decode"
        subprocess.run(["xcrun", "swiftc", "-O", "-warnings-as-errors", "-parse-as-library",
                        tests / "same-client-decode.swift", "-o", binary], check=True, timeout=120)
        command = [binary, "--hardware" if args.hardware else "--check-inputs"]
        for dimensions in ("1920x1080", "864x480"):
            case = args.fixtures / dimensions
            check_fixture(case)
            mp4 = tmp / (dimensions + ".mp4")
            subprocess.run([args.ffmpeg, "-nostdin", "-v", "error", "-f", "h264", "-i", case / "stream.h264",
                            "-map", "0:v:0", "-c:v", "copy", mp4], check=True, timeout=30)
            command.extend([mp4, case / "reference.nv12"])
        subprocess.run(command, check=True, timeout=60)


if __name__ == "__main__":
    main()
