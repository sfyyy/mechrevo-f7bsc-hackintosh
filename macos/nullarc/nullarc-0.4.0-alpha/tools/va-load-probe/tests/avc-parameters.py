#!/usr/bin/env python3
"""Round-trip known local streams through the x86_64 AppleGVA parameter parser."""
import argparse
import ctypes as C
import filecmp
from pathlib import Path
import re
import struct
import subprocess
import tempfile

GVA = "/System/Library/PrivateFrameworks/AppleGVA.framework/Versions/A/AppleGVA"
START = b"\0\0\0\1"
SPLIT = re.compile(rb"\x00\x00(?:\x00)?\x01")
REFERENCE_SPS = bytes.fromhex("6764001eac5230360f64")
REFERENCE_PPS = bytes.fromhex("68de09c8")


class AppleParser:
    def __init__(self, export_library):
        self.gva = C.CDLL(GVA)
        assert export_library.PhoenixAVDCompatibleAppleGVA()  # Check the UUID before using offsets.
        dyld = C.CDLL(None)
        dyld._dyld_get_image_name.restype = C.c_char_p
        dyld._dyld_get_image_header.restype = C.c_void_p
        i = next(i for i in range(dyld._dyld_image_count()) if dyld._dyld_get_image_name(i) == GVA.encode())
        base = dyld._dyld_get_image_header(i)
        self.prepare = C.CFUNCTYPE(None, C.c_void_p, C.c_void_p, C.c_size_t)(base + 0x7ed8)
        parse = C.CFUNCTYPE(C.c_void_p, C.c_void_p, C.c_void_p)
        self.sps = parse(base + 0x8d04)
        self.pps = parse(base + 0x9884)

    def blocks(self, sequence, picture):
        reader = C.create_string_buffer(0xa00)
        # Keep both parameter-set arrays alive while the PPS parser follows the SPS pointer.
        buffers = [C.create_string_buffer(32 * 0xc18), C.create_string_buffer(256 * 0x488)]
        result = []
        for nal, parse, size, buf in zip((sequence, picture), (self.sps, self.pps), (0xc18, 0x488), buffers):
            assert 1 < len(nal) <= 512
            payload = C.create_string_buffer(nal[1:] + bytes(64))
            self.prepare(reader, payload, len(nal) - 1)
            # Equivalent to the end-of-RBSP accounting in AppleGVA +0x7ffa,
            # for one already-framed NAL with its single-byte header removed.
            remaining = (len(nal) - 1) * 8 - struct.unpack_from("<I", reader, 0x38)[0]
            window = struct.unpack_from("<Q", reader, 0x18)[0]
            struct.pack_into("<Q", reader, 0x20, window + ((remaining + 31) // 32) * 4)
            struct.pack_into("<I", reader, 0x3c, (-remaining) % 32)
            block = parse(reader, buf)
            assert block and C.addressof(buf) <= block <= C.addressof(buf) + len(buf) - size
            result.append(C.create_string_buffer(C.string_at(block, size)))
        return result


def roundtrip(args, tmp, name, src, parser, export, frames, width, height, rewritten_sps=True):
    # rewritten_sps=False is the HRD case: the exported VUI drops the HRD tables,
    # so the bytes differ from the source SPS while the decoded pixels must not.
    nals = [n for n in SPLIT.split(src.read_bytes()) if n]
    sps, pps = (next(n for n in nals if n[0] & 31 == typ) for typ in (7, 8))
    if name == "reference":
        assert (sps, pps) == (REFERENCE_SPS, REFERENCE_PPS)
    sequence, picture = parser.blocks(sps, pps)
    out = C.create_string_buffer(512)
    written = C.c_size_t()
    assert export(sequence, picture, out, 512, C.byref(written))
    encoded = out.raw[:written.value]
    if rewritten_sps:
        assert encoded == START + sps + START + pps
    else:
        assert encoded.startswith(START + sps[:4]) and START + pps in encoded
    rewritten = tmp / (name + "-rewritten.h264")
    rewritten.write_bytes(encoded + b"".join(START + n for n in nals if n[0] & 31 not in (7, 8)))
    images = []
    for label, stream in (("original", src), ("exported", rewritten)):
        image = tmp / (name + "-" + label + ".nv12")
        subprocess.run([args.ffmpeg, "-v", "error", "-nostdin", "-hwaccel", "none", "-threads", "1",
                        "-f", "h264", "-i", stream, "-f", "rawvideo", "-pix_fmt", "nv12", image], check=True)
        images.append(image)
    assert images[1].stat().st_size == frames * width * height * 3 // 2 and filecmp.cmp(*images, shallow=False)
    for block, offset in ((sequence, 0x40), (picture, 0xbc)):
        original = block.raw
        block[offset] = 1
        assert not export(sequence, picture, out, 512, C.byref(written))
        assert not written.value and out.raw[:len(encoded)] == encoded
        C.memmove(block, original, len(original))
    assert not export(sequence, picture, out, len(encoded) - 1, C.byref(written))
    assert not written.value and out.raw[:len(encoded)] == encoded
    print(f"{name}: both NALs match, {frames} frames and {images[1].stat().st_size} NV12 bytes match")


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("ffmpeg", help="FFmpeg with libx264 and lavfi")
    cli.add_argument("reference", type=Path, help="libdrm-idr.h264")
    args = cli.parse_args()
    with tempfile.TemporaryDirectory() as folder:
        tmp = Path(folder)
        dylib = tmp / "parameters.dylib"
        src = Path(__file__).resolve().parents[1] / "avc_parameters.c"
        subprocess.run(["xcrun", "clang", "-arch", "x86_64", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-dynamiclib", src, "-o", dylib], check=True)
        library = C.CDLL(str(dylib))
        library.PhoenixAVDCompatibleAppleGVA.restype = C.c_bool
        export = library.PhoenixAVDExportAVCParameters
        export.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)]
        export.restype = C.c_bool
        parser = AppleParser(library)
        roundtrip(args, tmp, "reference", args.reference, parser, export, 1, 864, 480)
        for profile in ("baseline", "main", "high"):
            for width, height in ((864, 480), (1920, 1080)):
                name = f"{profile}-{width}x{height}"
                stream = tmp / (name + ".h264")
                params = "ref=1:bframes=0:weightp=0:aud=1:slices=4:keyint=12:min-keyint=12:scenecut=0"
                if profile == "main":
                    params += ":sps-id=3"
                if profile == "baseline":
                    params += ":colorprim=bt709:transfer=bt709:colormatrix=bt709"
                if profile == "high":
                    params += ":overscan=show:sar=17/15"
                subprocess.run([args.ffmpeg, "-v", "error", "-nostdin", "-f", "lavfi", "-i",
                                f"testsrc2=size={width}x{height}:rate=30", "-frames:v", "3", "-c:v", "libx264",
                                "-profile:v", profile, "-level", "4.0", "-threads", "1",
                                "-x264-params", params, "-f", "h264", stream], check=True)
                roundtrip(args, tmp, name, stream, parser, export, 3, width, height)
        # CBR live encoders signal NAL HRD; the exporter must drop it and still
        # produce a parameter set whose decoded pixels are identical.
        hrd = tmp / "high-hrd-1920x1080.h264"
        subprocess.run([args.ffmpeg, "-v", "error", "-nostdin", "-f", "lavfi", "-i",
                        "testsrc2=size=1920x1080:rate=30", "-frames:v", "3", "-c:v", "libx264",
                        "-profile:v", "high", "-level", "4.0", "-threads", "1",
                        "-b:v", "4M", "-maxrate", "4M", "-bufsize", "8M",
                        "-x264-params", "nal-hrd=cbr:bframes=2:ref=4:keyint=12:min-keyint=12:scenecut=0",
                        "-f", "h264", hrd], check=True)
        roundtrip(args, tmp, "high-hrd-1920x1080", hrd, parser, export, 3, 1920, 1080, rewritten_sps=False)


if __name__ == "__main__":
    main()
