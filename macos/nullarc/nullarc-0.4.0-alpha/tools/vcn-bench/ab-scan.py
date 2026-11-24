#!/usr/bin/env python3
"""A/B the decode path in one boot: every optimisation is behind a sysctl, so the
comparison does not need a reboot per build.

usage: ab-scan.py <output-dir> [--runs N]

It runs, per configuration, the native path (vt-metal, both clips) and the stock
FFmpeg path, and records medians. The configurations are staged so a result can
be attributed to one switch:

    off        build 75 behaviour (all three switches off)
    wait1      fence wait: sub-millisecond sleep only
    wait2      fence wait: spin + sleep
    idr        + IDR session reuse
    flush1     + visible-row cache invalidation
    flush2     + interleaved per-row invalidation

The script never touches the kext; it only writes debug.* sysctls.
"""
import json
import os
import pathlib
import re
import statistics
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE
FFMPEG = ROOT / "results/vt-stock-ffmpeg/ffmpeg"
VT_METAL = pathlib.Path("/tmp/vt-metal")
CLIPS = {"B": ROOT / "results/build75-playback-audit.mUaoWr/gop60.h264",
         "A": ROOT / "results/build71-cpu-compare.GoFC6LIS/repeated-1080p.h264"}
# The 30 fps CPU test (AC-2.4): a 24-frame 1080p capture looped to ten seconds.
CPU_CLIP = ROOT / "results/vcn-build73-chrome/douyu-1080.h264"
VT_BENCH = ROOT / "vt-bench"
# Optional R-8 control: our FFmpeg 9.0.2 build that can drop the VT texture
# compatibility attribute (VT_NO_OPENGL_COMPAT=1). Present only if that was built.
PATCHED_FFMPEG = ROOT / "results/ffmpeg-patched/ffmpeg"

CONFIGS = [
    ("off",    dict(wait=0, idr=0, flush=0)),
    ("wait1",  dict(wait=1, idr=0, flush=0)),
    ("wait2",  dict(wait=2, idr=0, flush=0)),
    ("idr",    dict(wait=2, idr=1, flush=0)),
    ("flush1", dict(wait=2, idr=1, flush=1)),
    ("flush2", dict(wait=2, idr=1, flush=2)),
]


def sysctl(wait, idr, flush):
    subprocess.run(["sudo", "-n", "sysctl", "-w",
                    f"debug.phx_vcn_wait={wait}",
                    f"debug.phx_avc_idr_reuse={idr}",
                    f"debug.phx_avc_flush_mode={flush}"], check=True, capture_output=True)


def ensure_sysctls():
    """The switches appear when the first user client connects (newUserClient),
    so one decode has to run before sysctl -w can find them."""
    probe = subprocess.run(["sudo", "-n", "sysctl", "-n", "debug.phx_vcn_wait"],
                           capture_output=True, text=True)
    if probe.returncode == 0:
        return
    print("sysctls not registered yet: running one decode to bring the client up", flush=True)
    run_vt_metal("B")
    time.sleep(0.5)
    probe = subprocess.run(["sudo", "-n", "sysctl", "-n", "debug.phx_vcn_wait"],
                           capture_output=True, text=True)
    if probe.returncode != 0:
        raise SystemExit("debug.phx_vcn_wait is missing: is build 78 installed? " + probe.stderr)


def build_vt_metal():
    if VT_METAL.exists():
        return
    subprocess.run(["xcrun", "clang", "-std=c11", "-O2",
                    str(ROOT / "results/build75-playback-audit.mUaoWr/vt-metal.m"),
                    "-framework", "Metal", "-framework", "VideoToolbox",
                    "-framework", "CoreMedia", "-framework", "CoreVideo",
                    "-framework", "CoreFoundation", "-o", str(VT_METAL)], check=True)


def run_vt_metal(clip):
    out = subprocess.run([str(VT_METAL), "--hardware", str(CLIPS[clip])],
                         capture_output=True, text=True, timeout=180)
    for line in out.stdout.splitlines():
        if line.startswith("{"):
            return json.loads(line)
    raise RuntimeError(out.stdout[-300:] + out.stderr[-300:])


def run_ffmpeg(clip, output, binary=None, env=None):
    if output == "download":
        extra = ["-vf", "hwdownload,format=nv12"]
    else:
        extra = []
    # verbose, not error: the "N frames decoded; M decode errors" summary is logged at
    # verbose level, and without it the run looks like zero frames.
    cmd = [str(binary or FFMPEG), "-nostdin", "-hide_banner", "-loglevel", "verbose", "-benchmark", "-threads", "1",
           "-hwaccel", "videotoolbox", "-hwaccel_output_format", "videotoolbox_vld",
           "-f", "h264", "-i", str(CLIPS[clip]), "-map", "0:v:0", *extra,
           "-fps_mode", "passthrough", "-c:v", "wrapped_avframe", "-f", "null", "-"]
    start = time.monotonic()
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=180, env=env)
    wall = time.monotonic() - start
    frames = re.search(r"(\d+) frames decoded; (\d+) decode errors", out.stderr)
    return dict(frames=int(frames.group(1)) if frames else 0,
                errors=int(frames.group(2)) if frames else -1,
                wall=wall, fps=(int(frames.group(1)) / wall) if frames else 0.0)


def run_cpu(wait):
    sysctl(wait, 0, 0)
    time.sleep(0.3)
    out = subprocess.run([str(VT_BENCH), "--hardware", str(CPU_CLIP), "13", "--realtime", "30"],
                         capture_output=True, text=True, timeout=300)
    for line in out.stdout.splitlines():
        if line.startswith("{"):
            return json.loads(line)
    raise RuntimeError(out.stdout[-300:] + out.stderr[-300:])


