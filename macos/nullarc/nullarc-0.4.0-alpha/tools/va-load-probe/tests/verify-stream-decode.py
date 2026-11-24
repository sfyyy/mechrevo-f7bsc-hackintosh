#!/usr/bin/env python3
"""Verify software pixels or a stock FFmpeg -> VTDecoderXPCService -> Phoenix AVC stream."""
import argparse
from datetime import datetime, timedelta
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess
import sys
import time

BUNDLE = Path("/Library/GPUBundles/PhoenixVALoadProbe.bundle")
EXE = Path("Contents/MacOS/PhoenixVALoadProbe")
SERVICE = "/System/Library/Frameworks/VideoToolbox.framework/Versions/A/XPCServices/VTDecoderXPCService.xpc/Contents/MacOS/VTDecoderXPCService"
GVA = "/System/Library/PrivateFrameworks/AppleGVA.framework/Versions/A/AppleGVA"
SUBSYSTEM = "org.nullarc.phoenix.va-load-probe"
KEY = "gvaForceAMDAVCDecode"
STOCK = "a45b462cf91ed89148ae218c4577e30896485d7a6792c3673bcf5f823fa01b63"
PEER = re.compile(r"com\.apple\.coremedia\.videodecoder\.peer\[(\d+)\]")
MANIFESTS = {
    "864x480": {
        "db401c124fb6c98448b88e4343ef84989eb2ed91accc643c2b0971a580cad708",  # descriptor input
        "386b53d90fd61169574eb9ed0949ad90ccfca30e05e49c4c5750067f7bd999ec",  # inline input
    },
    "1920x1080": {
        "e6fab346664165ba491a7011757011585ba968ff85472e8001abf8ae5cb2f35f",
        "8a088355b4f59381e5950ae6477a409e4c3febd032ea18d8e272411cb8e771fc",  # mirrored peer
    },
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def check_fixture(case):
    manifest = (case / "manifest.json").read_bytes()
    digest = sha(manifest)
    if digest not in MANIFESTS.get(case.name, ()):
        raise ValueError("Unknown fixture manifest in " + str(case))
    frames = json.loads(manifest)
    width, height = map(int, case.name.split("x"))
    size = width * height * 3 // 2
    with (case / "stream.h264").open("rb") as stream, (case / "reference.nv12").open("rb") as ref:
        for index, frame in enumerate(frames):
            au = (case / frame["au"]).read_bytes()
            if (frame["au"] != f"frame-{index:04d}.au" or len(au) != frame["au_bytes"] or
                    sha(au) != frame["au_sha256"] or stream.read(len(au)) != au):
                raise ValueError(f"Invalid access unit at {case.name} frame {index}")
            if frame["nv12_bytes"] != size or sha(ref.read(size)) != frame["nv12_sha256"]:
                raise ValueError(f"Invalid reference at {case.name} frame {index}")
        if stream.read(1) or ref.read(1):
            raise ValueError("Trailing fixture data")
    return len(frames), size, digest


def compare_pixels(case, raw, count, size):
    actual = raw.stat().st_size if raw.exists() else 0
    result = dict(output_bytes=actual, expected_bytes=count * size, matching_frames=0,
                  first_mismatch_frame=None, first_mismatch_offset_in_frame=None)
    with (case / "reference.nv12").open("rb") as ref, (raw if raw.exists() else Path(os.devnull)).open("rb") as pixels:
        for index in range(count):
            expected, observed = ref.read(size), pixels.read(size)
            if expected != observed:
                offset = next((i for i, (a, b) in enumerate(zip(expected, observed)) if a != b), len(observed))
                result.update(first_mismatch_frame=index, first_mismatch_offset_in_frame=offset)
                break
            result["matching_frames"] += 1
        if actual > count * size and result["first_mismatch_frame"] is None:
            result.update(first_mismatch_frame=count, first_mismatch_offset_in_frame=0)
    result["pixels_match"] = result["matching_frames"] == count and actual == count * size
    return result


def live_state(bundle):
    binary = bundle / EXE
    digest = sha(binary.read_bytes())
    if sha((BUNDLE / EXE).read_bytes()) != digest:
        raise ValueError("Installed Phoenix executable differs from candidate")
    uuids = subprocess.check_output(["/usr/bin/dwarfdump", "--uuid", binary], text=True)
    uuid, = re.findall(r"^UUID: ([0-9A-Fa-f-]+) \(x86_64\)", uuids, re.M)
    entries = plistlib.loads(subprocess.check_output(["/usr/sbin/ioreg", "-a", "-r", "-n", "NMXAccel", "-d", "1"]))
    devices = [d for d in entries if d.get("IODVDBundleName") == BUNDLE.stem and d.get("PhxVideoDecodeStreamABI") == 1]
    if len(devices) != 1:
        raise ValueError("Expected one Phoenix NMXAccel with PhxVideoDecodeStreamABI=1")
    prefs = {}
    for name, switches in (("any_host", []), ("current_host", ["-currentHost"])):
        domain = plistlib.loads(subprocess.check_output(["/usr/bin/defaults", *switches, "export", "com.apple.AppleGVA", "-"]))
        prefs[name] = domain.get(KEY)
    boot = subprocess.check_output(["/usr/sbin/sysctl", "-n", "kern.bootsessionuuid"], text=True).strip()
    return dict(boot=boot, registry=devices[0]["IORegistryEntryID"], uuid=uuid.upper(), bundle_sha256=digest, preferences=prefs)


def check_route(route, prefs):
    values = list(prefs.values())
    if route == "default":
        return all(v is None for v in values)
    return any(v is True for v in values) and all(v is None or v is True for v in values)


def existing_services():
    table = subprocess.check_output(["/bin/ps", "-ax", "-o", "pid=,comm="], text=True)
    fields = [line.split(None, 1) for line in table.splitlines()]
    return [int(pid) for pid, path in fields if path == SERVICE]


def collect_events(start, end, out):
    predicate = ('process == "VTDecoderXPCService" AND (eventMessage CONTAINS "videodecoder.peer[" OR '
                 'eventMessage BEGINSWITH "Phoenix " OR subsystem == "' + SUBSYSTEM + '" OR sender == "AppleGVA")')
    fmt = "%Y-%m-%d %H:%M:%S%z"
    with (out / "service.ndjson").open("w") as log:
        subprocess.run(["/usr/bin/log", "show", "--info", "--style", "ndjson", "--start", start.strftime(fmt),
                        "--end", (end + timedelta(seconds=1)).strftime(fmt), "--predicate", predicate],
                       check=True, stdout=log, timeout=45)
    lines = (out / "service.ndjson").read_text().splitlines()
    return [json.loads(line) for line in lines if line.startswith("{")]


def check_events(events, pid, route, state, start, end, previous, dimensions, count):
    """A fresh, uniquely correlated service prevents work from an older client being attributed to ours."""
    rows = []
    for event in events:
        if event.get("bootUUID") == state["boot"] and event.get("processImagePath") == SERVICE:
            stamp = datetime.strptime(event["timestamp"], "%Y-%m-%d %H:%M:%S.%f%z")
            if start <= stamp <= end:
                rows.append(event)
    rows.sort(key=lambda event: event["machTimestamp"])
    owners = {}
    for event in rows:
        peer = PEER.search(event.get("eventMessage", ""))
        if peer and event.get("subsystem") == "com.apple.xpc":
            owners.setdefault(event["processID"], set()).add(int(peer[1]))
    linked = [service for service, clients in owners.items() if pid in clients]
    if len(linked) != 1:
        return dict(passed=False, reason="Expected exactly one XPC service for this FFmpeg", services=linked)
    service, = linked
    if service in previous or owners[service] != {pid}:
        return dict(passed=False, reason="Pre-existing service or competing XPC client", service=service,
                    clients=sorted(owners[service]))
    expected_factory = f"AVD_CreateAVDAccelerator pid={service} process=VTDecoderXPCService image={BUNDLE / EXE} caller={GVA}"
    registry = state["registry"]
    expected_completions = [
        f"Phoenix AVC stream completed registry={registry} frames={number} visible={dimensions}"
        for number in [1, *range(60, count + 1, 60)]
    ]
    expected_close = f"Phoenix AVC stream closed registry={registry} frames={count}"
    seen, forced_routes = [], []
    route_query, active = False, False
    for event in rows:
        if event["processID"] != service:
            continue
        message = event.get("eventMessage", "").strip()
        sender = event.get("senderImagePath")
        if (event.get("subsystem") == "com.apple.xpc" and "activating connection" in message
                and f"videodecoder.peer[{pid}]" in message):
            active = True
        if sender == GVA and active:
            if "Playback Usage" in message:
                route_query = True
            forced = re.search(r"\bforce (.+?) for flow_id 2\b", message)
            if forced:
                forced_routes.append(forced[1])
        if message.startswith(("AVD_CreateAVDAccelerator ", "Phoenix AVC stream ", "Phoenix reference IDR completed")):
            # log show can resolve a UUID to the identical workspace copy. The
            # factory's own image= field below records the actual loaded path.
            if (not active or event.get("senderImageUUID") != state["uuid"] or
                    (message.startswith("AVD_Create") and event.get("subsystem") != SUBSYSTEM)):
                return dict(passed=False, service=service, reason="Unlinked event or wrong Phoenix identity")
            seen.append(message)
    wanted_routes = {"AMD"} if route == "forced" else set()
    # AppleGVA can call the same factory again while a stream is active.
    # Preserve every call in the report and require exactly one completed stream.
    stream_events = [message for message in seen if message != expected_factory]
    stream_verified = (seen[:1] == [expected_factory] and seen[-1:] == [expected_close]
                       and stream_events == [*expected_completions, expected_close])
    passed = route_query and set(forced_routes) == wanted_routes and stream_verified
    return dict(passed=passed, service=service, route_query=route_query, forced_routes=forced_routes, events=seen,
                reason=None if passed else "Route, factory, stream completion or close count differs")


def run_case(args, case, report):
    count, size, digest = check_fixture(case)
    report.update(fixture=str(case), manifest_sha256=digest, frames=count, bytes_per_frame=size)
    ffmpeg = args.ffmpeg.resolve()
    report["ffmpeg_sha256"] = sha(ffmpeg.read_bytes())
    if report["ffmpeg_sha256"] != STOCK:
        raise ValueError("FFmpeg differs from the stock binary used for the fixed-frame control")
    hw = args.route != "software"
    if hw:
        state = live_state(args.bundle)
        report["state_before"] = state
        if not check_route(args.route, state["preferences"]):
            raise ValueError(f"{KEY} differs from the requested route")
        previous = existing_services()
        report["preexisting_service_pids"] = previous
    raw = args.output / "decoded.nv12"
    argv = [str(ffmpeg), "-nostdin", "-n", "-hide_banner", "-loglevel", "verbose"]
    if hw:
        argv += ["-hwaccel", "videotoolbox", "-hwaccel_output_format", "videotoolbox_vld"]
    else:
        argv += ["-hwaccel", "none", "-threads", "1", "-c:v", "h264"]
    argv += ["-f", "h264", "-i", str(case / "stream.h264"), "-map", "0:v:0"]
    if hw:
        argv += ["-vf", "hwdownload,format=nv12"]
    argv += ["-fps_mode", "passthrough", "-pix_fmt", "nv12", "-f", "rawvideo", str(raw)]
    report["argv"] = argv
    env = {key: value for key, value in os.environ.items() if not key.startswith("DYLD_") and key != "FFREPORT"}
    start = datetime.now().astimezone()
    with (args.output / "ffmpeg.log").open("w") as log:
        with subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT, env=env) as client:
            report["ffmpeg_pid"] = client.pid
            try:
                report["exit_code"] = client.wait(timeout=30)
            except subprocess.TimeoutExpired:
                client.kill()
                report["exit_code"] = client.wait()
                report["timeout"] = True
    if hw:
        time.sleep(1)  # XPC teardown can finish after FFmpeg exits.
        end = datetime.now().astimezone()
        report.update(start=start.isoformat(), end=end.isoformat())
    report.update(compare_pixels(case, raw, count, size))
    if hw:
        time.sleep(1)  # Allow the bounded interval to reach the log store.
        events = collect_events(start, end, args.output)
        evidence = check_events(events, client.pid, args.route, state, start, end, previous, case.name, count)
        report["xpc"] = evidence
        report["state_after"] = live_state(args.bundle)
        report["hardware_verified"] = evidence["passed"] and state == report["state_after"]
    report["passed"] = report["exit_code"] == 0 and report["pixels_match"] and (not hw or report["hardware_verified"])
    if hw:
        report["hardware_verified"] = report["passed"]


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--route", choices=("software", "forced", "default"), required=True)
    cli.add_argument("--ffmpeg", type=Path, required=True)
    cli.add_argument("--fixture", type=Path, required=True, help="Prepared 864x480 or 1920x1080 fixture directory")
    cli.add_argument("--output", type=Path, required=True, help="New directory for this run")
    cli.add_argument("--bundle", type=Path, help="Expected candidate bundle for a live run")
    args = cli.parse_args()
    if args.route != "software" and not args.bundle:
        cli.error("Live runs need --bundle")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True)
    report = dict(route=args.route, passed=False, hardware_verified=False)
    try:
        run_case(args, args.fixture.resolve(), report)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        report["error"] = str(error)
    (args.output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))
    return int(not report["passed"])


if __name__ == "__main__":
    sys.exit(main())
