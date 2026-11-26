"""Mirror of the C++ unit test, used to verify its expectations before spending a
CI build: it re-implements include/veyra/engine/FixedTargetSchedule.h line for
line and asserts exactly the same 55 properties as
tests/unit/FixedTargetScheduleTests.cpp.

The C++ test is the authority. This exists because the build machine is remote,
and a wrong expected value costs a full build to discover.

    python tools/afg_test_mirror.py     # exits non-zero on any failure

Re-sync it whenever plan() changes. tools/afg_grid_sim.py answers a different
question (target x source matrix); neither replaces the C++ test.
"""
import math

UNIT = 10_000_000
MAX_TICKS = 7


def runs_for(fps):
    if not (fps > 0.0) or not math.isfinite(fps):
        return 4
    return max(2, min(MAX_TICKS, math.ceil(fps / 25.0)))


def policy_cap(policy, rate_cap):
    return min(rate_cap, 2) if policy == "tight" else rate_cap


def stall_ms(policy):
    return 60.0 if policy == "tight" else 250.0 if policy == "relaxed" else 100.0


class Config:
    def __init__(self, enabled=False, target=50.0, cadence="fixed",
                 max_interval=100.0, max_runs=None, buffer="balanced", gpu_adapt=True):
        self.enabled = enabled
        self.target = target
        self.cadence = cadence
        self.max_interval = max_interval
        self.buffer = buffer
        self.gpu_adapt = gpu_adapt
        self.max_runs = policy_cap(buffer, max_runs if max_runs is not None else runs_for(target))


def valid(c):
    if not c.enabled:
        return True
    return (math.isfinite(c.target) and 0.0 <= c.target <= 480.0
            and math.isfinite(c.max_interval) and 20.0 <= c.max_interval <= 1000.0
            and 1 <= c.max_runs <= MAX_TICKS)


def capacity(c, requested):
    if not c.enabled:
        return requested
    return max(requested, min(c.max_runs, MAX_TICKS) + 1)


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
        self.dropped_load = 0


class Scheduler:
    def __init__(self, cfg):
        self.cfg = cfg
        self.anchored = False
        self.origin = 0
        self.last_slot = 0
        self.generated = 0
        self.dropped_cap = 0
        self.dropped_near = 0
        self.last_snap_ms = 0.0
        self.resyncs = 0
        self.load_cap = 0
        self.dropped_load = 0
        self.load_limited_pairs = 0

    def set_load_cap(self, cap):
        self.load_cap = cap

    def effective_max_runs(self):
        if not self.cfg.gpu_adapt or not self.load_cap:
            return self.cfg.max_runs
        return min(self.load_cap, self.cfg.max_runs)

    def plan(self, a, b, content_fps):
        out = Plan()
        c = self.cfg
        if not c.enabled:
            out.bypass, out.reason = True, "off"
            return out
        if not (c.target > 0.0):
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
            self.resyncs += 1

        def slot_time(index):
            return self.origin + int(round(index * period))

        slot_b = int(math.ceil((b - self.origin) / period - 1e-6))
        if slot_b <= self.last_slot:
            slot_b = self.last_slot + 1
        if slot_b < 1:
            slot_b = 1
        min_t = 0.005
        pts, steps, found = [], [], 0
        for index in range(self.last_slot + 1, slot_b):
            at = slot_time(index)
            t = (at - a) / span
            if t <= 0.0 or t >= 1.0:
                out.dropped_near += 1
                self.dropped_near += 1
                continue
            t = min(max(t, min_t), 1.0 - min_t)
            if found == MAX_TICKS:
                out.dropped_cap += 1
                self.dropped_cap += 1
                continue
            pts.append(at)
            steps.append(t)
            found += 1
        first = 0
        limit = self.effective_max_runs()
        if found > limit:
            excess = found - limit
            if limit < c.max_runs:
                out.dropped_load += excess
                self.dropped_load += excess
                self.load_limited_pairs += 1
            else:
                out.dropped_cap += excess
                self.dropped_cap += excess
            first = found - limit
            found = limit
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


def ms(v):
    return int(v * 10000.0 + 0.5)


def run(spans_ms, target=50.0, cadence="fixed", start=ms(1000.0),
        content_fps=30.0, max_runs=None, max_interval=100.0):
    s = Scheduler(Config(True, target, cadence, max_interval, max_runs))
    pairs, a = [], start
    for d in spans_ms:
        b = a + ms(d)
        pairs.append((a, b, s.plan(a, b, content_fps)))
        a = b
    return s, pairs


def presented(pairs):
    out = []
    for a, b, p in pairs:
        for i in range(p.count):
            out.append(p.pts[i])
        out.append(p.real)
    return out


FAIL = []


def check(ok, label):
    print(("PASS  " if ok else "FAIL  ") + label)
    if not ok:
        FAIL.append(label)


