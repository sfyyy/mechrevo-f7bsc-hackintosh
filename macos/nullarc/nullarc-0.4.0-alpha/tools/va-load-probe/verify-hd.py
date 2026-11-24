#!/usr/bin/env python3
"""Run the recorded stock FFmpeg and verify the VideoToolbox session and H.264 output."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess
import tempfile
import time


def sha256(path):
    with path.open("rb") as stream:
        digest = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
        return digest.hexdigest()


def bundle_metadata(bundle, env):
    contents = bundle.resolve(strict=True) / "Contents"
    with (contents / "Info.plist").open("rb") as stream:
        name = plistlib.load(stream)["CFBundleExecutable"]
    if Path(name).name != name:
        raise ValueError("Invalid bundle executable name")
    image = contents / "MacOS" / name
    uuids = subprocess.check_output(["/usr/bin/dwarfdump", "--uuid", str(image)],
                                    env=env, text=True)
    match = re.search(r"^UUID: ([A-Fa-f0-9-]+) \(x86_64\)", uuids, re.M)
    if not match:
        raise ValueError("Expected an x86_64 Mach-O image")
    return {"path": str(image), "x86_64_uuid": match[1], "sha256": sha256(image)}


def write_frames(path, width, height, frames):
    with path.open("wb") as out:
        for index in range(frames):
            if frames == 1:
                # A new macroblock-aligned luma pattern distinguishes this run from both
                # encoder initialization pictures and concurrent or stale probe events.
                for y in range(0, height, 16):
                    levels = os.urandom((width + 15) // 16)
                    row = b"".join(bytes([32 + level % 177]) * 16 for level in levels)[:width]
                    out.write(row * min(16, height - y))
                # Give Cb and Cr distinct values so that a swapped UV plane fails decode validation.
                u, v = os.urandom(2)
                out.write(bytes([64 + u % 48, 144 + v % 48]) * (width * height // 4))
            else:
                left, right = (48, 200) if index % 2 else (200, 48)
                row = bytes([left]) * (width // 2) + bytes([right]) * (width // 2)
                out.write(row * height)
                out.write(bytes([80 if index & 2 else 176, 80 if index & 4 else 176]) *
                          (width * height // 4))


def factory_evidence(events, client_pid, ffmpeg, image):
    """Require a peer connection to our client and the matching probe's own event."""
    helper = ("/System/Library/Frameworks/VideoToolbox.framework/Versions/A/XPCServices/"
              "VTEncoderXPCService.xpc/Contents/MacOS/VTEncoderXPCService")
    apple_gva = "/System/Library/PrivateFrameworks/AppleGVA.framework/Versions/A/AppleGVA"
    peer_name = f"name=com.apple.coremedia.videoencoder.peer[{client_pid}]."
    peers = {}
    hits = []
    for event in events:
        proc = event.get("processID")
        path = event.get("processImagePath")
        message = event.get("eventMessage", "")
        if path == helper and "peer=true" in message and peer_name in message:
            peers[proc] = event
        if not image or event.get("subsystem") != "org.nullarc.phoenix.va-load-probe":
            continue
        if not (event.get("senderImagePath") == image["path"] and
                event.get("senderImageUUID", "").upper() == image["x86_64_uuid"].upper() and
                message.startswith(f"AVD_CreateAVDAccelerator pid={proc} ") and
                message.endswith(" caller=" + apple_gva)):
            continue
        if (proc in peers and path == helper) or (proc == client_pid and path == str(ffmpeg)):
            hits.append({"pid": proc, "factory_event": event, "client_connection": peers.get(proc)})
    return hits


def video_context_evidence(events, image, xpc_pids, width, height):
    size = width * height * 3 // 2
    staged_pattern = re.compile(rf"Video context staged NV12 {width} x {height} bytes={size} sha256=([0-9a-f]{{64}})")
    submitted_pattern = re.compile(rf"Video context submitted NV12 bytes={size} sha256=([0-9a-f]{{64}}) kr=([0-9a-f]+)")
    staged, submitted = [], []
    for event in events:
        if (event.get("processID") in xpc_pids and event.get("senderImagePath") == image["path"]
                and event.get("senderImageUUID", "").upper() == image["x86_64_uuid"].upper()
                and event.get("subsystem") == "org.nullarc.phoenix.va-load-probe"
                and event.get("category") == "video-context"):
            message = event.get("eventMessage", "")
            match = staged_pattern.fullmatch(message)
            if match:
                staged.append(match[1])
            match = submitted_pattern.fullmatch(message)
            if match:
                submitted.append({"pid": event["processID"], "sha256": match[1], "kr": int(match[2], 16)})
    return staged, submitted


