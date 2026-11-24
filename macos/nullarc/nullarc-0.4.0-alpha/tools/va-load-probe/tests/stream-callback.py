#!/usr/bin/env python3
"""Check the production AVD command conversion for the prepared IDR/P fixtures."""
import ctypes as C
import json
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
SRC = HERE.parent
TOOLS = SRC.parent
NULLARC = SRC.parents[1]
AVC = TOOLS / "avc"
helpers = runpy.run_path(str(HERE / "avc-parameters.py"))
START = helpers["START"]


def check_fixture(directory, parser, check):
    width, height = map(int, directory.name.split("x"))
    frames = json.loads((directory / "manifest.json").read_text())
    blocks = None
    parameters = None
    for frame in frames:
        nals = [n for n in helpers["SPLIT"].split((directory / frame["au"]).read_bytes()) if n]
        types = [n[0] & 31 for n in nals]
        if 5 in types:
            sets = [nals[types.index(kind)] for kind in (7, 8)]
            blocks = parser.blocks(*sets)
            parameters = b"".join(START + n for n in sets)
        slices = [START + n for n in nals if n[0] & 31 in (1, 5)]
        assert blocks and parameters and slices
        expected = parameters + b"".join(slices)
        lengths = (C.c_uint32 * len(slices))(*map(len, slices))
        if check(*blocks, expected, len(expected), lengths, len(slices), len(parameters), width, height):
            raise ValueError(f"{directory.name}/{frame['au']} disagrees with the expected access unit")
    print(f"{directory.name}: {len(frames)} AVD callbacks matched complete Annex B access units; invalid slice bounds rejected")


def main():
    if len(sys.argv) < 2:
        sys.exit("Usage: stream-callback.py fixture-directory [fixture-directory ...]")
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "callbacks.dylib"
        subprocess.run(["xcrun", "clang", "-std=c11", "-arch", "x86_64", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-dynamiclib", "-I", SRC, "-I", AVC, "-I", NULLARC / "src/phxaccel",
                        "-I", TOOLS / "vcn",
                        HERE / "stream-callback.c", SRC / "avd_decode.c", SRC / "avc_parameters.c", AVC / "avd_h264.c",
                        "-framework", "CoreVideo", "-framework", "CoreFoundation", "-framework", "IOSurface",
                        "-o", path], check=True)
        lib = C.CDLL(str(path))
        check = lib.checkCallback
        check.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_uint32),
                          C.c_uint, C.c_uint32, C.c_int32, C.c_int32]
        parser = helpers["AppleParser"](lib)
        for directory in sys.argv[1:]:
            check_fixture(Path(directory), parser, check)


if __name__ == "__main__":
    main()
