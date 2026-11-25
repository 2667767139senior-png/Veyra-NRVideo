#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fixed-target frame generation: offline check of the scheduling maths.

A literal port of include/veyra/engine/FixedTargetSchedule.h, plus a plain
multiplier mode for comparison, so the cadence claim can be checked without a
GPU, a capture card or a Veyra build. Run it after changing the header:

    python tools/fixed_target_sim.py

The model is the adaptive one: each real interval asks for its own length worth
of output frames at the target rate, the rounding remainder (carry) goes to the
next pair, and VFG then generates count in-between frames at i/(count+1).

Exit code 0 means every scenario's long-run output rate matched its target and
the safety rules (cap, stall, source already at target) held everywhere in the
swept matrix.
"""
from __future__ import annotations

import itertools
import math
import sys

UNIT = 10_000_000  # 100 ns per second
MAX_TICKS = 6


def ms(v: float) -> int:
    return int(round(v * 10000.0))


def to_ms(v: int) -> float:
    return v / 10000.0


def llround(x: float) -> int:
    """std::llround: half away from zero, unlike Python's banker's rounding."""
    return math.floor(x + 0.5) if x >= 0 else math.ceil(x - 0.5)


class Config:
    def __init__(self, target_fps=50.0, max_runs=2, smoothing=0.0,
                 clamp_min_ms=25.0, clamp_max_ms=60.0, max_interval_ms=100.0):
        self.enabled = True
        self.target_fps = target_fps
        self.max_runs = max_runs
        self.smoothing = smoothing
        self.clamp_min_ms = clamp_min_ms
        self.clamp_max_ms = clamp_max_ms
        self.max_interval_ms = max_interval_ms


class Plan:
    def __init__(self):
        self.count = 0
        self.multiplier = 1          # count+1: 1 means "generate nothing"
        self.t: list[float] = []
        self.bypass = False
        self.reason = "off"


def same_structure(a: Config, b: Config) -> bool:
    """Port of engine::fixedTargetSameStructure: only these two shape the graph."""
    return a.enabled == b.enabled and a.max_runs == b.max_runs


def capacity(cfg: Config, requested: int) -> int:
    """Port of engine::fixedTargetCapacity: generated slots a pair may need."""
    if not cfg.enabled:
        return requested
    return max(requested, min(cfg.max_runs, MAX_TICKS) + 1)


def runs_for(target_fps: float) -> int:
    """Port of engine::fixedTargetRunsFor: frames a target rate needs at 40 ms."""
    if not (target_fps > 0.0) or not math.isfinite(target_fps):
        return 4
    return max(2, min(MAX_TICKS, math.ceil(target_fps / 25.0)))


class Scheduler:
    def __init__(self, cfg: Config):
        self.cfg = cfg
        self.carry = 0.0
        self.smoothed = 0.0
        self.stats = {"intervals": 0, "generated": 0, "dropped": 0,
                      "long": 0, "at_target": 0, "invalid": 0, "off": 0}

    def plan(self, a: int, b: int, content_fps: float = 30.0) -> Plan:
        out = Plan()
        c = self.cfg
        if not c.enabled:
            return out
        if not (c.target_fps > 0.0):
            self.stats["off"] += 1
            out.bypass, out.reason = True, "targetOff"
            self.carry = 0.0
            return out
        span = b - a
        if span <= 0:
            self.stats["invalid"] += 1
            out.bypass, out.reason = True, "interval"
            self.carry = 0.0
            return out
        self.stats["intervals"] += 1
        span_ms = to_ms(span)
        if span_ms > c.max_interval_ms:
            self.stats["long"] += 1
            out.bypass, out.reason = True, "longInterval"
            self.carry = 0.0
            self.smoothed = 0.0
            return out
        if math.isfinite(content_fps) and content_fps >= c.target_fps * 0.98:
            self.stats["at_target"] += 1
            out.bypass, out.reason = True, "sourceAtTarget"
            self.carry = 0.0
            self.smoothed = 0.0
            return out
        used = span_ms
        if c.smoothing > 0.0:
            low, high = min(c.clamp_min_ms, c.clamp_max_ms), max(c.clamp_min_ms, c.clamp_max_ms)
            clamped = max(low, min(high, span_ms))
            self.smoothed = clamped if self.smoothed <= 0.0 else self.smoothed + (clamped - self.smoothed) * min(1.0, max(0.0, c.smoothing))
            used = self.smoothed
        want = used * c.target_fps / 1000.0 + self.carry
        count = llround(want) - 1
        count = max(0, count)
        if count > c.max_runs:
            self.stats["dropped"] += count - c.max_runs
            count = c.max_runs
            self.carry = 0.0
        else:
            self.carry = max(-1.0, min(1.0, want - (count + 1)))
        out.count = count
        out.multiplier = count + 1
        out.t = [(i + 1) / out.multiplier for i in range(count)]
        out.reason = "adaptive" if count else "none"
        self.stats["generated"] += count
        return out


def run(cfg: Config, spans_ms, content_fps=30.0, start_ms=1000.0):
    s = Scheduler(cfg)
    a = ms(start_ms)
    plans, total = [], 0.0
    for d in spans_ms:
        b = a + ms(d)
        plans.append(s.plan(a, b, content_fps))
        a = b
        total += d
    return s, plans, total


def observed_fps(plans, total_ms: float) -> float:
    frames = sum(p.count + (0 if p.bypass else 1) for p in plans)
    return frames / (total_ms / 1000.0)


def multiplier_mode(spans_ms, m: int = 2) -> float:
    """Output rate a constant multiplier would have produced on the same cadence."""
    return len(spans_ms) * m / (sum(spans_ms) / 1000.0)


def main() -> int:
    fails = 0

    def check(ok: bool, label: str) -> None:
        nonlocal fails
        print(f"  {'PASS' if ok else 'FAIL'}  {label}")
        fails += 0 if ok else 1

    print("single scenarios (target 80 FPS, maxRuns 4)")

    # 30 FPS source: 33.33 ms at 80 FPS asks for 2.67 frames.
    cfg = Config(80, 4)
    spans = [1000.0 / 30.0] * 60
    s, plans, total = run(cfg, spans)
    fps = observed_fps(plans, total)
    counts = sorted({p.count for p in plans})
    check(math.isclose(fps, 80.0, abs_tol=0.5), f"30->80: {fps:.2f} FPS out (target 80)")
    check(counts == [1, 2], f"30->80: generated frames per pair stay in {counts}")
    uniform = all(abs(p.t[i] - (i + 1) / p.multiplier) < 1e-12
                  for p in plans for i in range(p.count))
    check(uniform, "30->80: positions are i/(count+1), VFG's own t = i/M")
    check(s.stats["dropped"] == 0, "30->80: nothing dropped")

    # 30 FPS -> 50 FPS, the shipped default: 33.33 ms asks for 1.67 frames, so the
    # count walks 1,0,1,1,0,1. This is the only target in the panel that reaches
    # count = 0 on ordinary content, so it is the case that proves the
    # "generate nothing for this pair" path the graph turns into fgCalls = 0.
    cfg50 = Config(50, 2)
    s50, plans50, total50 = run(cfg50, [1000.0 / 30.0] * 60)
    fps50 = observed_fps(plans50, total50)
    empty = sum(1 for p in plans50 if p.count == 0)
    check(math.isclose(fps50, 50.0, abs_tol=0.5),
          f"30->50: {fps50:.2f} FPS out (target 50)")
    check({p.count for p in plans50} <= {0, 1},
          "30->50: generated frames per pair are 0 or 1")
    check(0 < empty < len(plans50), f"30->50: {empty}/60 pairs generate nothing")
    check(all(p.multiplier == p.count + 1 for p in plans50),
          "30->50: group width is count+1")
    check(all(abs(p.t[i] - 0.5) < 1e-12 for p in plans50 for i in range(p.count)),
          "30->50: a generated frame sits exactly halfway")
    check(s50.stats["dropped"] == 0, "30->50: the cap never bites at two runs")

    # The wobble the mode exists for: 30 FPS content dipping to 25 FPS.
    spans = [(40.0 if i % 2 else 1000.0 / 30.0) for i in range(60)]
    s, plans, total = run(cfg, spans)
    fps = observed_fps(plans, total)
    forced = multiplier_mode(spans, 2)
    check(math.isclose(fps, 80.0, abs_tol=0.6),
          f"25/30 wobble: {fps:.2f} FPS out (target 80)")
    check(abs(fps - 80.0) < abs(forced - 80.0),
          f"25/30 wobble: closer to target than a constant 2X ({forced:.2f} FPS)")
    long_counts = [p.count for i, p in enumerate(plans) if i % 2]
    short_counts = [p.count for i, p in enumerate(plans) if not i % 2]
    check(sum(long_counts) > sum(short_counts), "25/30 wobble: long intervals get the extra frame")

    # 40 ms at 80 FPS is 3.2: the carried remainder has to show up as 2/2/3/2/2.
    s, plans, _ = run(cfg, [40.0] * 5)
    got = [p.count for p in plans]
    check(sum(got) == 11, f"40 ms x5: generated {got} -> 16 output frames (80 FPS)")

    # The cap drops frames and forgets the debt.
    cfg160 = Config(160, runs_for(160))
    s, plans, total = run(cfg160, [50.0] * 30, content_fps=20.0)
    check(all(p.count <= cfg160.max_runs for p in plans), "20->160: no pair exceeds maxRuns")
    check(s.stats["dropped"] > 0, f"20->160: {s.stats['dropped']} frames dropped (expected)")
    check(abs(s.carry) < 1e-12, "20->160: the cap leaves no debt behind")

    # A stall is a bypass with the remainder forgotten.
    s = Scheduler(Config(80, 4))
    s.plan(ms(1000), ms(1000) + ms(33.34))
    out = s.plan(ms(2000), ms(2120))
    check(out.bypass and out.count == 0 and out.reason == "longInterval",
          "120 ms gap: bypass, no frames, remainder cleared")
    check(abs(s.carry) < 1e-12, "120 ms gap: no debt carried across the stall")

    # Source already at the target.
    s = Scheduler(Config(60, 4))
    out = s.plan(ms(1000), ms(1016), content_fps=60.0)
    check(out.bypass and out.reason == "sourceAtTarget", "60 FPS source at 60 target: auto bypass")

    # Zero target: the panel's "insert nothing".
    s = Scheduler(Config(0, 4))
    out = s.plan(ms(1000), ms(1040), content_fps=25.0)
    check(out.bypass and out.reason == "targetOff", "target 0: bypass, no division")

    # Smoothing damps the reaction to one short interval.
    spans = [40.0, 25.0, 25.0, 40.0]
    _, a, _ = run(Config(80, 4), spans)
    _, b, _ = run(Config(80, 4, smoothing=0.6, clamp_min_ms=30, clamp_max_ms=45), spans)
    check(a[1].count < b[1].count, f"smoothing: {b[1].count} vs {a[1].count} frames after a 25 ms pair")

    print("\nmatrix sweep (source 20-60 FPS x target 60-160 FPS, steady and 25% wobble)")
    # Two kinds of case: the ones the cap can serve, where the average output rate
    # has to land on the target, and the ones whose longest interval needs more than
    # maxRuns + 1 frames. The second kind is a physical limit of the group width -
    # VFG tops out at 8 frames per pair - and the scheduler answers it by dropping
    # frames and forgetting the debt, never by owing them as latency.
    worst_rate, worst_case, capped = 0.0, "", []
    for source, target, pattern in itertools.product(
            (20.0, 25.0, 30.0, 40.0, 50.0, 60.0),
            (60.0, 72.0, 80.0, 100.0, 120.0, 144.0, 160.0),
            ("steady", "wobble")):
        if target <= source:
            continue
        base = 1000.0 / source
        spans = ([base] * 120 if pattern == "steady"
                 else [base * (1.0 + 0.25 * (1 if i % 2 else -1)) for i in range(120)])
        cfg = Config(target, runs_for(target))
        s, plans, total = run(cfg, spans, content_fps=source)
        fps = observed_fps(plans, total)
        label = f"{source:.0f}->{target:.0f} {pattern}"
        over = fps > target * 1.02
        if target * (max(spans) / 1000.0) > cfg.max_runs + 1 + 1e-9:
            capped.append(f"{label} ({fps:.1f} FPS)")
            if over:
                check(False, f"{label}: {fps:.2f} FPS overshoots the target")
            continue
        err = abs(fps - target) / target
        if err > worst_rate:
            worst_rate, worst_case = err, f"{label} ({fps:.2f})"
        ok = (err < 0.02 and not over
              and all(p.count <= cfg.max_runs for p in plans)
              and all(p.multiplier <= capacity(cfg, 2) for p in plans))
        if not ok:
            check(False, f"{label}: {fps:.2f} FPS ({err*100:.2f}% off)")
    check(worst_rate < 0.02,
          f"every case inside the cap lands within 2% of target (worst {worst_case}, {worst_rate*100:.2f}%)")
    print(f"  note: {len(capped)} case(s) need more than maxRuns+1 frames per interval "
          f"(VFG tops out at 8 per pair): {', '.join(capped[:4])}{' ...' if len(capped) > 4 else ''}")

    print("\ncapacity")
    check(capacity(Config(80, 4), 2) == 5, "capacity covers maxRuns + the real frame")
    check(capacity(Config(120, 5), 2) == 6, "120 FPS asks for one more slot")
    check(capacity(Config(160, 6), 8) == 8, "a larger requested multiplier is kept")
    check(capacity(Config(80, 4), 2) <= 2 * (8 - 1), "capacity fits the generated pool (FrameBatch=8)")

    if fails == 0:
        print("\nall fixed-target simulations passed")
    else:
        print(f"\n{fails} fixed-target simulation(s) failed")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
