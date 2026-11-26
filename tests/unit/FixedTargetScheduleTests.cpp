// SPDX-License-Identifier: GPL-3.0-or-later
// AFG (engine/FixedTargetSchedule.h): the output grid, its timesteps, the snap
// bound of each cadence policy, and the rules that keep a hiccup from becoming a
// backlog.
//
// These are the properties the product promised, not just "50 FPS on average":
//   - every output frame lands on the same grid, whatever the input jitter does;
//   - a real frame is never shown before its own timestamp, and never a full
//     period late;
//   - a stall or a lost slot is dropped, never repaid;
//   - the grid does not drift over a long run;
//   - the legacy multiplier path is untouched when the mode is off.
#include "veyra/engine/FixedTargetSchedule.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace veyra::engine;
static int failures = 0;
static void check(bool ok, const std::string& label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok) ++failures;
}
static int64_t ms(double v) { return int64_t(v * 10000.0 + 0.5); }

static FixedTargetConfig config(double target, unsigned maxRuns = 0,
                                FixedTargetCadence cadence = FixedTargetCadence::Fixed) {
    FixedTargetConfig c;
    c.enabled = true;
    c.targetFps = target;
    c.maxRuns = maxRuns ? maxRuns : fixedTargetRunsFor(target);
    c.cadence = cadence;
    return c;
}

// One source pair as the engine sees it, plus the plan made for it.
struct Pair {
    int64_t a = 0, b = 0;
    FixedTargetPlan plan;
};

static std::vector<Pair> run(FixedTargetScheduler& s, const std::vector<double>& spansMs,
                             int64_t start = ms(1000), double contentFps = 30.0) {
    std::vector<Pair> out;
    int64_t a = start;
    for (double d : spansMs) {
        Pair p;
        p.a = a;
        p.b = a + ms(d);
        p.plan = s.plan(p.a, p.b, contentFps);
        out.push_back(p);
        a = p.b;
    }
    return out;
}

// Every presented frame in order: generated slots first, then the real frame.
struct Presented {
    std::vector<int64_t> pts;
    std::vector<double> timestep;   // 0 for real frames
    size_t real = 0;
};

static Presented presented(const std::vector<Pair>& pairs) {
    Presented out;
    for (const auto& p : pairs) {
        for (unsigned i = 0; i < p.plan.count; ++i) {
            out.pts.push_back(p.plan.pts100ns[i]);
            out.timestep.push_back(p.plan.timestep[i]);
        }
        out.pts.push_back(p.plan.realPts100ns);
        out.timestep.push_back(0.0);
        ++out.real;
    }
    return out;
}