PERIOD = ms(20.0)   # 200000

# --- 30 -> 50 ---------------------------------------------------------------
s, pairs = run([1000.0 / 30.0] * 300)
gen = sum(p.count for _, _, p in pairs)
empty = sum(1 for _, _, p in pairs if p.count == 0)
check(gen == 200, f"30->50: 300 pairs generate 200 frames (got {gen})")
check(empty == 100, f"30->50: every third pair generates nothing (got {empty})")
origin = pairs[0][0]
check(all((p.pts[i] - origin) % PERIOD == 0 for _, _, p in pairs for i in range(p.count)),
      "30->50: every slot is a whole 20 ms step from the origin")
steps_ok = True
for a, b, p in pairs:
    for i in range(p.count):
        exp = (p.pts[i] - a) / (b - a)
        if not (0.0 < p.t[i] < 1.0) or abs(p.t[i] - exp) > 1e-9:
            steps_ok = False
check(steps_ok, "30->50: timestep == (slot-a)/(b-a), inside (0,1)")
mono = True
prev = -1 << 62
for a, b, p in pairs:
    for i in range(p.count):
        if p.pts[i] <= prev:
            mono = False
        prev = p.pts[i]
    if p.real <= prev:
        mono = False
    prev = p.real
check(mono, "30->50: presentation times strictly increase across pair boundaries")
check(all(0 <= p.real - b < PERIOD for _, b, p in pairs),
      "30->50: real frame never early, never a full period late")
allp = presented(pairs)
check(len(allp) == 500, f"30->50: 500 presented frames in 10 s (got {len(allp)})")
check(all(allp[i] - allp[i - 1] == PERIOD for i in range(1, len(allp))),
      "30->50: presented interval is exactly 20 ms")

# --- 25 -> 50 ---------------------------------------------------------------
s, pairs = run([40.0] * 40, content_fps=25.0)
check(all(p.count == 1 for _, _, p in pairs), "25->50: one generated frame per pair")
check(all(abs(p.t[0] - 0.5) < 1e-9 for _, _, p in pairs), "25->50: t = 0.5")
check(all(p.real == b for _, b, p in pairs), "25->50: real frame not delayed")

# --- 25/30 jitter -> 50 -----------------------------------------------------
spans = [40.0 if i % 3 == 0 else 1000.0 / 30.0 for i in range(120)]
s, pairs = run(spans, content_fps=27.0)
allp = presented(pairs)
check(all(allp[i] - allp[i - 1] == PERIOD for i in range(1, len(allp))),
      "25/30 jitter -> 50: output intervals stay exactly 20 ms")
counts = {}
for _, _, p in pairs:
    counts[p.count] = counts.get(p.count, 0) + 1
print("      jitter count histogram:", dict(sorted(counts.items())))
check(counts.get(0, 0) > 0 and counts.get(1, 0) > 0,
      "25/30 jitter -> 50: the per-pair count really varies")
check(counts.get(2, 0) == 0, "25/30 jitter -> 50: no pair needs more than one frame")

# --- stall ------------------------------------------------------------------
s, pairs = run([33.3, 33.3, 33.3, 120.0, 33.3, 33.3, 33.3])
check(pairs[3][2].reason == "longInterval", "stall: the long pair is bypassed")
check(pairs[4][2].count <= 2, "stall: the next pair is not charged for the gap")
check(pairs[4][2].real - pairs[4][1] < PERIOD,
      "stall: the grid restarts in phase, no accumulated offset")

# --- bypass rules -----------------------------------------------------------
s, pairs = run([1000.0 / 30.0] * 10, target=30.0, content_fps=30.0)
check(pairs[0][2].reason == "sourceAtTarget", "source already at the target: bypass")
sz, zp = run([33.3] * 4, target=0.0)
check(zp[1][2].reason == "targetOff", "target 0: bypass")
sb = Scheduler(Config(True, 50.0))
for d in [33.3] * 3:
    pass
b0 = ms(2000.0)
same = sb.plan(b0, b0, 30.0)
check(same.reason == "interval", "zero-length interval: bypass")

# --- cadence policies -------------------------------------------------------
s, pairs = run([1000.0 / 30.0] * 60, cadence="latency")
check(all(p.real == 0 for _, _, p in pairs), "latency policy: real frames keep their timestamps")
check(s.last_snap_ms == 0.0, "latency policy: reported snap is zero")
f, fp = run([1000.0 / 30.0] * 60)
check(any(p.real > b for _, b, p in fp), "fixed policy: real frames are snapped onto the grid")

# --- cap --------------------------------------------------------------------
s, pairs = run([66.7] * 20, target=160.0, content_fps=15.0)
check(all(p.count <= MAX_TICKS for _, _, p in pairs),
      "cap: a pair never plans more frames than the slot pool holds")
check(s.dropped_cap > 0, f"cap: slots past the cap are counted as dropped ({s.dropped_cap})")