def completed_input_events(events, xpc_pids, width, height, digest):
    pattern = re.compile(rf"Phoenix H.264 hardware frame {width}x{height}, ([1-9][0-9]*) bytes "
                         rf"input_sha256={digest} transport=(video-context|direct)")
    completed = []
    for event in events:
        match = pattern.fullmatch(event["eventMessage"])
        if match and event["processID"] in xpc_pids:
            completed.append({"pid": event["processID"], "bytes": int(match[1]), "transport": match[2]})
    return completed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("result_base", type=Path)
    parser.add_argument("--bundle", type=Path, help="Record the on-disk identity of this bundle")
    parser.add_argument("--one-frame", action="store_true",
                        help="Require the installed Phoenix VA backend to encode a verified 160x128 frame")
    parser.add_argument("--probe-input", action="store_true",
                        help="Check delivery of the 160x128 input to the XPC probe without requesting MP4 headers")
    parser.add_argument("--transport", choices=["video-context", "direct"],
                        help="Require this transport for a one-frame encode (default: video-context)")
    args = parser.parse_args()
    if args.probe_input and not args.one_frame:
        parser.error("--probe-input requires --one-frame")
    if args.one_frame and not args.bundle:
        parser.error("--one-frame requires --bundle")
    if args.transport and (not args.one_frame or args.probe_input):
        parser.error("--transport requires --one-frame without --probe-input")
    width, height, frames = (160, 128, 1) if args.one_frame else (1920, 1080, 8)
    base = args.result_base.resolve(strict=True)
    ffmpeg = base / "ffmpeg"
    manifest = (base / "source-check.txt").read_text()
    matching = re.findall(r"^ffmpeg sha256=([0-9a-f]{64})$", manifest, re.M)
    digest = sha256(ffmpeg)
    if matching != [digest]:
        parser.error("FFmpeg does not match the source-check.txt binary hash")
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("DYLD_", "__XPC_DYLD_", "XPC_DYLD_"))}
    run = Path(tempfile.mkdtemp(prefix="verify-hd-", dir=base))
    print(run, flush=True)
    identity = bundle_metadata(args.bundle, env) if args.bundle else None
    output = run / "hardware.mp4"
    raw = run / "input.nv12"
    write_frames(raw, width, height, frames)
    argv = [str(ffmpeg), "-nostdin", "-hide_banner", "-loglevel", "debug", "-f", "rawvideo",
            "-pixel_format", "nv12", "-video_size", f"{width}x{height}", "-framerate", "25",
            "-i", str(raw), "-frames:v", str(frames), "-c:v", "h264_videotoolbox",
            "-allow_sw", "0", "-b:v", "4M"]
    if args.probe_input:
        # A global-header muxer makes FFmpeg encode a synthetic picture to obtain SPS/PPS at open.
        argv.extend(["-flags:v", "-global_header", "-f", "null", "-"])
    else:
        argv.append(str(output))
    start = datetime.datetime.now(datetime.timezone.utc)
    logfile = run / "ffmpeg.log"
    with logfile.open("wb") as stream:
        process = subprocess.Popen(argv, env=env, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.DEVNULL, stderr=stream)
        try:
            status = process.wait(timeout=120)
        except subprocess.TimeoutExpired:
            process.kill()
            status = process.wait()
    end = datetime.datetime.now(datetime.timezone.utc)
    # The unified log is asynchronous and the query accepts whole seconds.
    time.sleep(1)
    query = ["/usr/bin/log", "show", "--style", "ndjson", "--info", "--debug",
             "--start", start.astimezone().strftime("%Y-%m-%d %H:%M:%S"),
             "--end", (end + datetime.timedelta(seconds=1)).astimezone().strftime("%Y-%m-%d %H:%M:%S"),
             "--predicate", f'processIdentifier == {process.pid} OR process == "VTEncoderXPCService" OR '
                            'subsystem == "org.nullarc.phoenix.va-load-probe"']
    records = subprocess.run(query, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True, timeout=30, env=env)
    (run / "events.ndjson").write_text(records.stdout)
    events = [json.loads(line) for line in records.stdout.splitlines() if line.startswith("{")]
    hits = factory_evidence(events, process.pid, ffmpeg, identity)
    factory_pids = {hit["pid"] for hit in hits}
    completions = [event for event in events
                   if event.get("processID") in factory_pids and identity
                   and event.get("senderImagePath") == identity["path"]
                   and event.get("senderImageUUID", "").upper() == identity["x86_64_uuid"].upper()
                   and event.get("subsystem") == "org.nullarc.phoenix.va-load-probe"
                   and event.get("category") == "encode"
                   and event.get("eventMessage", "").startswith("Phoenix H.264 hardware frame ")]
    hardware_open_results = []
    for event in events:
        if (event.get("processID") in factory_pids and identity
                and event.get("senderImagePath") == identity["path"]
                and event.get("senderImageUUID", "").upper() == identity["x86_64_uuid"].upper()
                and event.get("category") == "avd"):
            match = re.fullmatch(r"AVD hardware open result=(-?\d+)", event.get("eventMessage", ""))
            if match:
                hardware_open_results.append(int(match[1]))
    (run / "hardware-frame-events.json").write_text(json.dumps(completions, indent=2) + "\n")
    (run / "factory-evidence.json").write_text(json.dumps(hits, indent=2) + "\n")
    (run / "log-query.stderr").write_text(records.stderr)
    log = logfile.read_text(errors="replace")
    bytes_written = output.stat().st_size if output.exists() else 0
    encoded = re.search(r"(\d+) frames encoded; (\d+) packets muxed", log)
    encoded_count = int(encoded[1]) if encoded else 0
    packet_count = int(encoded[2]) if encoded else 0
    report = {"pid": process.pid, "start_utc": start.isoformat(), "end_utc": end.isoformat(),
              "argv": argv, "ffmpeg_sha256": digest, "bundle_on_disk": identity,
              "coremedia_query_status": records.returncode,
              "factory_entry_proven": bool(hits), "linked_factory_pids": [hit["pid"] for hit in hits],
              "phoenix_hardware_frame_events": len(completions),
              "phoenix_hardware_open_results": hardware_open_results,
              "exit_code": status, "bytes": bytes_written, "encoded_frames": encoded_count,
              "muxed_packets": packet_count, "decoded_frames": None,
              "session_error": re.findall(r"cannot create compression session: (-?\d+)", log),
              "selected_encoder": re.findall(r"Init the encoder: ([^\s]+)", log)}
    ok = status == 0 and bytes_written > 0 and encoded_count == packet_count == frames
    if args.one_frame:
        xpc_pids = {hit["pid"] for hit in hits if hit["client_connection"]}
        staged, submitted = video_context_evidence(events, identity, xpc_pids, width, height)
        input_digest = sha256(raw)
        report["input_nv12_sha256"] = input_digest
        report["xpc_staged_nv12_sha256"] = staged
        report["xpc_staged_input_match"] = records.returncode == 0 and input_digest in staged
        report["xpc_video_submissions"] = submitted
        report["xpc_submitted_input_match"] = records.returncode == 0 and any(
            item["sha256"] == input_digest and item["kr"] == 0 for item in submitted)
        completed = completed_input_events(completions, xpc_pids, width, height, input_digest)
        report["xpc_completed_input_events"] = completed
        report["xpc_completed_input_match"] = records.returncode == 0 and bool(completed)
        if not args.probe_input:
            expected = args.transport or "video-context"
            report["required_transport"] = expected
            report["xpc_completed_transport_match"] = records.returncode == 0 and any(
                item["transport"] == expected for item in completed)
    if ok:
        # The archived minimal CLI has no rawvideo encoder. The null output codec
        # wrapped_avframe may also be disabled. Use AVFoundation to count decoded frames.
        decode = subprocess.run(["/usr/bin/xcrun", "swift", "-", str(output), str(width), str(height),
                                 str(frames), str(raw)], input=SWIFT,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=120, text=True, env=env)
        (run / "decode.log").write_text(decode.stdout)
        count = re.search(r"^verified_frames=(\d+)$", decode.stdout, re.M)
        report["decoded_frames"] = int(count[1]) if count else None
        ok = decode.returncode == 0 and report["decoded_frames"] == frames
    report["hardware_encode_and_decode_check_passed"] = ok
    if args.one_frame:
        ok = ok and report.get("xpc_completed_transport_match", False)
        report["stock_phoenix_one_frame_passed"] = ok
    if args.probe_input:
        report["input_delivery_probe_passed"] = report["xpc_staged_input_match"] and report["xpc_submitted_input_match"]
    (run / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, ensure_ascii=False))
    if args.probe_input:
        return 0 if report["input_delivery_probe_passed"] else 1
    if identity and not args.one_frame:
        return 0 if hits else 1
    return 0 if ok else 1


