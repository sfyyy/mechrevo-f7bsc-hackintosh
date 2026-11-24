#!/usr/bin/env python3
"""Compare forced AMD and default routing for the fixed Phoenix reference IDR."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import plistlib
import re
import subprocess
import sys
import time

INSTALLED = Path("/Library/GPUBundles/PhoenixVALoadProbe.bundle")
EXE = Path("Contents/MacOS/PhoenixVALoadProbe")
DOMAIN = "com.apple.AppleGVA"
KEY = "gvaForceAMDAVCDecode"
REFERENCE = "a7d7da3e5d6880ac3fa75926aa5e7143722bc8245248b893b3638eae299620da"
ANNEXB = "f0f79288b199930805b07116d95fedd5086cdf937cf6b312cd0dcf424210220c"
MP4 = "46677fe1446ebca8e4c45148bfb40535237e93b55d4f5d0cae5ed1a2b799e884"
SERVICE = "/System/Library/Frameworks/VideoToolbox.framework/Versions/A/XPCServices/VTDecoderXPCService.xpc/Contents/MacOS/VTDecoderXPCService"
SUDO = ["sudo", "-n"]
SWAP = """import ctypes,os,sys
lib=ctypes.CDLL(None,use_errno=True)
rc=lib.renamex_np(os.fsencode(sys.argv[1]),os.fsencode(sys.argv[2]),2)
if rc: raise OSError(ctypes.get_errno(),os.strerror(ctypes.get_errno()))"""

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def backup_digest(path):
    return subprocess.check_output(SUDO + ["/usr/bin/shasum", "-a", "256", path], text=True).split()[0]

def preferences():
    return plistlib.loads(subprocess.check_output(["defaults", "export", DOMAIN, "-"]))

def run_client(out, label, args, expected, raw, state):
    start = datetime.datetime.now().astimezone()
    with (out / (label + ".log")).open("w") as log:
        proc = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
        try:
            rc = proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            try:
                with (out / (label + "-sample.log")).open("w") as sample_log:
                    subprocess.run(["/usr/bin/sample", str(proc.pid), "1", "-file",
                                    out / (label + ".sample")],
                                   stdout=sample_log, stderr=subprocess.STDOUT, timeout=4)
            except subprocess.TimeoutExpired:
                pass
            finally:
                proc.kill()
                proc.wait()
            rc = 124
    end = datetime.datetime.now().astimezone()
    time.sleep(1)
    pid = proc.pid
    pred = 'process == "VTDecoderXPCService" AND (eventMessage CONTAINS "videodecoder.peer[" OR subsystem == "org.nullarc.phoenix.va-load-probe" OR eventMessage BEGINSWITH "Phoenix reference IDR" OR sender == "AppleGVA" OR sender == "VideoToolbox")'
    events = subprocess.check_output(["/usr/bin/log", "show", "--info", "--style", "ndjson", "--start",
            start.strftime("%Y-%m-%d %H:%M:%S"), "--predicate", pred], text=True)
    rows = [json.loads(line) for line in events.splitlines() if line.startswith("{")]
    rows = [e for e in rows if e.get("processImagePath") == SERVICE and e.get("bootUUID") == state["boot_session"]
            and start <= datetime.datetime.strptime(e["timestamp"], "%Y-%m-%d %H:%M:%S.%f%z") <= end]
    owners = {}
    for e in rows:
        peer = re.search(r"com\.apple\.coremedia\.videodecoder\.peer\[(\d+)\]", e.get("eventMessage", ""))
        if peer:
            owners.setdefault(e["processID"], set()).add(int(peer.group(1)))
    peers = {p for p, clients in owners.items() if clients == {pid}}
    linked = [e for e in rows if e.get("processID") in peers]
    (out / (label + "-service.log")).write_text("".join("%s [%d] %s\n" %
        (e.get("timestamp", ""), e["processID"], e.get("eventMessage", "")) for e in linked))
    linked.sort(key=lambda e: e["machTimestamp"])
    connections, factories, hardware, forced_pids = set(), set(), set(), set()
    for e in linked:
        service = e["processID"]
        message = e.get("eventMessage", "")
        if f"com.apple.coremedia.videodecoder.peer[{pid}]" in message:
            connections.add(service)
        if service not in connections:
            continue
        if "force AMD for flow_id 2" in message:
            forced_pids.add(service)
        if e.get("senderImageUUID") != state["candidate_uuid"]:
            continue
        if (e.get("subsystem") == "org.nullarc.phoenix.va-load-probe" and
                f"AVD_CreateAVDAccelerator pid={service} " in message and
                str(INSTALLED / EXE) in message and
                "caller=/System/Library/PrivateFrameworks/AppleGVA.framework/" in message):
            factories.add(service)
        if service in factories and f"Phoenix reference IDR completed registry={state['registry_id']} output_bytes=737280" in message:
            hardware.add(service)
    client = next((json.loads(l) for l in (out / (label + ".log")).read_text().splitlines() if l.startswith("{")), {})
    if raw:
        pixels = raw.read_bytes() if raw.is_file() else b""
        checked = len(pixels)
        match = pixels == expected
        decoded = rc == 0
    else:
        checked = client.get("nv12_checked", 0)
        match = checked == len(expected) and client.get("mismatches") == 0
        # Attribute hardware to the XPC factory and VCN completion even when
        # the optional session property is unavailable to the client.
        decoded = (rc in (0, 1) and client.get("using_hardware") is not False and
                   client.get("pid") == pid and client.get("callbacks") == 1 and
                   all(client.get(k) == 0 for k in ("create_status", "decode_status", "callback_status")))
    valid_route = (factories & hardware & forced_pids) if label.startswith("forced") else ((factories & hardware) - forced_pids)
    result = dict(pid=pid, xpc_peers=sorted(peers), factory=sorted(factories), vcn=sorted(hardware),
                  exit_code=rc, validated_bytes=checked, pixels_match=match, client=client,
                  forced_route=sorted(forced_pids), argv=list(map(str, args)), passed=bool(valid_route and decoded and match))
    (out / (label + ".json")).write_text(json.dumps(result, indent=2) + "\n")
    print(label, json.dumps(result), flush=True)
    return result

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("bundle", "verifier", "ffmpeg", "fixture", "mp4", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--ffmpeg-control", action="store_true", help="Also run FFmpeg while AMD is explicitly selected")
    parser.add_argument("--ffmpeg-threads", type=int, choices=(0, 1), default=0, help="Decoder threads (0 selects FFmpeg's default)")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir()
    reference = args.fixture / "libdrm-output.nv12"
    expected = reference.read_bytes()
    if len(expected) != 622080 or digest(reference) != REFERENCE:
        raise ValueError("Unexpected hardware reference")
    if digest(args.fixture / "libdrm-idr.h264") != ANNEXB or digest(args.mp4) != MP4:
        raise ValueError("Input does not match the verified H.264 fixture")
    services = plistlib.loads(subprocess.check_output(["ioreg", "-a", "-r", "-n", "NMXAccel", "-d", "1"]))
    devices = [s for s in services if s.get("IODVDBundleName") == INSTALLED.stem and
               s.get("PhxVideoDecodeReferenceABI") == 1 and s.get("IOVARendererID") == 0x01020000]
    if len(devices) != 1:
        raise ValueError("Expected a unique Phoenix accelerator with its VA companion")
    registry = devices[0]["IORegistryEntryID"]
    original = digest(INSTALLED / EXE)
    new = digest(args.bundle / EXE)
    before = preferences().get(KEY)
    if before is not None and type(before) is not bool:
        raise ValueError("Unexpected preference type")
    subprocess.run(["codesign", "--verify", "--strict", args.bundle], check=True)
    uuid = subprocess.check_output(["dwarfdump", "--uuid", args.bundle / EXE], text=True).split()[1].upper()
    path = subprocess.check_output(SUDO + ["mktemp", "-d", str(INSTALLED.parent / ".phoenix-test.XXXXXXXX")], text=True).strip()
    copy = Path(path) / INSTALLED.name
    state = dict(original_bundle=original, candidate_bundle=new, original_pref=before, backup_during_test=str(copy),
                 boot_session=subprocess.check_output(["sysctl", "-n", "kern.bootsessionuuid"], text=True).strip(),
                 registry_id=registry, candidate_uuid=uuid, ffmpeg_sha256=digest(args.ffmpeg))
    (out / "state.json").write_text(json.dumps(state, indent=2) + "\n")
    try:
        subprocess.run(SUDO + ["ditto", args.bundle, copy], check=True)
        subprocess.run(SUDO + ["chown", "-R", "root:wheel", copy], check=True)
        subprocess.run(SUDO + ["chmod", "-R", "go-w", copy], check=True)
        subprocess.run(SUDO + ["/usr/bin/python3", "-I", "-c", SWAP, INSTALLED, copy], check=True)
        if digest(INSTALLED / EXE) != new:
            raise RuntimeError("Unexpected installed bundle")
        subprocess.run(["defaults", "write", DOMAIN, KEY, "-bool", "true"], check=True)
        forced = run_client(out, "forced", [args.verifier, "--hardware", args.fixture / "libdrm-idr.h264", reference],
                            expected, None, state)
        ffmpeg_args = [args.ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "verbose"]
        if args.ffmpeg_threads:
            ffmpeg_args += ["-threads", str(args.ffmpeg_threads)]
        ffmpeg_args += [
            "-hwaccel", "videotoolbox", "-hwaccel_output_format", "videotoolbox_vld", "-i", args.mp4,
            "-frames:v", "1", "-vf", "hwdownload,format=nv12", "-pix_fmt", "nv12", "-f", "rawvideo"]
        forced_peers = set(forced["xpc_peers"])
        if args.ffmpeg_control:
            raw = out / "ffmpeg-forced.nv12"
            control = run_client(out, "forced-ffmpeg", ffmpeg_args + [raw], expected, raw, state)
            forced_peers.update(control["xpc_peers"])
        subprocess.run(["defaults", "delete", DOMAIN, KEY], check=True)
        if KEY in preferences():
            raise RuntimeError("Forced preference still present")
        raw = out / "ffmpeg.nv12"
        auto = run_client(out, "default", ffmpeg_args + [raw], expected, raw, state)
        separate_services = not (forced_peers & set(auto["xpc_peers"]))
        state["fresh_default_service"] = separate_services
        return int(not (forced["passed"] and auto["passed"] and separate_services))
    finally:
        try:
            if before is not None:
                subprocess.run(["defaults", "write", DOMAIN, KEY, "-bool", str(before).lower()], check=True)
            elif KEY in preferences():
                subprocess.run(["defaults", "delete", DOMAIN, KEY], check=True)
        finally:
            if digest(INSTALLED / EXE) == new and backup_digest(copy / EXE) == original:
                subprocess.run(SUDO + ["/usr/bin/python3", "-I", "-c", SWAP, INSTALLED, copy], check=True)
            if digest(INSTALLED / EXE) != original:
                raise RuntimeError("Restore failed; original backup recorded in state.json")
            # Keep the original at its exact path and delete only the private staging directory.
            subprocess.run(SUDO + ["/usr/bin/python3", "-I", "-c",
                "import shutil,sys; shutil.rmtree(sys.argv[1])", path], check=True)
            state.update(restored_bundle=digest(INSTALLED / EXE), restored_pref=preferences().get(KEY))
            (out / "state.json").write_text(json.dumps(state, indent=2) + "\n")

if __name__ == "__main__":
    sys.exit(main())
