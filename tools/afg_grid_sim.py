"""Offline model of the AFG output grid (include/veyra/engine/FixedTargetSchedule.h).

Same arithmetic as the header, used to explore source/target combinations the
unit test cannot enumerate cheaply. The C++ test is the authority for the fixed
cases; this prints a matrix and flags the combinations where the grid cannot be
kept (a target the slot pool cannot fill).

    python tools/afg_grid_sim.py
"""
import math

UNIT = 10_000_000
MAX_TICKS = 7


def runs_for(fps):
    if not (fps > 0):
        return 4
    return max(2, min(MAX_TICKS, math.ceil(fps / 25.0)))


class Config:
    def __init__(self, enabled=False, target=50.0, cadence="fixed", max_interval=100.0, max_runs=None):
        self.enabled = enabled
        self.target = target
        self.cadence = cadence
        self.max_interval = max_interval
        self.max_runs = max_runs if max_runs is not None else runs_for(target)


class Plan:
    def __init__(self):
        self.count = 0
        self.pts = []
        self.t = []
        self.real = 0
        self.bypass = False
        self.reason = "off"
        self.dropped_cap = 0
        self.dropped_near = 0

    def multiplier(self):
        return self.count + 1


class Scheduler:
    def __init__(self, cfg):
        self.cfg = cfg
        self.anchored = False
        self.origin = 0
        self.last_slot = 0
        self.generated = 0
        self.dropped_cap = 0
        self.last_snap_ms = 0.0

    def plan(self, a, b, content_fps):
        out = Plan()
        c = self.cfg
        if not c.enabled:
            return out
        if not (c.target > 0):
            out.bypass, out.reason = True, "targetOff"
            self.anchored = False
            return out
        span = b - a
        if span <= 0:
            out.bypass, out.reason = True, "interval"
            self.anchored = False
            return out
        if span / 10000.0 > c.max_interval:
            out.bypass, out.reason = True, "longInterval"
            self.anchored = False
            return out
        if math.isfinite(content_fps) and content_fps >= c.target * 0.98:
            out.bypass, out.reason = True, "sourceAtTarget"
            self.anchored = False
            return out
        period = UNIT / c.target
        if not self.anchored:
            self.origin, self.last_slot, self.anchored = a, 0, True
        def slot_time(index):
            return self.origin + int(round(index * period))
        slot_b = int(math.ceil((b - self.origin) / period - 1e-6))
        if slot_b <= self.last_slot:
            slot_b = self.last_slot + 1
        if slot_b < 1:
            slot_b = 1
        pts, steps, found = [], [], 0
        for index in range(self.last_slot + 1, slot_b):
            at = slot_time(index)
            t = (at - a) / span
            if t <= 0.0 or t >= 1.0:
                out.dropped_near += 1
                continue
            t = min(max(t, 0.005), 0.995)
            if found == MAX_TICKS:
                out.dropped_cap += 1
                self.dropped_cap += 1
                continue
            pts.append(at)
            steps.append(t)
            found += 1
        first = 0
        if found > c.max_runs:
            out.dropped_cap += found - c.max_runs
            self.dropped_cap += found - c.max_runs
            first = found - c.max_runs
            found = c.max_runs
        out.count = found
        out.pts = pts[first:first + found]
        out.t = steps[first:first + found]
        if c.cadence == "fixed":
            out.real = slot_time(slot_b)
            self.last_snap_ms = (out.real - b) / 10000.0
        else:
            out.real = 0
            self.last_snap_ms = 0.0
        self.last_slot = slot_b
        out.reason = "grid" if found else "none"
        self.generated += found
        return out


def run(spans_ms, target=50.0, cadence="fixed", start=10_000_000, content_fps=30.0,
        max_runs=None, max_interval=100.0):
    s = Scheduler(Config(True, target, cadence, max_interval, max_runs))
    pairs, a = [], start
    for d in spans_ms:
        b = a + int(d * 10000 + 0.5)
        pairs.append((a, b, s.plan(a, b, content_fps)))
        a = b
    return s, pairs


def presented(pairs):
    out = []
    for a, b, p in pairs:
        for i in range(p.count):
            out.append((p.pts[i], p.t[i]))
        out.append((p.real, None))
    return out


FAIL = []


def check(ok, label):
    print(("PASS " if ok else "FAIL ") + label)
    if not ok:
        FAIL.append(label)