def registry():
    text = subprocess.run(["ioreg", "-r", "-c", "PhxBringup", "-d", "1"],
                          capture_output=True, text=True).stdout
    timing = re.search(r'"PhxAvcLastTiming" = "([^"]*)"', text)
    counters = re.search(r'"PhxAvcCounters" = "([^"]*)"', text)
    build = re.search(r'"phx-build" = (\d+)', text)
    fields = {}
    for match in (timing, counters):
        if match:
            for item in match.group(1).split():
                if "=" in item:
                    key, value = item.split("=", 1)
                    fields[key] = value
    return build.group(1) if build else "?", fields


def main():
    out = pathlib.Path(sys.argv[1])
    runs = 3
    if "--runs" in sys.argv:
        runs = int(sys.argv[sys.argv.index("--runs") + 1])
    out.mkdir(parents=True, exist_ok=True)
    build_vt_metal()
    ensure_sysctls()
    results = {}
    for name, config in CONFIGS:
        sysctl(config["wait"], config["idr"], config["flush"])
        time.sleep(0.3)
        entry = {"config": config, "native": {}, "ffmpeg": {}}
        for clip in ("B", "A"):
            samples = [run_vt_metal(clip) for _ in range(runs)]
            entry["native"][clip] = dict(
                fps=round(statistics.median(s["fps"] for s in samples), 1),
                ms=round(1000 / statistics.median(s["fps"] for s in samples), 3),
                cpu_ms=round(statistics.median(s["cpu_ms_per_frame"] for s in samples), 2),
                runs=[round(s["fps"], 1) for s in samples])
        for output in ("surface", "download"):
            samples = [run_ffmpeg("B", output) for _ in range(runs)]
            entry["ffmpeg"][output] = dict(
                fps=round(statistics.median(s["fps"] for s in samples), 1),
                errors=sum(s["errors"] for s in samples),
                runs=[round(s["fps"], 1) for s in samples])
        entry["registry"] = registry()
        results[name] = entry
        print(f"{name:7s} nativeB={entry['native']['B']['fps']:6.1f} nativeA={entry['native']['A']['fps']:6.1f} "
              f"ffmpegB={entry['ffmpeg']['surface']['fps']:6.1f}/{entry['ffmpeg']['download']['fps']:6.1f} "
              f"timing={entry['registry'][1].get('wait', '?')}/{entry['registry'][1].get('idr_reuse', '?')}/"
              f"{entry['registry'][1].get('flush_us', '?')}", flush=True)
    # AC-2.4: playback CPU at 30 fps, per wait mode. The spin must not cost more
    # than the +5% budget over the IOSleep loop.
    cpu = {}
    for wait in (0, 1, 2):
        samples = [run_cpu(wait) for _ in range(3)]
        cpu[wait] = dict(cpu_ms=round(statistics.median(s["cpu_ms_per_frame"] for s in samples), 2),
                         cpu_pct=round(statistics.median(s["cpu_pct_of_one_core"] for s in samples), 1),
                         runs=[round(s["cpu_ms_per_frame"], 2) for s in samples])
        print(f"cpu wait={wait}: {cpu[wait]['cpu_ms']} ms/frame ({cpu[wait]['cpu_pct']}% of one core)", flush=True)
    results["cpu_realtime30"] = cpu
    # R-8 control on the all-on configuration: the same stock command line, but
    # with our build that can skip the OpenGL-texture-compatibility attribute.
    if PATCHED_FFMPEG.exists():
        patched = {}
        env = dict(os.environ, VT_NO_OPENGL_COMPAT="1")
        for clip in ("B", "A"):
            samples = [run_ffmpeg(clip, "download", PATCHED_FFMPEG, env) for _ in range(3)]
            patched[clip] = dict(fps=round(statistics.median(s["fps"] for s in samples), 1),
                                 errors=sum(s["errors"] for s in samples))
            print(f"r8 patched {clip}: {patched[clip]['fps']} fps", flush=True)
        results["ffmpeg_patched"] = patched
    sysctl(2, 1, 1)   # leave the defaults on
    (out / "ab.json").write_text(json.dumps(results, indent=2) + "\n")
    rows = ["| 配置 | 原生 B | 原生 A | FFmpeg B surface | FFmpeg B download | 解码错误 |",
            "|---|---|---|---|---|---|"]
    for name, _ in CONFIGS:
        e = results[name]
        rows.append(f"| {name} | {e['native']['B']['fps']} | {e['native']['A']['fps']} | "
                    f"{e['ffmpeg']['surface']['fps']} | {e['ffmpeg']['download']['fps']} | "
                    f"{e['ffmpeg']['surface']['errors'] + e['ffmpeg']['download']['errors']} |")
    rows.append("")
    rows.append("## 30 fps playback CPU (vt-bench --realtime 30, 1080p douyu)")
    rows.append("")
    rows.append("| wait mode | CPU ms/frame | one core |")
    rows.append("|---|---|---|")
    for wait, entry in cpu.items():
        rows.append(f"| {wait} | {entry['cpu_ms']} | {entry['cpu_pct']}% |")
    (out / "report.md").write_text("# build " + results["off"]["registry"][0] + " A/B\n\n" + "\n".join(rows) + "\n")
    print("written", out / "report.md")


if __name__ == "__main__":
    main()
