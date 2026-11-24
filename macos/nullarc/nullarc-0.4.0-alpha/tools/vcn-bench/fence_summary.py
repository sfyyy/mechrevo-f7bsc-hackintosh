"""Group the fence-sleep.d trace lines and report the same shape as fence-summary.json.

One line per ioctl: frame <timestamp> <pid> <tid> <arg1> <sleeps> <off_count> <blocked>
                          <ioctl_ns> <off_cpu_ns> <block_ns> <on_cpu_ns>
"""
import statistics


def _percentile(values, q):
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * q / 100.0
    low = int(position)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def load_frames(path):
    frames = []
    with open(path) as log:
        for line in log:
            fields = line.split()
            if len(fields) != 12 or fields[0] != "frame":
                continue
            frames.append(dict(timestamp=int(fields[1]), pid=int(fields[2]), tid=int(fields[3]),
                               selector=int(fields[4]), sleeps=int(fields[5]),
                               ioctl_ns=int(fields[8]), off_ns=int(fields[9]),
                               block_ns=int(fields[10]), cpu_ns=int(fields[11])))
    return frames


def summarize(frames):
    groups = {}
    for frame in frames:
        groups.setdefault(frame["sleeps"], []).append(frame)
    summary = {}
    for sleeps, members in sorted(groups.items()):
        name = ("idr_%d_sleeps" if sleeps >= 3 else "p_%d_sleeps") % sleeps
        entry = {"n": len(members)}
        for label, key in (("ioctl_ms", "ioctl_ns"), ("off_cpu_ms", "off_ns"),
                           ("block_ms", "block_ns"), ("on_cpu_ms", "cpu_ns")):
            values = [m[key] / 1e6 for m in members]
            entry[label] = {"p50": round(_percentile(values, 50), 4),
                            "p95": round(_percentile(values, 95), 4),
                            "mean": round(statistics.mean(values), 4)}
        summary[name] = entry
    stamps = [f["timestamp"] for f in frames]
    if len(stamps) > 1:
        gaps = [(b - a) / 1e6 for a, b in zip(stamps, stamps[1:])]
        summary["all"] = {"n": len(frames),
                          "ioctl_ms": {"p50": round(_percentile([f["ioctl_ns"] / 1e6 for f in frames], 50), 4),
                                       "p95": round(_percentile([f["ioctl_ns"] / 1e6 for f in frames], 95), 4)},
                          "gap_ms": {"p50": round(_percentile(gaps, 50), 4),
                                     "p95": round(_percentile(gaps, 95), 4),
                                     "mean": round(statistics.mean(gaps), 4)},
                          "wall_s": round((stamps[-1] - stamps[0]) / 1e9, 3),
                          "ioctl_share_of_wall": round(sum(f["ioctl_ns"] for f in frames) / (stamps[-1] - stamps[0]) * 100, 1)}
    return summary