PERIOD = 200000

s, pairs = run([1000 / 30.0] * 300)
gen = sum(p.count for _, _, p in pairs)
empty = sum(1 for _, _, p in pairs if p.count == 0)
check(gen == 200, "30->50: 200 generated in 300 pairs")
check(empty == 100, "30->50: every third pair empty")
origin = pairs[0][0]
exact = all((p.pts[i] - origin) % PERIOD == 0 for _, _, p in pairs for i in range(p.count))
check(exact, "30->50: slots are whole 20 ms steps")
steps_ok = all(abs(p.t[i] - (p.pts[i] - a) / (b - a)) < 1e-12 for a, b, p in pairs for i in range(p.count))
check(steps_ok, "30->50: timestep == (slot-a)/(b-a)")
snap = all(0 <= p.real - b < PERIOD for _, b, p in pairs)
check(snap, "30->50: snap inside one period")
allp = presented(pairs)
even = all(allp[i][0] - allp[i - 1][0] == PERIOD for i in range(1, len(allp)))
check(even, "30->50: every presented interval is 20 ms")
check(len(allp) == 500, "30->50: 500 outputs in 10 s")
mono = all(allp[i][0] > allp[i - 1][0] for i in range(1, len(allp)))
check(mono, "30->50: strictly increasing")

s, pairs = run([40.0] * 40, content_fps=25.0)
check(all(p.count == 1 for _, _, p in pairs), "25->50: one frame per pair")
check(all(abs(p.t[0] - 0.5) < 1e-12 for _, _, p in pairs), "25->50: t=0.5")
check(all(p.real == b for _, b, p in pairs), "25->50: real frame not delayed")

spans = [40.0 if i % 3 == 0 else 1000 / 30.0 for i in range(120)]
s, pairs = run(spans, content_fps=27.0)
allp = presented(pairs)
check(all(allp[i][0] - allp[i - 1][0] == PERIOD for i in range(1, len(allp))),
      "25/30 jitter: still exactly 20 ms")
counts = {}
for _, _, p in pairs:
    counts[p.count] = counts.get(p.count, 0) + 1
check(counts.get(0, 0) > 0 and counts.get(1, 0) > 0, "jitter: counts vary")

s, pairs = run([33.3, 33.3, 33.3, 120.0, 33.3, 33.3, 33.3])
check(pairs[3][2].reason == "longInterval", "stall: bypassed")
check(pairs[4][2].count <= 2, "stall: next pair not charged")

s, pairs = run([66.7] * 20, target=160.0, content_fps=15.0, max_runs=runs_for(160.0))
check(all(p.count <= 7 for _, _, p in pairs), "cap: within slot pool")
check(s.dropped_cap > 0, "cap: overflow counted")

s, pairs = run([1000 / 29.97] * 9000, content_fps=29.97)
check(all((p.pts[i] - pairs[0][0]) % PERIOD == 0 for _, _, p in pairs for i in range(p.count)),
      "no drift over 5 min")
expected = 9000 * (1000 / 29.97) / 1000 * 50
actual = s.generated + 9000
check(abs(actual - expected) / expected < 0.01, f"rate within 1% ({actual:.0f} vs {expected:.0f})")

s, pairs = run([1000 / 30.0] * 60, cadence="latency")
check(all(p.real == 0 for _, _, p in pairs), "latency: real not retimed")

print()
print("target x source matrix (fixed cadence, one minute each):")
print("  target  source   outFps  genPerPair  droppedCap  offGrid")
for target in (40, 50, 60, 80, 100, 120, 144, 160):
    for source in (20, 25, 30, 40, 50, 60):
        if source >= target * 0.98:
            continue
        spans = [1000.0 / source] * (int(source * 60))
        s2, pairs2 = run(spans, target=target, content_fps=source)
        out = presented(pairs2)
        period = 1000.0 / target * 10000
        off = sum(1 for i in range(1, len(out)) if abs((out[i][0] - out[i - 1][0]) - period) > 1.5)
        fps = len(out) / 60.0
        gen = sum(p.count for _, _, p in pairs2) / max(1, len(pairs2))
        print(f"  {target:6d}  {source:6d}  {fps:6.2f}  {gen:10.2f}  {s2.dropped_cap:10d}  {off:7d}")

print()
print("FAILURES:", len(FAIL))
