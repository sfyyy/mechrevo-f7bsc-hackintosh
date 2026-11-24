#!/usr/bin/env python3
"""Model the ordinary-board H.264 renderer filter from an AppleGVA text image."""

import argparse
import json
from pathlib import Path
import plistlib
import struct
import subprocess
import uuid


APPLE_GVA_UUID = uuid.UUID("ca5414f3-6937-3c05-9ea2-d82b35afd4f0")
APPLE_GVA_ADDRESS = 0x7FFA0C578000
APPLE_GVA_TEXT_LENGTH = 0xEAD27
DYLD_CACHE = Path("/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64h")


def read_current_image():
    subcaches = sorted(DYLD_CACHE.parent.glob(DYLD_CACHE.name + ".[0-9][0-9]"))
    for path in [DYLD_CACHE, *subcaches]:
        with path.open("rb") as cache:
            cache.seek(16)
            mapping_offset, mapping_count = struct.unpack("<II", cache.read(8))
            cache.seek(mapping_offset)
            for _ in range(mapping_count):
                address, size, offset, _, _ = struct.unpack("<QQQII", cache.read(32))
                if address <= APPLE_GVA_ADDRESS < address + size:
                    image_offset = APPLE_GVA_ADDRESS - address
                    if APPLE_GVA_TEXT_LENGTH > size - image_offset:
                        raise ValueError("AppleGVA text crosses a cache mapping boundary")
                    cache.seek(offset + image_offset)
                    return cache.read(APPLE_GVA_TEXT_LENGTH)
    raise ValueError("AppleGVA reference address is absent from the dyld cache")


def image_uuid(image):
    magic, cpu, _, _, command_count = struct.unpack_from("<5I", image)
    if (magic, cpu) != (0xFEEDFACF, 0x01000007):
        raise ValueError("Expected an x86_64 Mach-O image")
    offset = 32
    for _ in range(command_count):
        command, size = struct.unpack_from("<2I", image, offset)
        if size < 8 or offset + size > len(image):
            raise ValueError("Invalid Mach-O load command")
        if command == 0x1B and size == 24:
            return uuid.UUID(bytes=image[offset + 8:offset + size])
        offset += size
    raise ValueError("Mach-O image has no UUID")


def immediate(image, offset, opcode, size, signed=False):
    stop = offset + len(opcode)
    if image[offset:stop] != opcode or stop + size > len(image):
        raise ValueError(f"Unexpected AppleGVA instruction at +{offset:#x}")
    return int.from_bytes(image[stop:stop + size], "little", signed=signed)


def renderer_ids(base, mask):
    return {base + bit for bit in range(mask.bit_length()) if mask & (1 << bit)}


def read_policy(image):
    if image_uuid(image) != APPLE_GVA_UUID:
        raise ValueError("AppleGVA UUID differs from this probe's reference image")
    # The ordinary-board branch appends the alternate slot at %rbp-0x150.
    branch = bytes.fromhex("4d85f60f84ac010000488db5b0feffff4889dfe898120000")
    if image[0x23E30:0x23E30 + len(branch)] != branch:
        raise ValueError("Ordinary-board routing branch differs")

    amd_base = -immediate(image, 0x23CE7, b"\x8d\xbe", 4, signed=True)
    amd_mask = immediate(image, 0x23C9C, b"\xb9", 4)
    alternate_base = -immediate(image, 0x23CCB, b"\x8d\xbe", 4, signed=True)
    alternate_mask = immediate(image, 0x23CA1, b"\xba", 4)
    alternate_extra = immediate(image, 0x23D07, b"\x81\xfe", 4)
    factory_mask = immediate(image, 0x2413F, b"\x48\xbf", 8)
    factory_extra = immediate(image, 0x2414F, b"\x81\xfa", 4)
    factory_base = -immediate(image, 0x24134, b"\x8d\xb2", 4, signed=True)
    special_boards = set()
    for offset in (0x23DEB, 0x23E02, 0x23E19):
        start = offset + 7 + immediate(image, offset, b"\x48\x8d\x35", 4, signed=True)
        special_boards.add(image[start:image.index(b"\0", start)].decode("ascii"))
    amd = renderer_ids(amd_base, amd_mask)
    alternate = renderer_ids(alternate_base, alternate_mask) | {alternate_extra}
    factory = renderer_ids(factory_base, factory_mask) | {factory_extra}
    return amd, alternate, factory, special_boards


def attached_devices():
    output = subprocess.check_output(["ioreg", "-a", "-r", "-d", "1", "-c", "IOPlatformExpertDevice"])
    platform, = plistlib.loads(output)
    board = platform["board-id"].rstrip(b"\0").decode("ascii")
    output = subprocess.check_output(["ioreg", "-a", "-r", "-d", "1", "-n", "NMXAccel"])
    accelerators = plistlib.loads(output)
    renderers = {item["IOVARendererID"] for item in accelerators
                 if item.get("IODVDBundleName") == "PhoenixVALoadProbe" and type(item.get("IOVARendererID")) is int}
    return board, renderers


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", nargs="?", type=Path, help="AppleGVA __TEXT image instead of the local dyld cache")
    args = parser.parse_args()
    image = args.image.read_bytes() if args.image else read_current_image()
    amd, alternate, factory, special_boards = read_policy(image)
    board, published = attached_devices()
    if board in special_boards:
        raise SystemExit("This board takes a different AppleGVA routing branch")

    # The filter assumes that the discovered renderer has passed eligibility.
    report = {
        "applegva_uuid": str(APPLE_GVA_UUID),
        "board_id": board,
        "phoenix_published_renderer_ids": [hex(value) for value in sorted(published)],
        "phoenix_ids_in_default_slot": [hex(value) for value in sorted(published & alternate)],
        "phoenix_ids_in_forced_amd_slot": [hex(value) for value in sorted(published & amd)],
        "alternate_ids_reaching_factory_dispatch": [hex(value) for value in sorted(alternate & factory)],
        "scope": "Static filter for eligible renderers. Runtime plugin and decoder admission are separate.",
    }
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