# --- no drift, 5 minutes at 29.97 -------------------------------------------
s, pairs = run([1000.0 / 29.97] * 9000, content_fps=29.97)
check(all((p.pts[i] - pairs[0][0]) % PERIOD == 0 for _, _, p in pairs for i in range(p.count)),
      "5 min at 29.97 -> 50: generated slots never drift off the grid")
check(all((p.real - pairs[0][0]) % PERIOD == 0 for _, _, p in pairs),
      "5 min at 29.97 -> 50: real frames stay on the grid too")
expected = 9000 * (1000.0 / 29.97) / 1000.0 * 50.0
actual = s.generated + 9000
check(abs(actual - expected) / expected < 0.01,
      f"5 min at 29.97 -> 50: rate within 1% ({actual} vs {expected:.0f})")

# --- legacy / capacity / validation ----------------------------------------
off = Config(False)
check(capacity(off, 3) == 3, "legacy: a disabled config does not resize the VFG pool")
check(capacity(off, 1) == 1, "legacy: multiplier 1 stays 1")
sl = Scheduler(off)
p = sl.plan(0, ms(33.3), 30.0)
check(p.bypass and p.count == 0 and p.real == 0, "legacy: an off scheduler plans nothing")
check(capacity(Config(True, 50.0), 2) == 3, "afg: pool covers maxRuns + 1")
check(capacity(Config(True, 160.0), 2) == 8, f"afg: 160 FPS sizes an 8-wide group (got {capacity(Config(True, 160.0), 2)})")
check(valid(Config(False)), "the disabled config is always valid")
check(valid(Config(True, 50.0)), "the default AFG config is valid")
check(valid(Config(True, 0.0)), "target 0 is valid")
check(not valid(Config(True, 481.0)), "a target above 480 is rejected")
check(not valid(Config(True, 50.0, max_runs=0)), "maxRuns 0 is rejected")
check(not valid(Config(True, 50.0, max_runs=MAX_TICKS + 1)), "maxRuns past the pool is rejected")
check(not valid(Config(True, 50.0, max_interval=10.0)), "a max interval below one period is rejected")
check(runs_for(50.0) == 2, "50 FPS asks for at most two frames per pair")
check(runs_for(160.0) == 7, "160 FPS asks for seven")


# --- GPU load adaptation ----------------------------------------------------
s, pairs = run([1000.0 / 30.0] * 30, target=100.0)
full = sum(p.count for _, _, p in pairs)
check(full > 0 and s.dropped_load == 0, "load: nothing dropped while no cap is set")
s2 = Scheduler(Config(True, 100.0))
s2.set_load_cap(1)
capped_pairs = []
a = ms(1000.0)
for _ in range(30):
    b = a + ms(1000.0 / 30.0)
    capped_pairs.append((a, b, s2.plan(a, b, 30.0)))
    a = b
check(s2.effective_max_runs() == 1, "load: the cap lowers the per-pair ceiling")
check(all(p.count <= 1 for _, _, p in capped_pairs), "load: no pair exceeds the cap")
limited = sum(p.dropped_load for _, _, p in capped_pairs)
check(limited > 0 and s2.dropped_load > 0, "load: removed slots counted as droppedLoad")
check(s2.load_limited_pairs > 0, "load: pairs acted on are counted")
m = Scheduler(Config(True, 50.0, gpu_adapt=False))
m.set_load_cap(1)
check(m.effective_max_runs() == m.cfg.max_runs, "load: the switch overrides the cap")

# --- buffer policy ---------------------------------------------------------
check(stall_ms("tight") == 60.0, "buffer: tight gives up at 60 ms")
check(stall_ms("balanced") == 100.0, "buffer: balanced at 100 ms")
check(stall_ms("relaxed") == 250.0, "buffer: relaxed at 250 ms")
check(policy_cap("tight", 7) == 2, "buffer: tight caps a pair at two frames")
check(policy_cap("balanced", 7) == 7, "buffer: balanced keeps the rate cap")
b = Scheduler(Config(True, 50.0, buffer="balanced"))
b.anchored = True
for a0 in [1000.0, 1033.33, 1066.67]:
    pass
gb = b.plan(ms(2000.0), ms(2150.0), 30.0)
check(gb.reason == "longInterval", "buffer: balanced bypasses a 150 ms gap")
r = Scheduler(Config(True, 50.0, buffer="relaxed", max_interval=stall_ms("relaxed")))
gr = r.plan(ms(2000.0), ms(2150.0), 30.0)
check(r.cfg.max_interval > 150.0 and not gr.bypass and gr.count > 0,
      f"buffer: relaxed interpolates the same gap (count={gr.count})")

import sys
print()
print("FAILURES:", len(FAIL))
for f in FAIL:
    print("  -", f)
sys.exit(1 if FAIL else 0)
