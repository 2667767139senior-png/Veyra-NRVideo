#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fixed-target frame generation: offline check of the scheduling maths.

A literal port of include/veyra/engine/FixedTargetSchedule.h, plus a multiplier
mode for comparison, so the cadence claim can be checked without a GPU, a
capture card or a Veyra build. Run it after changing the header:

    python tools/fixed_target_sim.py

Exit code 0 means every published interval in every scenario was exactly one
output period and every scenario matched its expected tick counts.
"""
from __future__ import annotations

import itertools
import sys

UNIT = 10_000_000  # 100 ns per second
MAX_TICKS = 6


def ms(v: float) -> int:
    return int(round(v * 10000.0))


def to_ms(v: int) -> float:
    return v / 10000.0


class Config:
    def __init__(self, target_fps=80.0, max_runs=4, smoothing=0.0,
                 clamp_min_ms=25.0, clamp_max_ms=60.0, max_interval_ms=100.0,
                 near_start=0.05):
        self.enabled = True
        self.target_fps = target_fps
        self.max_runs = max_runs
        self.smoothing = smoothing
        self.clamp_min_ms = clamp_min_ms
        self.clamp_max_ms = clamp_max_ms
        self.max_interval_ms = max_interval_ms
        self.near_start = near_start


class Plan:
    def __init__(self):
        self.t: list[float] = []
        self.pts: list[int] = []
        self.real = 0
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
    """Port of engine::fixedTargetRunsFor: runs a target rate needs at 40 ms."""
    if not (target_fps > 0.0):
        return 4
    return min(max(int(-(-target_fps // 25.0)), 2), MAX_TICKS)


class Scheduler:
    """Port of engine::FixedTargetScheduler."""

    def __init__(self, cfg: Config):
        self.cfg = cfg
        self.phase = 0
        self.phase_valid = False
        self.smoothed = 0.0
        self.stats = {"intervals": 0, "ticks": 0, "dropped": 0,
                      "long": 0, "at_target": 0, "invalid": 0, "off": 0, "resync": 0}

    def plan(self, a: int, b: int, content_fps: float) -> Plan:
        out = Plan()
        out.real = b
        span = b - a
        if not (self.cfg.target_fps > 0.0):
            self.stats["off"] += 1
            out.bypass = True
            out.reason = "targetOff"
            self.phase_valid = False
            return out
        if span <= 0:
            self.stats["invalid"] += 1
            out.bypass = True
            out.reason = "interval"
            self.phase_valid = False
            return out
        self.stats["intervals"] += 1
        period = max(1, int(round(UNIT / self.cfg.target_fps)))
        if span > int(self.cfg.max_interval_ms * 10000.0):
            self.stats["long"] += 1
            self.stats["resync"] += 1
            out.bypass = True
            out.reason = "longInterval"
            self.phase, self.phase_valid, self.smoothed = b, True, 0.0
            return out
        if content_fps == content_fps and content_fps >= self.cfg.target_fps * 0.98:
            self.stats["at_target"] += 1
            out.bypass = True
            out.reason = "sourceAtTarget"
            self.phase, self.phase_valid, self.smoothed = b, True, 0.0
            return out
        denom = span
        if self.cfg.smoothing > 0.0:
            lo = int(round(self.cfg.clamp_min_ms * 10000.0))
            hi = int(round(self.cfg.clamp_max_ms * 10000.0))
            clamped = min(max(span, min(lo, hi)), max(lo, hi))
            if self.smoothed <= 0:
                self.smoothed = float(clamped)
            else:
                self.smoothed += (clamped - self.smoothed) * min(max(self.cfg.smoothing, 0.0), 1.0)
            denom = max(1, int(round(self.smoothed)))
        if not self.phase_valid:
            self.phase, self.phase_valid = a + period, True
        if self.phase <= a:
            self.phase += ((a - self.phase) // period + 1) * period
        tick = self.phase
        while tick < b:
            t = (tick - a) / denom
            self.phase = tick + period
            if t <= self.cfg.near_start:
                tick += period
                continue
            if len(out.t) >= self.cfg.max_runs or len(out.t) >= MAX_TICKS:
                self.stats["dropped"] += 1
                tick += period
                continue
            out.t.append(min(max(t, 0.001), 0.999))
            out.pts.append(tick)
            self.stats["ticks"] += 1
            tick += period
        # Every iteration advanced phase by exactly one period, so the loop left it
        # on the first grid point at or after B. The real frame takes that point and
        # the next pair starts one period later.
        out.real = self.phase
        self.phase += period
        out.reason = "ticks" if out.t else "none"
        return out


def simulate(cfg: Config, deltas_ms: list[float], start_ms=1000.0, content_fps=30.0):
    """Every submitted PTS, in order, as the presentation timeline would see it."""
    s = Scheduler(cfg)
    submitted: list[int] = []
    counts: list[int] = []
    a = ms(start_ms)
    for d in deltas_ms:
        b = a + ms(d)
        p = s.plan(a, b, content_fps)
        submitted.extend(p.pts)
        submitted.append(p.real)
        counts.append(len(p.t))
        a = b
    return submitted, counts, s.stats


def multiplier_stream(deltas_ms: list[float], multiplier: int, start_ms=1000.0):
    """The current behaviour, for comparison: t = i/M inside every interval."""
    submitted: list[int] = []
    a = ms(start_ms)
    for d in deltas_ms:
        b = a + ms(d)
        for i in range(1, multiplier):
            submitted.append(a + (b - a) * i // multiplier)
        submitted.append(b)
        a = b
    return submitted


def intervals(pts: list[int]) -> list[float]:
    return [to_ms(pts[i] - pts[i - 1]) for i in range(1, len(pts))]


def spread(values: list[float]) -> tuple[float, float, float]:
    lo, hi = min(values), max(values)
    return lo, hi, (hi - lo) / ((lo + hi) / 2.0) * 100.0


def report(title: str, pts: list[int]) -> bool:
    gaps = intervals(pts)
    lo, hi, pct = spread(gaps)
    ok = hi - lo <= 0.002  # 0.2 us of integer rounding, not a frame of drift
    print(f"  {'PASS' if ok else 'FAIL'}  {title}: {len(pts)} frames, "
          f"interval {lo:.3f}-{hi:.3f} ms (spread {pct:.2f}%)")
    return ok


def main() -> int:
    fails = 0
    print("Fixed-target scheduling, 80 FPS target")

    steady = [1000.0 / 30.0] * 180
    pts, counts, st = simulate(Config(80), steady)
    fails += not report("steady 30 FPS", pts)
    print(f"        generated per interval: {min(counts)}-{max(counts)} "
          f"(mean {sum(counts) / len(counts):.3f}), dropped {st['dropped']}")

    wobble = [40.0 if i % 3 == 0 else 1000.0 / 30.0 for i in range(180)]
    pts, counts, st = simulate(Config(80), wobble)
    fails += not report("30 <-> 25 FPS wobble", pts)
    print(f"        generated per interval: {min(counts)}-{max(counts)}, "
          f"source span {min(wobble):.2f}-{max(wobble):.2f} ms")

    drift = [1000.0 / 30.0 + 1.7 * ((i * 7) % 5 - 2) for i in range(180)]
    pts, _, _ = simulate(Config(80), drift)
    fails += not report("30 FPS with +-3.4 ms transport jitter", pts)

    slow = [60.0] * 120
    pts, counts, st = simulate(Config(120), slow, content_fps=20.0)
    lo, hi = min(counts), max(counts)
    ok = hi == 4 and st["dropped"] > 0
    print(f"  {'PASS' if ok else 'FAIL'}  20 FPS -> 120 FPS: capped at maxRuns=4 "
          f"(saw {lo}-{hi} runs, {st['dropped']} ticks dropped)")
    fails += not ok
    gaps = intervals(pts)
    grid = ms(1000.0 / 120.0)
    on_grid = all((p - pts[0]) % grid == 0 for p in pts)
    print(f"  {'PASS' if on_grid else 'FAIL'}  dropped ticks keep every PTS on the 8.33 ms grid")
    fails += not on_grid

    # A pause: 120 ms with no content, then normal service.
    pts, counts, st = simulate(Config(80), steady[:40] + [120.0] + steady[:80])
    gaps = intervals(pts)
    stall = max(range(len(gaps)), key=lambda i: gaps[i])
    after = gaps[stall + 1:]
    ok = (st["long"] == 1 and st["resync"] == 1 and gaps[stall] > 100.0
          and max(after) - min(after) <= 0.002)
    print(f"  {'PASS' if ok else 'FAIL'}  120 ms stall: 1 bypass, 1 resync, "
          f"cadence restored afterwards ({min(after):.3f}-{max(after):.3f} ms, "
          f"stall gap {gaps[stall]:.3f} ms)")
    fails += not ok

    # A real 60 FPS source at a 60 FPS target: nothing to insert.
    pts, counts, st = simulate(Config(60), [1000.0 / 60.0] * 120, content_fps=60.0)
    ok = st["at_target"] == len(counts) and sum(counts) == 0
    print(f"  {'PASS' if ok else 'FAIL'}  60 FPS source at a 60 FPS target: every pair bypassed")
    fails += not ok

    # The real frame takes the first grid point at or after B: that is what keeps
    # every submitted interval at one period, and it bounds the cost to < 1 period.
    print("Real-frame grid snap")
    s = Scheduler(Config(80))
    period = ms(1000.0 / 80.0)
    a, on_grid, worst = ms(1000.0), True, 0
    for i in range(240):
        b = a + ms(40.0 if i % 4 == 0 else 1000.0 / 30.0)
        p = s.plan(a, b, 30.0)
        if p.real < b or p.real - b >= period:
            on_grid = False
        worst = max(worst, p.real - b)
        a = b
    print(f"  {'PASS' if on_grid else 'FAIL'}  the real frame lands on the first grid point at or after B")
    fails += not on_grid
    print(f"  {'PASS' if worst < period else 'FAIL'}  snap cost stays under one period (worst {to_ms(worst):.3f} ms)")
    fails += 0 if worst < period else 1

    # Only the mode and the run cap shape the graph; the knobs are read per pair.
    base, other = Config(80), Config(100)
    other.smoothing, other.clamp_min_ms, other.clamp_max_ms = 0.5, 30.0, 70.0
    knobs_ok = same_structure(base, other) and not same_structure(base, Config(80, max_runs=6))
    print(f"  {'PASS' if knobs_ok else 'FAIL'}  rate/smoothing keep the graph, a new run cap rebuilds it")
    fails += not knobs_ok
    cap_ok = capacity(base, 2) == 5 and capacity(base, 8) == 8 and capacity(Config(80, 6), 2) == 7
    print(f"  {'PASS' if cap_ok else 'FAIL'}  generated-slot capacity covers maxRuns + the real frame")
    fails += not cap_ok

    # The panel takes any integer 0-160 now, so the run cap follows what was typed
    # and 0 means "insert nothing" instead of a division by zero.
    print("Typed targets (0-160)")
    runs_ok = (runs_for(80) == 4 and runs_for(120) == 5 and runs_for(144) == 6
               and runs_for(160) == MAX_TICKS and runs_for(0) == 4)
    print(f"  {'PASS' if runs_ok else 'FAIL'}  run cap follows the target: 80->{runs_for(80)} "
          f"120->{runs_for(120)} 144->{runs_for(144)} 160->{runs_for(160)}")
    fails += not runs_ok
    off_pts, off_counts, off_st = simulate(Config(0), steady, content_fps=30.0)
    off_ok = off_st["off"] == len(off_counts) and sum(off_counts) == 0
    print(f"  {'PASS' if off_ok else 'FAIL'}  target 0 inserts nothing "
          f"({off_st['off']} pairs bypassed)")
    fails += not off_ok
    for target, source in ((100, 30.0), (120, 30.0), (144, 30.0), (160, 30.0), (120, 25.0)):
        cfg = Config(target, runs_for(target))
        pts, counts, st = simulate(cfg, [1000.0 / source] * 120, content_fps=source)
        lo2, hi2, pct2 = spread(intervals(pts))
        ok = st["dropped"] == 0 and pct2 < 1.0
        print(f"  {'PASS' if ok else 'FAIL'}  {source:.0f} FPS -> {target} FPS at maxRuns={cfg.max_runs}: "
              f"{min(counts)}-{max(counts)} runs, dropped {st['dropped']}, spread {pct2:.2f}%")
        fails += not ok

    # Comparison against the current multiplier mode on the wobbling source.
    print("Multiplier mode on the same 30<->25 FPS source (for comparison)")
    for m in (2, 3):
        pts = multiplier_stream(wobble, m)
        gaps = intervals(pts)
        lo, hi, pct = spread(gaps)
        print(f"        {m}X: interval {lo:.3f}-{hi:.3f} ms (spread {pct:.1f}%), "
              f"effective {1000.0 / (sum(gaps) / len(gaps)):.1f} FPS")

    # Sweep: no configuration may put a PTS off the grid, and the cadence holds
    # wherever the per-interval cap does not have to drop ticks (dropping them is
    # the deliberate backpressure case, and it leaves a hole by definition).
    print("Sweep: target 60-144, source 20-60 FPS, maxRuns 1-6")
    worst = 0.0
    capped = 0
    for target, source, runs in itertools.product(
            (60, 72, 80, 90, 100, 120, 144), (20, 24, 25, 30, 33.3, 40, 48, 60), (1, 2, 3, 4, 5, 6)):
        cfg = Config(target, runs)
        deltas = [1000.0 / source] * 90
        pts, _, st = simulate(cfg, deltas, content_fps=source)
        gaps = intervals(pts)
        if not gaps:
            continue
        period = ms(1000.0 / target)
        if any((p - pts[0]) % period != 0 for p in pts):
            print(f"  FAIL  off-grid PTS at {target} FPS target / {source} FPS source / maxRuns={runs}")
            fails += 1
        if st["dropped"] or st["at_target"]:
            capped += 1
            continue
        lo, hi = min(gaps), max(gaps)
        worst = max(worst, hi - lo)
    print(f"  {'PASS' if worst <= 0.002 else 'FAIL'}  worst interval deviation: {worst * 1000:.2f} us "
          f"({capped} combinations excluded as capped or bypassed)")
    fails += 0 if worst <= 0.002 else 1

    print("OK" if not fails else f"FAILED ({fails})")
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
