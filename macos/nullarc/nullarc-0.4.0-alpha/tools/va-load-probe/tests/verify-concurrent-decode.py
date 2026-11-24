#!/usr/bin/env python3
"""Verify two simultaneous default-route H.264 decoders and reference frames across peer teardown."""
import argparse
import asyncio
from contextlib import ExitStack, suppress
from datetime import datetime
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import time

vt = runpy.run_path(str(Path(__file__).with_name("verify-stream-decode.py")))
PHASE_TIMEOUT = 20


def now():
    return datetime.now().astimezone()


def ffmpeg_command(binary, output):
    # Read individual access units from stdin so the test controls when each
    # stream can advance. Flush every raw frame to expose progress before EOF.
    return [str(binary), "-nostdin", "-n", "-hide_banner", "-loglevel", "verbose",
            "-probesize", "32", "-analyzeduration", "1", "-fpsprobesize", "0",
            "-threads", "1", "-hwaccel", "videotoolbox", "-hwaccel_output_format", "videotoolbox_vld",
            "-f", "h264", "-i", "pipe:0", "-map", "0:v:0", "-vf", "hwdownload,format=nv12",
            "-fps_mode", "passthrough", "-pix_fmt", "nv12", "-c:v", "rawvideo", "-threads:v", "1",
            "-flush_packets", "1", "-f", "rawvideo", str(output)]


async def feed(client, fixture, first, stop):
    for index in range(first, stop):
        client.stdin.write((fixture / f"frame-{index:04d}.au").read_bytes())
        await client.stdin.drain()


async def await_pixels(client, output, required_bytes):
    while not output.exists() or output.stat().st_size < required_bytes:
        if client.returncode is not None:
            raise ValueError(f"{output.parent.name} exited with {client.returncode} before writing {required_bytes} bytes")
        await asyncio.sleep(0.025)


async def exercise(args, streams, report):
    clients = []
    env = {k: v for k, v in os.environ.items() if not k.startswith("DYLD_") and k != "FFREPORT"}
    with ExitStack() as stack:
        try:
            for s in streams:
                argv = ffmpeg_command(args.ffmpeg, s["raw"])
                log = stack.enter_context((s["raw"].parent / "ffmpeg.log").open("w"))
                client = await asyncio.create_subprocess_exec(*argv, stdin=asyncio.subprocess.PIPE,
                                                             stdout=log, stderr=subprocess.STDOUT, env=env)
                clients.append(client)
                s["report"].update(ffmpeg_pid=client.pid, argv=argv)

            async def step(i, first, stop, frames):
                s, client = streams[i], clients[i]
                report["phase"] = f"{s['label']} access units {first}..{stop - 1}"
                await asyncio.wait_for(feed(client, s["case"], first, stop), PHASE_TIMEOUT)
                await asyncio.wait_for(await_pixels(client, s["raw"], frames * s["size"]), PHASE_TIMEOUT)
                report["progress"].append(dict(stream=s["label"], fed=stop, output_frames_at_least=frames,
                                               time=now().isoformat()))

            async def finish(i):
                s, client = streams[i], clients[i]
                client.stdin.close()
                status = await asyncio.wait_for(client.wait(), PHASE_TIMEOUT)
                if status:
                    raise ValueError(f"{s['label']} FFmpeg exited with {status}")

            # Independent IDRs start each session. Frames 1..11 (zero-based)
            # are P frames. Both must make progress on their own references.
            await step(0, 0, 6, 2)
            await step(1, 0, 6, 2)
            await step(0, 6, 8, 7)
            await step(1, 6, streams[1]["count"], 7)
            await finish(1)
            await asyncio.sleep(1)  # The XPC service tears down asynchronously.
            report["resume_after_peer_exit"] = now().isoformat()
            # No new IDR is provided before this P-frame checkpoint.
            await step(0, 8, 12, 9)
            await step(0, 12, streams[0]["count"], 11)
            await finish(0)
            report["phase"] = "complete"
        finally:
            for client in clients:
                if client.returncode is None:
                    with suppress(ProcessLookupError):
                        client.kill()
                client.stdin.close()
            await asyncio.gather(*(client.wait() for client in clients))
            for s, client in zip(streams, clients):
                s["report"]["exit_code"] = client.returncode


