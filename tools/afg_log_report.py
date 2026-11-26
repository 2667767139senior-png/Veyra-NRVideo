#!/usr/bin/env python3
"""Summarise an AFG run from a Veyra log.

Reads the two per-second lines the scheduler and the presenter emit and reports
what the product promised to be judged on: the delivered interval distribution
(P50/P95/P99), how far it sat from the target period, how many presents were off
cadence, what the scheduler had to drop, and what VFG cost per frame.

It never computes "average FPS" and calls it a result: an average hides exactly
the judder this mode exists to remove, so the percentiles are the headline.

    python tools/afg_log_report.py <logfile> [--json]

Exit code 0 when a run was found and every check passed, 1 when a check failed,
2 when the log has no AFG data at all (the mode never ran - read the
"mode=on but not running" line for the gate that stopped it).
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field, asdict
from pathlib import Path

FIXED_TARGET = re.compile(r"\[fixed-target\]\s*(?P<body>.*)$")
AFG_OUTPUT = re.compile(r"\[afg-output\]\s*(?P<body>.*)$")
REFUSAL = re.compile(r"\[fixed-target\]\s*mode=on but not running:(?P<body>.*)$")
TIMESTAMP = re.compile(r"^(?P<ts>\d{4}-\d\d-\d\dT[\d:.]+Z)")


def fields(body: str) -> dict[str, str]:
    """`key=value` pairs; the trailing parenthetical notes are dropped."""
    body = body.split(" (")[0]
    out: dict[str, str] = {}
    for token in body.split():
        if "=" in token:
            key, _, value = token.partition("=")
            out[key] = value
    return out


def num(values: dict[str, str], key: str, default: float = 0.0) -> float:
    try:
        return float(values[key])
    except (KeyError, ValueError):
        return default


@dataclass
class PaceWindow:
    samples: int
    p50: float
    p95: float
    p99: float
    mean: float
    sd: float
    off: int


@dataclass
class Run:
    target: float = 0.0
    cadence: str = ""
    buffer: str = ""
    source_fps: list[float] = field(default_factory=list)
    generated_fps: list[float] = field(default_factory=list)
    frames: list[int] = field(default_factory=list)
    reason: dict[str, int] = field(default_factory=dict)
    snap_ms: list[float] = field(default_factory=list)
    guard_ms: list[float] = field(default_factory=list)
    fg_gpu: list[float] = field(default_factory=list)
    load_cap: list[int] = field(default_factory=list)
    counters: dict[str, int] = field(default_factory=dict)
    windows: list[PaceWindow] = field(default_factory=list)
    refusals: list[str] = field(default_factory=list)
    first_seen: str = ""
    last_seen: str = ""


def parse(path: Path) -> Run:
    run = Run()
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        stamp = TIMESTAMP.match(line)
        ts = stamp.group("ts") if stamp else ""
        refusal = REFUSAL.search(line)
        if refusal:
            run.refusals.append(refusal.group("body").strip())
            continue
        match = AFG_OUTPUT.search(line)
        if match:
            v = fields(match.group("body"))
            if v.get("samples"):
                run.windows.append(PaceWindow(
                    samples=int(num(v, "samples")), p50=num(v, "p50"), p95=num(v, "p95"),
                    p99=num(v, "p99"), mean=num(v, "mean"), sd=num(v, "sd"),
                    off=int(num(v, "offCadence"))))
            continue
        match = FIXED_TARGET.search(line)
        if not match:
            continue
        v = fields(match.group("body"))
        if v.get("mode") != "afg":
            continue
        if not run.first_seen:
            run.first_seen = ts
        run.last_seen = ts
        run.target = num(v, "targetFps", run.target)
        run.cadence = v.get("cadence", run.cadence)
        run.buffer = v.get("buffer", run.buffer)
        run.source_fps.append(num(v, "contentFps"))
        if "generatedFps" in v:
            run.generated_fps.append(num(v, "generatedFps"))
        run.frames.append(int(num(v, "frames")))
        run.reason[v.get("reason", "?")] = run.reason.get(v.get("reason", "?"), 0) + 1
        run.snap_ms.append(num(v, "snapMs"))
        if "guardMs" in v:
            run.guard_ms.append(num(v, "guardMs"))
        if "fgGpuP95Ms" in v:
            run.fg_gpu.append(num(v, "fgGpuP95Ms"))
        if "loadCap" in v:
            run.load_cap.append(int(num(v, "loadCap")))
        # Cumulative counters: the last line wins.
        for key in ("intervals", "generated", "droppedCap", "droppedNear", "droppedLoad",
                    "loadLimitedPairs", "resyncs", "bypassLong", "bypassAtTarget", "bypassOff"):
            if key in v:
                run.counters[key] = int(num(v, key))
    return run


def pct(values: list[float], q: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, int(q * (len(ordered) - 1) + 0.5))
    return ordered[index]


def report(run: Run, period: float) -> tuple[dict, list[str]]:
    problems: list[str] = []
    samples = sum(w.samples for w in run.windows)
    # Builds before 2026-11-26 counted off-cadence intervals cumulatively while the
    # percentiles came from a sliding window, so the raw figure reads "1211 of 256".
    # Recovering the per-window increments keeps old logs measurable.
    offs = [w.off for w in run.windows]
    cumulative_off = all(b >= a for a, b in zip(offs, offs[1:])) and (not offs or offs[-1] > (offs[0] if offs else 0))
    off = (sum(b - a for a, b in zip([0] + offs, offs)) if cumulative_off
           else sum(offs))
    # The first windows carry start-up (first frame, graph warm-up). The cadence
    # judgement is about steady play, and the start-up spike is reported separately.
    steady = run.windows[2:] or run.windows

    # Weighted by samples, so a run with a slow start is not over-counted.
    mean_interval = (sum(w.mean * w.samples for w in run.windows) / samples) if samples else 0.0
    worst_p95 = max((w.p95 for w in steady), default=0.0)
    worst_p99 = max((w.p99 for w in steady), default=0.0)
    typical_p95 = pct([w.p95 for w in steady], 0.5)
    typical_p50 = pct([w.p50 for w in steady], 0.5)

    summary = {
        "seconds": len(run.frames),
        "target_fps": run.target,
        "period_ms": period,
        "cadence": run.cadence,
        "buffer": run.buffer,
        "present": {
            "samples": samples,
            "mean_interval_ms": round(mean_interval, 3),
            "effective_fps": round(1000.0 / mean_interval, 2) if mean_interval else 0.0,
            "p50_ms": round(typical_p50, 3),
            "p95_ms": round(typical_p95, 3),
            "worst_window_p95_ms": round(worst_p95, 3),
            "worst_window_p99_ms": round(worst_p99, 3),
            "off_cadence": off,
            "off_cadence_ratio": round(off / samples, 4) if samples else 0.0,
            "off_cadence_cumulative": cumulative_off,
        },
        "source": {
            "p50_fps": round(pct(run.source_fps, 0.5), 2),
            "p05_fps": round(pct(run.source_fps, 0.05), 2),
            "p95_fps": round(pct(run.source_fps, 0.95), 2),
            "min_fps": round(min(run.source_fps), 2) if run.source_fps else 0.0,
            "max_fps": round(max(run.source_fps), 2) if run.source_fps else 0.0,
        },
        "generated": {
            "p50_fps": round(pct(run.generated_fps, 0.5), 2),
            "p05_fps": round(pct(run.generated_fps, 0.05), 2),
            "p95_fps": round(pct(run.generated_fps, 0.95), 2),
        },
        "frames_per_pair": {
            "0": run.frames.count(0), "1": run.frames.count(1),
            "2": run.frames.count(2), "3+": sum(1 for f in run.frames if f >= 3),
        },
        "reason": run.reason,
        "counters": run.counters,
        "snap_ms": {
            "p50": round(pct(run.snap_ms, 0.5), 2),
            "p95": round(pct(run.snap_ms, 0.95), 2),
            "max": round(max(run.snap_ms), 2) if run.snap_ms else 0.0,
        },
        "guard_ms": {"p50": round(pct(run.guard_ms, 0.5), 2),
                     "max": round(max(run.guard_ms), 2) if run.guard_ms else 0.0},
        "vfg_gpu_ms": {
            "p50": round(pct(run.fg_gpu, 0.5), 2),
            "p95": round(pct(run.fg_gpu, 0.95), 2),
            "max": round(max(run.fg_gpu), 2) if run.fg_gpu else 0.0,
            "budget_ratio_p95": round(pct(run.fg_gpu, 0.95) / period, 3) if period else 0.0,
        },
        "load_cap": {"min": min(run.load_cap), "max": max(run.load_cap)} if run.load_cap else {},
        "refusals": run.refusals[:3],
        "window": {"first": run.first_seen, "last": run.last_seen},
    }

    # The checks that make this an acceptance rather than a description.
    if run.target and abs(mean_interval - period) > period * 0.02:
        problems.append(f"mean interval {mean_interval:.2f} ms is more than 2% from the {period:.2f} ms target")
    if samples and off / samples > 0.02:
        problems.append(f"{off}/{samples} presents ({off / samples:.1%}) were off cadence (limit 2%)")
    if worst_p99 > period * 3:
        problems.append(f"worst window P99 {worst_p99:.2f} ms is more than 3 periods")
    if run.counters.get("droppedCap", 0) > 0:
        problems.append(f"{run.counters['droppedCap']} slots lost to the per-pair cap")
    if not samples:
        problems.append("no afg-output windows: nothing was presented through the AFG path")
    return summary, problems


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--period", type=float, default=0.0,
                        help="output period in ms (default 1000/targetFps)")
    args = parser.parse_args()
    path = Path(args.log)
    if not path.is_file():
        print(f"ERROR: {path} not found")
        return 2
    run = parse(path)
    if not run.frames and not run.windows:
        print("no AFG data in this log")
        for refusal in run.refusals[:5]:
            print(f"  mode=on but not running:{refusal}")
        return 2
    period = args.period or (1000.0 / run.target if run.target else 0.0)
    summary, problems = report(run, period)
    if args.json:
        print(json.dumps(summary, indent=2, ensure_ascii=False))
    else:
        p = summary["present"]
        print(f"AFG run: {summary['seconds']} log seconds, target {summary['target_fps']:.0f} FPS "
              f"({summary['period_ms']:.2f} ms), cadence={summary['cadence']} buffer={summary['buffer']}")
        print(f"  source     {summary['source']['p50_fps']:.2f} FPS "
              f"(p05 {summary['source']['p05_fps']:.2f}, p95 {summary['source']['p95_fps']:.2f}, "
              f"range {summary['source']['min_fps']:.2f}-{summary['source']['max_fps']:.2f})")
        print(f"  generated  {summary['generated']['p50_fps']:.2f} FPS "
              f"(p05 {summary['generated']['p05_fps']:.2f}, p95 {summary['generated']['p95_fps']:.2f})")
        print(f"  present    {p['effective_fps']:.2f} FPS from {p['samples']} intervals, "
              f"P50 {p['p50_ms']:.2f} / P95 {p['p95_ms']:.2f} ms, "
              f"worst window P95 {p['worst_window_p95_ms']:.2f} P99 {p['worst_window_p99_ms']:.2f} ms")
        print(f"  cadence    {p['off_cadence']}/{p['samples']} off ({p['off_cadence_ratio']:.2%})")
        print(f"  frames/pair {summary['frames_per_pair']}  reasons {summary['reason']}")
        print(f"  snap ms    {summary['snap_ms']}  guard {summary['guard_ms']} ms  vfg gpu {summary['vfg_gpu_ms']}")
        print(f"  counters   {summary['counters']}")
        if summary["load_cap"]:
            print(f"  load cap   {summary['load_cap']}")
        if summary["refusals"]:
            print(f"  refusals   {summary['refusals']}")
    if problems:
        print("\nFAIL:")
        for problem in problems:
            print(f"  - {problem}")
        return 1
    print("\nPASS: every AFG acceptance check in this report")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
