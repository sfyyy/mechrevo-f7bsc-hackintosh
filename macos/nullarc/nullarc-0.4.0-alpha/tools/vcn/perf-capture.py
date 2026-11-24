#!/usr/bin/env python3
"""Capture a Phoenix VCN AVC decode performance log while video is playing.

usage: python3 perf-capture.py [seconds] [output-dir]
Samples PhxBringup's PhxAvcLastTiming (published every 12 decoded frames),
active sessions, and CPU of the decode-related processes, then writes
raw.json, timing.csv and report.md into the output directory.
"""
import collections, csv, datetime, json, os, re, statistics, subprocess, sys, time

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 60
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "results",
    "perf-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
os.makedirs(out, exist_ok=True)

PROCS = {"VTDecoderXPCService": "vt_decoder", "Helper (GPU)": "chrome_gpu",
         "WindowServer": "windowserver", "Helper (Renderer)": "chrome_renderer"}


def reg():
    o = subprocess.run(["ioreg", "-r", "-c", "PhxBringup", "-d", "1"], capture_output=True, text=True).stdout
    t = re.search(r'"PhxAvcLastTiming" = "([^"]*)"', o)
    a = re.search(r'"PhxAvcActiveSessions" = (\d+)', o)
    b = re.search(r'"phx-build" = (\d+)', o)
    m = re.search(r'"PhxAvcReadbackMode" = "([^"]*)"', o)
    d = {}
    if t:
        for kv in t.group(1).split():
            if "=" in kv:
                k, v = kv.split("=", 1)
                d[k] = v
            elif re.fullmatch(r"\d+x\d+", kv):
                d["size"] = kv
    return d, int(a.group(1)) if a else 0, b.group(1) if b else "?", m.group(1) if m else "?"


def cpu():
    ps = subprocess.run(["ps", "-axo", "pcpu=,command="], capture_output=True, text=True).stdout
    r = {v: 0.0 for v in PROCS.values()}
    for line in ps.splitlines():
        pc, _, cmd = line.strip().partition(" ")
        for pat, key in PROCS.items():
            if pat in cmd:
                r[key] += float(pc)
    return r


start = time.time()
first, active0, build, _ = reg()
s0 = int(first.get("serial", 0))
samples, cpus, actives = {}, [], collections.Counter()
next_cpu = 0.0
while (now := time.time() - start) < secs:
    d, a, _, mode = reg()
    actives[a] += 1
    if d.get("serial") and int(d["serial"]) != s0:
        samples[int(d["serial"])] = dict(d, active=a, t=round(now, 2), mode=mode)
    if now >= next_cpu:
        cpus.append(dict(t=round(now, 1), **cpu()))
        next_cpu += 2
    time.sleep(0.15)
last, active1, _, mode1 = reg()
s1 = int(last.get("serial", s0))

# Stream completions logged by the user-space decoder (every 60 frames per stream).
log = subprocess.run(["/usr/bin/log", "show", "--last", f"{int(secs) + 5}s", "--style", "compact",
                      "--predicate", 'eventMessage CONTAINS "Phoenix AVC stream" OR eventMessage CONTAINS "Phoenix AVC rejected"'],
                     capture_output=True, text=True).stdout
rejects = [l for l in log.splitlines() if "Phoenix AVC rejected" in l and "log run" not in l]
closes = [l for l in log.splitlines() if "stream closed" in l and "log run" not in l]

json.dump(dict(build=build, seconds=secs, serial_start=s0, serial_end=s1, samples=samples, cpu=cpus,
               active_histogram=actives, rejects=rejects, closes=closes), open(os.path.join(out, "raw.json"), "w"), indent=1)
with open(os.path.join(out, "timing.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["t", "serial", "size", "idr", "slot", "active", "upload_us", "fence_us", "readback_us", "copyout_us", "readback", "pitch"])
    for s, x in sorted(samples.items()):
        w.writerow([x["t"], s, x.get("size"), x.get("idr"), x.get("slot"), x["active"], x.get("upload_us"),
                    x.get("fence_us"), x.get("readback_us"), x.get("copyout_us"), x.get("readback"), x.get("pitch")])


def stats(key):
    v = sorted(int(x[key]) for x in samples.values() if key in x)
    if not v:
        return None
    return dict(n=len(v), min=v[0], p50=v[len(v) // 2], p95=v[min(len(v) - 1, int(len(v) * .95))], max=v[-1],
                mean=round(statistics.mean(v)))


frames = (s1 - s0) & 0xffffffff
rows = []
for k in ("upload_us", "fence_us", "readback_us", "copyout_us"):
    s = stats(k)
    if s:
        rows.append(f"| {k} | {s['n']} | {s['min']} | {s['p50']} | {s['p95']} | {s['max']} | {s['mean']} |")
tot = [int(x["upload_us"]) + int(x["fence_us"]) + int(x["readback_us"]) + int(x.get("copyout_us", 0))
       for x in samples.values() if "upload_us" in x]
tot.sort()
c = {k: round(statistics.mean(x[k] for x in cpus), 1) for k in PROCS.values()} if cpus else {}
cmax = {k: round(max(x[k] for x in cpus), 1) for k in PROCS.values()} if cpus else {}
sizes = collections.Counter(x.get("size") for x in samples.values())
slots = collections.Counter(x.get("slot") for x in samples.values())
modes = collections.Counter(x.get("readback") for x in samples.values())
report = f"""# Phoenix VCN AVC 解码性能日志

- 时间：{datetime.datetime.now():%Y-%m-%d %H:%M:%S}，采样 {secs:.0f} s
- PhxBringup build {build}，结束时读回模式 `{mode1}`，结束时活动会话 {active1}
- 内核序号 {s0} → {s1}：**共解码 {frames} 帧，合计 {frames / secs:.1f} fps**（全部会话之和）
- 活动会话数分布（按采样次数）：{dict(sorted(actives.items()))}
- 分辨率：{dict(sizes)}；槽位：{dict(sorted(slots.items()))}；读回路径：{dict(modes)}

## 每帧内核耗时（µs，每 12 帧一个样本）

| 阶段 | 样本 | min | p50 | p95 | max | mean |
|---|---|---|---|---|---|---|
{chr(10).join(rows)}
| **合计** | {len(tot)} | {tot[0] if tot else '-'} | {tot[len(tot)//2] if tot else '-'} | {tot[min(len(tot)-1, int(len(tot)*.95))] if tot else '-'} | {tot[-1] if tot else '-'} | {round(statistics.mean(tot)) if tot else '-'} |

阶段含义：upload = 写消息/码流到 BAR0；fence = VCN 解码到 fence 回写；readback = 从 UMA 别名直写进 surface；copyout = 回读后剩余拷贝（直写路径应为 0）。
单路理论上限 ≈ 1e6 / p50 合计 = **{round(1e6 / tot[len(tot)//2]) if tot else '-'} fps**（全部会话共享 vcnLock，串行）。

## 进程 CPU（%，每 2 s 采样，均值 / 峰值）

| 进程 | 均值 | 峰值 |
|---|---|---|
""" + "\n".join(f"| {k} | {c.get(k)} | {cmax.get(k)} |" for k in PROCS.values()) + f"""

## 异常

- 拒绝帧（Phoenix AVC rejected）：{len(rejects)} 条
- 流关闭：{len(closes)} 条
""" + "".join(f"  - `{l.split('] ')[-1][:200]}`\n" for l in rejects[:10])
open(os.path.join(out, "report.md"), "w").write(report)
print(report)
print("written:", out)