int main() {
    // 30 -> 50: the shipped default. 200 generated frames per 300 pairs, and the
    // union of generated and real frames is exactly one frame per 20 ms.
    {
        FixedTargetScheduler s;
        s.configure(config(50.0));
        const auto pairs = run(s, std::vector<double>(300, 1000.0 / 30.0));
        unsigned generated = 0, empty = 0;
        for (const auto& p : pairs) {
            generated += p.plan.count;
            if (p.plan.count == 0) ++empty;
        }
        check(generated == 200, "30->50: 300 pairs generate 200 frames (500 outputs in 10 s)");
        check(empty == 100, "30->50: every third pair generates nothing");

        const int64_t period = ms(20.0);
        bool exact = true, monotonic = true, snapBounded = true, steps = true;
        int64_t previous = std::numeric_limits<int64_t>::min();
        for (const auto& p : pairs) {
            // The guard is a constant lookahead on every frame, so the snap is what
            // is left after removing it.
            const int64_t snapped = p.plan.realPts100ns - p.plan.guard100ns;
            if (snapped < p.b || snapped - p.b >= period) snapBounded = false;
            for (unsigned k = 0; k < p.plan.count; ++k) {
                const int64_t slot = p.plan.pts100ns[k];
                if ((slot - pairs.front().a) % period != 0) exact = false;
                if (slot <= previous) monotonic = false;
                previous = slot;
                // The guard shifts the presentation time only; the position inside
                // the pair is still measured from the slot itself.
                const double expected = double(slot - p.plan.guard100ns - p.a) / double(p.b - p.a);
                if (!(p.plan.timestep[k] > 0.0 && p.plan.timestep[k] < 1.0)) steps = false;
                if (std::abs(p.plan.timestep[k] - expected) > 1e-9) steps = false;
            }
            if (p.plan.realPts100ns <= previous) monotonic = false;
            previous = p.plan.realPts100ns;
        }
        check(exact, "30->50: every slot is a whole 20 ms step from the origin");
        check(steps, "30->50: every timestep equals (slot-a)/(b-a) and stays inside (0,1)");
        check(monotonic, "30->50: presentation times strictly increase across pair boundaries");
        check(snapBounded, "30->50: the real frame is never early and never a full period late");

        // Consecutive presented frames exactly one period apart is the only claim
        // that means "even cadence" rather than "50 FPS on average".
        const auto all = presented(pairs);
        bool even = all.pts.size() > 1;
        for (size_t i = 1; i < all.pts.size(); ++i)
            if (all.pts[i] - all.pts[i - 1] != period) even = false;
        check(all.pts.size() == 500, "30->50: 500 presented frames in 10 s");
        check(even, "30->50: presented interval is exactly 20 ms, not an average of 20 ms");
    }

    // 25 -> 50: 40 ms is exactly two slots, so every pair generates one frame at
    // t = 0.5 and the real frame needs no snap at all.
    {
        FixedTargetScheduler s;
        s.configure(config(50.0));
        const auto pairs = run(s, std::vector<double>(40, 40.0), ms(1000), 25.0);
        bool every = true, half = true, onGrid = true;
        for (const auto& p : pairs) {
            every = every && p.plan.count == 1;
            half = half && std::abs(p.plan.timestep[0] - 0.5) < 1e-9;
            onGrid = onGrid && p.plan.realPts100ns - p.plan.guard100ns == p.b;
        }
        check(every, "25->50: one generated frame per 40 ms pair");
        check(half, "25->50: it sits at t = 0.5");
        check(onGrid, "25->50: 40 ms already lands on the grid, so the real frame is not delayed");
    }

    // 25/30 jitter -> 50: the cadence is what must not move. Counts differ per
    // pair; the presented grid never does.
    {
        FixedTargetScheduler s;
        s.configure(config(50.0));
        std::vector<double> spans;
        for (int i = 0; i < 120; ++i) spans.push_back(i % 3 == 0 ? 40.0 : 1000.0 / 30.0);
        const auto pairs = run(s, spans, ms(1000), 27.0);
        const auto all = presented(pairs);
        const int64_t period = ms(20.0);
        bool even = all.pts.size() > 10;
        for (size_t i = 1; i < all.pts.size(); ++i)
            if (all.pts[i] - all.pts[i - 1] != period) even = false;
        check(even, "25/30 jitter -> 50: output intervals stay exactly 20 ms");
        unsigned counts[3] = {0, 0, 0};
        for (const auto& p : pairs) counts[std::min<unsigned>(p.plan.count, 2u)]++;
        check(counts[0] > 0 && counts[1] > 0, "25/30 jitter -> 50: the per-pair count really varies");
        check(counts[2] == 0, "25/30 jitter -> 50: no pair needs more than one frame");
    }

    // A stall is not repaid. 120 ms > maxIntervalMs, so the pair is bypassed and
    // the grid re-anchors instead of owing three frames to the next pair.
    {
        FixedTargetScheduler s;
        s.configure(config(50.0));
        const std::vector<double> spans = {33.3, 33.3, 33.3, 120.0, 33.3, 33.3, 33.3};
        const auto pairs = run(s, spans);
        check(pairs[3].plan.bypass && std::string(pairs[3].plan.reason) == "longInterval",
              "stall: the long pair is bypassed, not interpolated");
        check(pairs[4].plan.count <= 2, "stall: the next pair is not charged for the gap");
        check(pairs[4].plan.realPts100ns - pairs[4].plan.guard100ns - pairs[4].b < ms(20.0),
              "stall: the grid restarts in phase, no accumulated offset");
    }

    // Bypass rules that must not spend GPU.
    {
        FixedTargetScheduler s;
        s.configure(config(30.0));
        const auto pairs = run(s, std::vector<double>(10, 1000.0 / 30.0), ms(1000), 30.0);
        check(pairs[0].plan.bypass && std::string(pairs[0].plan.reason) == "sourceAtTarget",
              "source already at the target: bypass");
        FixedTargetScheduler zero;
        zero.configure(config(0.0));
        const auto z = run(zero, std::vector<double>(4, 33.3));
        check(z[1].plan.bypass && std::string(z[1].plan.reason) == "targetOff", "target 0: bypass");
        FixedTargetScheduler bad;
        bad.configure(config(50.0));
        run(bad, std::vector<double>(3, 33.3));
        const FixedTargetPlan same = bad.plan(ms(2000), ms(2000), 30.0);
        check(same.bypass && std::string(same.reason) == "interval", "zero-length interval: bypass");
    }

    // The latency policy keeps real timestamps and accepts an uneven union; the
    // fixed policy is the one that guarantees the 20 ms grid.
    {
        FixedTargetScheduler s;
        s.configure(config(50.0, 0, FixedTargetCadence::Latency));
        const auto pairs = run(s, std::vector<double>(60, 1000.0 / 30.0));
        bool noRetime = true;
        for (const auto& p : pairs) noRetime = noRetime && p.plan.realPts100ns == 0;
        check(noRetime, "latency policy: real frames keep their own timestamps");
        check(s.stats().lastSnapMs == 0.0, "latency policy: reported snap is zero");
        // Same source, fixed policy: the real frame does move.
        FixedTargetScheduler f;
        f.configure(config(50.0));
        const auto fp = run(f, std::vector<double>(60, 1000.0 / 30.0));
        bool someRetime = false;
        for (const auto& p : fp) someRetime = someRetime || p.plan.realPts100ns > p.b;
        check(someRetime, "fixed policy: real frames are snapped onto the grid");
    }

    // Cap: a target the GPU cannot fill per pair must drop slots instead of
    // owing them. 15 FPS (66.7 ms) at 160 FPS asks for ten frames; the cap keeps
    // seven. (20 FPS at 160 is exactly 8-wide and must NOT drop - it is the
    // boundary the pool was sized for.)
    {
        FixedTargetScheduler s;
        s.configure(config(160.0));
        const auto pairs = run(s, std::vector<double>(20, 66.7), ms(1000), 15.0);
        const auto st = s.stats();
        bool within = true;
        for (const auto& p : pairs) within = within && p.plan.count <= kFixedTargetMaxTicks;
        check(within, "cap: a pair never plans more frames than the slot pool holds");
        check(st.droppedCap > 0, "cap: slots past the cap are counted as dropped");
    }

    // No drift: 29.97 in, 50 out, five minutes. Slot times stay exact multiples
    // of the period from the origin and the generated count matches the rate.
    {
        FixedTargetScheduler s;
        s.configure(config(50.0));
        const double span = 1000.0 / 29.97;
        const auto pairs = run(s, std::vector<double>(9000, span), ms(1000), 29.97);
        const int64_t period = ms(20.0);
        bool exact = true, bounded = true;
        for (const auto& p : pairs) {
            for (unsigned k = 0; k < p.plan.count; ++k)
                if ((p.plan.pts100ns[k] - pairs.front().a) % period != 0) exact = false;
            if ((p.plan.realPts100ns - p.plan.guard100ns - pairs.front().a) % period != 0) bounded = false;
        }
        check(exact, "5 min at 29.97 -> 50: generated slots never drift off the grid");
        check(bounded, "5 min at 29.97 -> 50: real frames stay on the grid too");
        const auto st = s.stats();
        // Total presented frames: 300.3 s of output at 50 FPS. The real frames are
        // 9000 of those, so generated = total - 9000. (Adding the real frames to
        // both sides again used to double count them and fail by 37%.)
        const double expected = 9000.0 * span / 1000.0 * 50.0;
        const double actual = double(st.generated) + 9000.0;
        check(std::abs(actual - expected) / expected < 0.01,
              "5 min at 29.97 -> 50: output count matches the target rate within 1%");
    }

    // Legacy multiplier mode must be untouched: with the mode off the graph is
    // sized exactly as the user asked, and no plan alters anything.
    {
        const FixedTargetConfig off;
        check(fixedTargetCapacity(off, 3) == 3, "legacy: a disabled config does not resize the VFG pool");
        check(fixedTargetCapacity(off, 1) == 1, "legacy: multiplier 1 stays 1");
        FixedTargetScheduler s;
        s.configure(off);
        const FixedTargetPlan p = s.plan(0, ms(33.3), 30.0);
        check(p.bypass && p.count == 0 && p.realPts100ns == 0, "legacy: an off scheduler plans nothing");
        check(fixedTargetCapacity(config(50.0), 2) == 3, "afg: the pool covers maxRuns + 1");
        check(fixedTargetCapacity(config(160.0), 2) == 8, "afg: 160 FPS sizes an 8-wide group");
        check(fixedTargetSameStructure(config(50.0, 2), config(80.0, 2)),
              "afg: a rate change keeps the same graph structure");
        check(!fixedTargetSameStructure(config(50.0, 2), config(50.0, 4)),
              "afg: a cap change does rebuild the graph");
    }

    // Lookahead (FixedTargetPlan::guard100ns). A generated frame between A and B
    // only exists once B does, so a grid with no lookahead gets presented late and
    // the spacing turns into 27/13 ms around a correct 20 ms mean - measured on a
    // real 30 -> 50 run before this was added. The guard is a whole number of
    // periods and the same for every frame, so spacing is untouched.
    {
        FixedTargetScheduler s;
        s.configure(config(50.0));
        const auto pairs = run(s, std::vector<double>(30, 1000.0 / 30.0));
        const auto guard = pairs[1].plan.guard100ns;
        check(guard >= ms(20.0) && guard % ms(20.0) == 0,
              "guard: a whole number of periods, at least one");
        check(guard == ms(40.0), "guard: 33.3 ms of input needs two 20 ms periods");
        bool constant = true;
        for (const auto& p : pairs) constant = constant && p.plan.guard100ns == guard;
        check(constant, "guard: constant across the run, so it cannot modulate the cadence");
        // 25 FPS: 40 ms is exactly two periods, so the guard is exactly one interval.
        FixedTargetScheduler q;
        q.configure(config(50.0));
        const auto slow = run(q, std::vector<double>(20, 40.0), ms(1000), 25.0);
        check(slow[1].plan.guard100ns == ms(40.0), "guard: 40 ms of input needs two periods too");
        // Latency policy takes no lookahead: it keeps the source clock on purpose.
        FixedTargetScheduler l;
        l.configure(config(50.0, 0, FixedTargetCadence::Latency));
        const auto fast = run(l, std::vector<double>(20, 1000.0 / 30.0));
        bool noGuard = true;
        for (const auto& p : fast) noGuard = noGuard && p.plan.guard100ns == 0;
        check(noGuard, "guard: the latency policy adds none");
    }

    // GPU load adaptation: the cap only ever lowers a pair's count, and what it
    // removes is reported separately from a configuration overflow.
    {
        // 100 FPS (10 ms period) needs three slots for a 33.3 ms pair, so a cap of
        // one has something to remove. At 50 FPS the same pair asks for one and the
        // cap could not tell "adapted" from "nothing to adapt".
        const FixedTargetConfig cfg = config(100.0);
        FixedTargetScheduler uncapped;
        uncapped.configure(cfg);
        const auto before = run(uncapped, std::vector<double>(30, 1000.0 / 30.0));
        unsigned full = 0;
        for (const auto& p : before) full += p.plan.count;
        check(full > 0 && uncapped.stats().droppedLoad == 0,
              "load: nothing is dropped while no cap is set");

        FixedTargetScheduler s;
        s.configure(cfg);
        s.setLoadCap(1);
        check(s.effectiveMaxRuns() == 1, "load: the cap lowers the per-pair ceiling");
        const auto capped = run(s, std::vector<double>(30, 1000.0 / 30.0));
        bool within = true;
        unsigned limited = 0;
        for (const auto& p : capped) within = within && p.plan.count <= 1;
        for (const auto& p : capped) limited += p.plan.droppedLoad;
        check(within, "load: no pair plans more frames than the measured GPU cost affords");
        check(limited > 0 && s.stats().droppedLoad > 0, "load: removed slots are counted as droppedLoad");
        check(s.stats().loadLimitedPairs > 0, "load: the pairs it acted on are counted");
        // Off means off: the switch has to be honoured by the scheduler itself, not
        // only by whoever sets the cap.
        FixedTargetConfig manual = cfg;
        manual.gpuLoadAdapt = false;
        FixedTargetScheduler m;
        m.configure(manual);
        m.setLoadCap(1);
        check(m.effectiveMaxRuns() == manual.maxRuns, "load: the adaptation switch overrides the cap");
    }

    // Buffer policy: the stall threshold and the per-pair ceiling are the two
    // numbers it really changes.
    {
        check(fixedTargetStallMs(FixedTargetBufferPolicy::Tight) == 60.0, "buffer: tight gives up at 60 ms");
        check(fixedTargetStallMs(FixedTargetBufferPolicy::Balanced) == 100.0, "buffer: balanced at 100 ms");
        check(fixedTargetStallMs(FixedTargetBufferPolicy::Relaxed) == 250.0, "buffer: relaxed at 250 ms");
        check(fixedTargetPolicyCap(FixedTargetBufferPolicy::Tight, 7) == 2, "buffer: tight caps a pair at two frames");
        check(fixedTargetPolicyCap(FixedTargetBufferPolicy::Balanced, 7) == 7, "buffer: balanced keeps the rate cap");
        // A 150 ms gap: over balanced's threshold, under relaxed's.
        FixedTargetConfig balanced = config(50.0);
        balanced.maxIntervalMs = fixedTargetStallMs(balanced.buffer);
        FixedTargetScheduler b;
        b.configure(balanced);
        run(b, std::vector<double>(3, 1000.0 / 30.0));
        const FixedTargetPlan longGap = b.plan(ms(2000), ms(2150), 30.0);
        check(longGap.bypass && std::string(longGap.reason) == "longInterval",
              "buffer: balanced bypasses a 150 ms gap");
        FixedTargetConfig relaxed = config(50.0);
        relaxed.buffer = FixedTargetBufferPolicy::Relaxed;
        relaxed.maxIntervalMs = fixedTargetStallMs(relaxed.buffer);
        FixedTargetScheduler r;
        r.configure(relaxed);
        run(r, std::vector<double>(3, 1000.0 / 30.0));
        const FixedTargetPlan longGapRelaxed = r.plan(ms(2000), ms(2150), 30.0);
        check(!longGapRelaxed.bypass && longGapRelaxed.count > 0,
              "buffer: relaxed interpolates the same gap");
        FixedTargetConfig bad = config(50.0);
        bad.buffer = FixedTargetBufferPolicy(3);
        check(!fixedTargetValid(bad), "buffer: an unknown policy is rejected");
    }

    // Validation keeps hand-edited profiles out of the scheduler.
    {
        check(fixedTargetValid(FixedTargetConfig{}), "the disabled config is always valid");
        FixedTargetConfig v = config(50.0);
        check(fixedTargetValid(v), "the default AFG config is valid");
        v = config(50.0); v.targetFps = 0;
        check(fixedTargetValid(v), "target 0 is valid (it means insert nothing)");
        v = config(50.0); v.targetFps = 481;
        check(!fixedTargetValid(v), "a target above 480 is rejected");
        v = config(50.0); v.maxRuns = 0;
        check(!fixedTargetValid(v), "maxRuns 0 is rejected");
        v = config(50.0); v.maxRuns = kFixedTargetMaxTicks + 1;
        check(!fixedTargetValid(v), "maxRuns past the slot pool is rejected");
        v = config(50.0); v.maxIntervalMs = 10;
        check(!fixedTargetValid(v), "a max interval below one period is rejected");
        check(fixedTargetRunsFor(50.0) == 2, "50 FPS asks for at most two frames per pair");
        check(fixedTargetRunsFor(160.0) == 7, "160 FPS asks for seven");
    }

    if (failures == 0) std::printf("all AFG schedule tests passed\n");
    else std::printf("%d AFG schedule test(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