SWIFT = """
import AVFoundation
import Foundation
let asset = AVURLAsset(url: URL(fileURLWithPath: CommandLine.arguments[1]))
let width = Int(CommandLine.arguments[2])!
let height = Int(CommandLine.arguments[3])!
let frames = Int(CommandLine.arguments[4])!
let reference = frames == 1 ? try Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[5])) : Data()
guard frames != 1 || reference.count == width * height * 3 / 2 else { exit(1) }
let reader = try AVAssetReader(asset: asset)
guard let track = asset.tracks(withMediaType: .video).first else { exit(1) }
guard track.formatDescriptions.count == 1,
      CMFormatDescriptionGetMediaSubType(track.formatDescriptions[0] as! CMFormatDescription)
          == kCMVideoCodecType_H264 else { exit(1) }
let out = AVAssetReaderTrackOutput(track: track, outputSettings: [
    kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange])
guard reader.canAdd(out) else { exit(1) }
reader.add(out)
guard reader.startReading() else { exit(1) }
var count = 0
while let sample = out.copyNextSampleBuffer() {
    guard count < frames, let img = CMSampleBufferGetImageBuffer(sample),
          CVPixelBufferGetWidth(img) == width, CVPixelBufferGetHeight(img) == height,
          CVPixelBufferGetPixelFormatType(img) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
          CMTimeCompare(CMSampleBufferGetPresentationTimeStamp(sample),
                        CMTime(value: Int64(count), timescale: 25)) == 0,
          CVPixelBufferLockBaseAddress(img, .readOnly) == kCVReturnSuccess else { exit(1) }
    guard let y = CVPixelBufferGetBaseAddressOfPlane(img, 0)?.assumingMemoryBound(to: UInt8.self),
          let uv = CVPixelBufferGetBaseAddressOfPlane(img, 1)?.assumingMemoryBound(to: UInt8.self)
          else { exit(1) }
    let yStride = CVPixelBufferGetBytesPerRowOfPlane(img, 0)
    let uvStride = CVPixelBufferGetBytesPerRowOfPlane(img, 1)
    if frames == 1 {
        // Compare both planes against this run's reference, excluding row padding.
        for row in 0..<height {
            for col in 0..<width {
                guard abs(Int(y[row * yStride + col]) - Int(reference[row * width + col])) <= 16
                    else { exit(1) }
            }
        }
        for row in 0..<(height / 2) {
            for col in 0..<width {
                guard abs(Int(uv[row * uvStride + col]) - Int(reference[(height + row) * width + col])) <= 16
                    else { exit(1) }
            }
        }
    } else {
        for row in [height / 4, 3 * height / 4] {
            for col in [width / 4, 3 * width / 4] {
                let expectedY = ((col < width / 2) == (count % 2 == 0)) ? 200 : 48
                let ci = (row / 2) * uvStride + (col / 2) * 2
                guard abs(Int(y[row * yStride + col]) - expectedY) <= 12,
                      abs(Int(uv[ci]) - (count & 2 != 0 ? 80 : 176)) <= 12,
                      abs(Int(uv[ci + 1]) - (count & 4 != 0 ? 80 : 176)) <= 12 else { exit(1) }
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(img, .readOnly)
    count += 1
}
print("verified_frames=\\(count)")
exit(reader.status == .completed && count == frames ? 0 : 1)
"""

if __name__ == "__main__":
    raise SystemExit(main())