def stream_timestamps(events, evidence, state):
    result = {}
    for e in events:
        if (e.get("processID") != evidence["service"] or e.get("bootUUID") != state["boot"]
                or e.get("senderImageUUID") != state["uuid"] or e.get("processImagePath") != vt["SERVICE"]):
            continue
        msg = e.get("eventMessage", "")
        if msg.startswith("Phoenix AVC stream completed "):
            result["first"] = datetime.strptime(e["timestamp"], "%Y-%m-%d %H:%M:%S.%f%z")
        elif msg.startswith("Phoenix AVC stream closed "):
            result["close"] = datetime.strptime(e["timestamp"], "%Y-%m-%d %H:%M:%S.%f%z")
    return result


def prepare_streams(args, report):
    streams = []
    seen_frames = set()
    for role, case in (("primary", args.fixtures / "1920x1080"),
                       ("peer", args.peer_fixture or args.fixtures / "864x480")):
        count, size, digest = vt["check_fixture"](case)
        expected = {f["nv12_sha256"] for f in json.loads((case / "manifest.json").read_bytes())}
        if expected & seen_frames:
            raise ValueError("The two streams must have distinct reference pictures")
        seen_frames.update(expected)
        label = f"{role}-{case.name}"
        out = args.output / label
        out.mkdir()
        details = dict(fixture=str(case), resolution=case.name, frames=count, manifest_sha256=digest)
        report["streams"][label] = details
        streams.append(dict(label=label, case=case, count=count, size=size, raw=out / "decoded.nv12", report=details))
    return streams


def verify(args, report):
    streams = prepare_streams(args, report)
    report["ffmpeg_sha256"] = vt["sha"](args.ffmpeg.read_bytes())
    if report["ffmpeg_sha256"] != vt["STOCK"]:
        raise ValueError("FFmpeg differs from the stock binary used by the fixed-frame control")
    state = vt["live_state"](args.bundle)
    report["state_before"] = state
    if not vt["check_route"]("default", state["preferences"]):
        raise ValueError("Forced AMD preference is present")
    previous = vt["existing_services"]()
    start = now()
    try:
        asyncio.run(exercise(args, streams, report))
    except (OSError, ValueError, asyncio.TimeoutError) as exc:
        report["error"] = f"{type(exc).__name__} during {report.get('phase', 'startup')}: {exc}"
    time.sleep(1)
    end = now()
    time.sleep(1)
    events = vt["collect_events"](start, end, args.output)
    times = []
    for s in streams:
        entry = s["report"]
        entry.update(vt["compare_pixels"](s["case"], s["raw"], s["count"], s["size"]))
        if "ffmpeg_pid" not in entry:
            continue
        xpc = vt["check_events"](events, entry["ffmpeg_pid"], "default", state, start, end, previous,
                                 s["case"].name, s["count"])
        entry["xpc"] = xpc
        if xpc["passed"]:
            stamps = stream_timestamps(events, xpc, state)
            entry["hardware_times"] = {key: value.isoformat() for key, value in stamps.items()}
            times.append(stamps)
    report["state_after"] = vt["live_state"](args.bundle)
    if len(times) == 2 and "resume_after_peer_exit" in report:
        first, peer = times
        resume = datetime.fromisoformat(report["resume_after_peer_exit"])
        report["overlap_and_resume_verified"] = first["first"] < peer["first"] < peer["close"] <= resume < first["close"]
    report["passed"] = (report.get("phase") == "complete" and report["overlap_and_resume_verified"]
                        and report["state_after"] == state
                        and all(s["report"]["pixels_match"] for s in streams))


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--ffmpeg", type=Path, required=True)
    cli.add_argument("--fixtures", type=Path, required=True, help="Parent directory of the standard fixture resolutions")
    cli.add_argument("--peer-fixture", type=Path, help="Second fixture directory, defaults to the standard 864x480 clip")
    cli.add_argument("--output", type=Path, required=True, help="New output directory")
    cli.add_argument("--bundle", type=Path, default=vt["BUNDLE"])
    args = cli.parse_args()
    args.ffmpeg = args.ffmpeg.resolve()
    args.fixtures = args.fixtures.resolve()
    if args.peer_fixture:
        args.peer_fixture = args.peer_fixture.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True)
    report = dict(route="default", passed=False, overlap_and_resume_verified=False, streams={}, progress=[])
    try:
        verify(args, report)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        report["error"] = str(exc)
    (args.output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))
    return int(not report["passed"])


if __name__ == "__main__":
    sys.exit(main())
