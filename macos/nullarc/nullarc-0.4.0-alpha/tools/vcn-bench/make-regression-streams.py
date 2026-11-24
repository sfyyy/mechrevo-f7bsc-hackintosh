#!/usr/bin/env python3
"""Builds the two regression streams the acceptance list asks for.

    E geometry-switch.h264  960x540 followed by 1920x1080: the decoder must drop
                            the session instead of reusing it across the change
    F corrupt-slice.h264    the 1080p stream with one slice overwritten: the next
                            IDR must rebuild the session

usage: make-regression-streams.py <output-dir>
"""
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
RES = HERE.parent / "results"
SHORT = RES / "vcn-build73-chrome/douyu-540.h264"
LONG = RES / "results-1080p-placeholder"


def nal_offsets(data):
    """(start, type) of every Annex B NAL, start codes included."""
    nals = []
    i = 0
    while i + 4 < len(data):
        if data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 1:
            start = i - 1 if i and data[i - 1] == 0 else i
            nals.append((start, data[i + 3] & 31))
            i += 3
        else:
            i += 1
    return nals


def main():
    out = pathlib.Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    clip = RES / "build75-playback-audit.mUaoWr/gop60.h264"
    long_data = clip.read_bytes()
    short_data = SHORT.read_bytes()

    switch = out / "geometry-switch.h264"
    switch.write_bytes(short_data + long_data)
    print(f"{switch}: {len(short_data)} bytes 960x540 + {len(long_data)} bytes 1080p")

    corrupt = bytearray(long_data)
    nals = nal_offsets(corrupt)
    vcl = [(start, nals[k + 1][0]) for k, (start, kind) in enumerate(nals[:-1]) if kind in (1, 5)]
    target = vcl[len(vcl) // 2]                     # a P slice halfway through the stream
    start, end = target
    payload = start + 4
    # The slice header, not the middle of the payload: the parser has to reject
    # the access unit (or the firmware has to report an error) for the next IDR
    # to be forced into a fresh session.
    for i in range(payload, min(payload + 16, end)):
        corrupt[i] = 0xFF
    broken = out / "corrupt-slice.h264"
    broken.write_bytes(bytes(corrupt))
    print(f"{broken}: corrupted {min(16, end - payload)} header bytes of the slice at offset {start}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
