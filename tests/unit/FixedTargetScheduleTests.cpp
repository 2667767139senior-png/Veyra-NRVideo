// SPDX-License-Identifier: GPL-3.0-or-later
// Fixed-target frame generation (engine/FixedTargetSchedule.h): the output grid,
// the per-interval tick count, the cap and the bypass rules.
//
// tools/fixed_target_sim.py runs the same cases over the whole 60-144 FPS x
// 20-60 FPS x maxRuns 1-6 matrix; keep the two in step.
#include "veyra/engine/FixedTargetSchedule.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace veyra::engine;
static int failures = 0;
static void check(bool ok, const std::string& label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok) ++failures;
}
static int64_t ms(double v) { return int64_t(v * 10000.0 + 0.5); }

static FixedTargetConfig config(double target, unsigned maxRuns = 4) {
    FixedTargetConfig c;
    c.enabled = true;
    c.targetFps = target;
    c.maxRuns = maxRuns;
    return c;
}

// Feeds a source cadence and returns every submitted PTS in order, exactly as the
// presentation timeline sees them: the generated ticks, then the real frame.
static std::vector<int64_t> run(FixedTargetScheduler& s, const std::vector<int64_t>& deltas,
                                int64_t start = ms(1000), double contentFps = 30.0,
                                std::vector<unsigned>* counts = nullptr) {
    std::vector<int64_t> out;
    int64_t a = start;
    for (auto d : deltas) {
        const int64_t b = a + d;
        const auto plan = s.plan(a, b, contentFps);
        for (unsigned i = 0; i < plan.count; ++i) out.push_back(plan.pts100ns[i]);
        out.push_back(plan.realPts100ns);
        if (counts) counts->push_back(plan.count);
        a = b;
    }
    return out;
}

static bool uniform(const std::vector<int64_t>& pts, int64_t period) {
    for (size_t i = 1; i < pts.size(); ++i)
        if (pts[i] - pts[i - 1] != period) return false;
    return pts.size() > 3;
}

static bool strictly_rising(const std::vector<int64_t>& pts) {
    for (size_t i = 1; i < pts.size(); ++i)
        if (pts[i] <= pts[i - 1]) return false;
    return pts.size() > 1;
}

int main() {
    // A steady 30 FPS source at 80 FPS: the count varies, the output does not.
    {
        FixedTargetScheduler s;
        s.configure(config(80));
        std::vector<unsigned> counts;
        const auto pts = run(s, std::vector<int64_t>(120, ms(1000.0 / 30.0)), ms(1000), 30.0, &counts);
        check(uniform(pts, ms(1000.0 / 80.0)), "30 FPS -> 80 FPS: every submitted interval is one 80 FPS period");
        unsigned lo = 99, hi = 0;
        for (auto c : counts) { lo = std::min(lo, c); hi = std::max(hi, c); }
        check(lo == 1 && hi == 2, "30 FPS -> 80 FPS: 1 or 2 generated frames per interval");
        check(s.stats().droppedByCap == 0, "30 FPS -> 80 FPS: nothing dropped at maxRuns=4");
    }

    // The point of the feature: a source that alternates 33/40 ms keeps the same
    // output cadence, where a multiplier swings between 60 and 50 FPS.
    {
        FixedTargetScheduler s;
        s.configure(config(80));
        std::vector<int64_t> deltas;
        for (int i = 0; i < 200; ++i) deltas.push_back(ms(i % 3 == 0 ? 40.0 : 1000.0 / 30.0));
        const auto pts = run(s, deltas);
        check(uniform(pts, ms(1000.0 / 80.0)), "30<->25 FPS wobble: the output interval never moves");
    }

    // Transport jitter that the deduplicated content clock still carries.
    {
        FixedTargetScheduler s;
        s.configure(config(80));
        std::vector<int64_t> deltas;
        for (int i = 0; i < 180; ++i) deltas.push_back(ms(1000.0 / 30.0 + 1.7 * ((i * 7) % 5 - 2)));
        check(uniform(run(s, deltas), ms(1000.0 / 80.0)), "+-3.4 ms pair jitter: the output interval never moves");
    }

    // 20 FPS at 120 FPS would need six runs; maxRuns=4 drops the surplus but the
    // clock keeps running and every PTS stays on the grid.
    {
        FixedTargetScheduler s;
        s.configure(config(120));
        std::vector<unsigned> counts;
        const auto pts = run(s, std::vector<int64_t>(60, ms(50.0)), ms(1000), 20.0, &counts);
        unsigned hi = 0;
        for (auto c : counts) hi = std::max(hi, c);
        check(hi == 4, "20 FPS -> 120 FPS: capped at maxRuns=4");
        check(s.stats().droppedByCap > 0, "20 FPS -> 120 FPS: the surplus ticks are counted as dropped");
        check(strictly_rising(pts), "dropped ticks never reorder the submitted stream");
        const auto period = ms(1000.0 / 120.0);
        bool onGrid = strictly_rising(pts);
        for (size_t i = 1; i < pts.size(); ++i)
            if ((pts[i] - pts[0]) % period != 0) onGrid = false;
        check(onGrid, "every submitted PTS stays on the output grid after drops");
    }

    // A 120 ms stall is a pause, not content: no runs, the real frame is
    // submitted as it arrives and the grid restarts from it.
    {
        FixedTargetScheduler s;
        s.configure(config(80));
        run(s, std::vector<int64_t>(20, ms(1000.0 / 30.0)), ms(1000));
        const auto plan = s.plan(ms(2000), ms(2120), 30.0);
        check(plan.bypass && plan.count == 0, "120 ms gap: bypass, no interpolation");
        check(plan.realPts100ns == ms(2120), "120 ms gap: the real frame keeps its own PTS");
        check(s.stats().bypassLong == 1 && s.stats().resyncs == 1, "120 ms gap: counted and re-synchronised");
        const int64_t b = ms(2153.333);
        const auto after = s.plan(ms(2120), b, 30.0);
        check(after.count >= 1 && !after.bypass, "after a stall the grid runs again at once");
        // The snap is measured against B, not against the stall: the real frame is
        // never pushed past the first grid point at or after itself.
        check((after.realPts100ns - b) <= ms(1000.0 / 80.0), "the first real frame after a stall is snapped within one period");
    }

    // A real 60 FPS source (menus) at a 60 FPS target has nothing to insert.
    {
        FixedTargetScheduler s;
        s.configure(config(60));
        const auto plan = s.plan(ms(1000), ms(1016.667), 60.0);
        check(plan.bypass && plan.count == 0, "60 FPS source at a 60 FPS target: automatic bypass");
        check(s.stats().bypassAtTarget == 1, "the bypass is counted");
    }

    // t never reaches 0 or 1, always rises, and the real frame always follows the
    // last tick - FrameBatch appends the real frame last and needs a rising PTS.
    {
        FixedTargetScheduler s;
        s.configure(config(120, kFixedTargetMaxTicks));
        bool sane = true;
        for (double span : {8.4, 12.5, 16.667, 20.0, 25.0, 33.333, 40.0, 50.0, 66.0, 99.0}) {
            const auto plan = s.plan(ms(500), ms(500 + span), 30.0);
            if (plan.count + 1 > 1 + kFixedTargetMaxTicks) sane = false;
            for (unsigned i = 0; i < plan.count; ++i)
                if (!(plan.t[i] > 0.0 && plan.t[i] < 1.0)) sane = false;
            for (unsigned i = 1; i < plan.count; ++i)
                if (!(plan.t[i] > plan.t[i - 1])) sane = false;
            const int64_t last = plan.count ? plan.pts100ns[plan.count - 1] : ms(500);
            if (plan.realPts100ns <= last) sane = false;
        }
        check(sane, "t stays inside (0,1), rises, and the real frame follows the last tick");
    }

    // With smoothing off the tick sits at its exact content position; with it on
    // the span is clamped, so a 20 ms source is stretched against the 25 ms floor.
    {
        FixedTargetConfig fast = config(80);
        FixedTargetScheduler exact;
        exact.configure(fast);
        const auto a = exact.plan(ms(100), ms(120), 50.0);
        fast.smoothing = 0.5;
        FixedTargetScheduler smoothed;
        smoothed.configure(fast);
        const auto b = smoothed.plan(ms(100), ms(120), 50.0);
        check(a.count == b.count, "smoothing does not change which ticks exist");
        if (a.count && b.count)
            check(b.t[0] < a.t[0], "smoothing stretches a short interval against the 25 ms floor");
    }

    // Capacity: a fixed-target graph must be built with enough generated slots.
    {
        FixedTargetConfig c = config(100, 4);
        check(fixedTargetCapacity(c, 2) == 5, "capacity covers maxRuns + the real frame");
        check(fixedTargetCapacity(c, 8) == 8, "a larger requested multiplier is kept");
        c.maxRuns = kFixedTargetMaxTicks;
        check(fixedTargetCapacity(c, 2) == kFixedTargetMaxTicks + 1, "capacity follows maxRuns");
        FixedTargetConfig off;
        check(fixedTargetCapacity(off, 2) == 2, "multiplier mode keeps the requested multiplier");
    }

    // The panel now takes any integer 0-160, so the run cap has to follow the
    // typed rate: at 40 ms of content, 120 FPS needs five ticks and 144 needs six.
    {
        check(fixedTargetRunsFor(80.0) == 4, "80 FPS keeps the historical four runs");
        check(fixedTargetRunsFor(60.0) == 3, "60 FPS needs three runs at 40 ms");
        check(fixedTargetRunsFor(120.0) == 5, "120 FPS gets a fifth run so nothing is dropped");
        check(fixedTargetRunsFor(144.0) == 6, "144 FPS reaches the batch ceiling");
        check(fixedTargetRunsFor(160.0) == kFixedTargetMaxTicks, "160 FPS stays inside the ceiling");
        check(fixedTargetRunsFor(0.0) == 4, "a zero target leaves the default alone");
    }

    // The real frame takes the first output grid point at or after B. That is what
    // keeps every submitted interval at exactly one period, and it bounds the cost:
    // the pair is never held longer than one output period.
    {
        FixedTargetScheduler s;
        s.configure(config(80));
        const int64_t period = ms(1000.0 / 80.0);
        bool onGrid = true;
        int64_t worstDelay = 0;
        int64_t a = ms(1000);
        for (int i = 0; i < 240; ++i) {
            const int64_t b = a + ms(i % 4 == 0 ? 40.0 : 1000.0 / 30.0);
            const auto plan = s.plan(a, b, 30.0);
            if (plan.realPts100ns < b || plan.realPts100ns - b >= period) onGrid = false;
            worstDelay = std::max(worstDelay, plan.realPts100ns - b);
            a = b;
        }
        check(onGrid, "the real frame lands on the first grid point at or after B");
        check(worstDelay < period, "snapping the real frame costs less than one output period");
    }

    // Only the mode and the run cap shape the graph; the rate and the smoothing
    // knobs are read per pair (EnhancementSettings::sameVideoConfiguration).
    {
        const FixedTargetConfig a = config(80);
        FixedTargetConfig b = a;
        b.targetFps = 100; b.smoothing = 0.5; b.clampMinMs = 30; b.clampMaxMs = 70; b.maxIntervalMs = 150;
        check(fixedTargetSameStructure(a, b), "rate and smoothing knobs keep the running graph");
        b.maxRuns = 6;
        check(!fixedTargetSameStructure(a, b), "a different run cap rebuilds the graph");
        b = a; b.enabled = false;
        check(!fixedTargetSameStructure(a, b), "turning the mode off rebuilds the graph");
    }

    // Validation.
    {
        // 0 is the panel's "insert nothing" value, so it has to be accepted and
        // turned into a bypass rather than rejected or divided by.
        FixedTargetConfig zero = config(0.0);
        check(fixedTargetValid(zero), "a zero target is accepted as 'generate nothing'");
        FixedTargetScheduler z;
        z.configure(zero);
        const auto zplan = z.plan(ms(100), ms(133), 30.0);
        check(zplan.bypass && zplan.count == 0 && std::string(zplan.reason) == "targetOff",
              "a zero target bypasses the pair instead of dividing by zero");
        check(z.stats().bypassOff == 1, "the zero-target bypass is counted");
        FixedTargetConfig c = config(80);
        c.maxRuns = kFixedTargetMaxTicks + 1;
        check(!fixedTargetValid(c), "maxRuns above the batch capacity is rejected");
        c = config(80);
        c.clampMinMs = 60;
        c.clampMaxMs = 25;
        check(!fixedTargetValid(c), "an inverted clamp range is rejected");
        c = config(80);
        c.maxIntervalMs = 40;
        check(!fixedTargetValid(c), "a longest interval below the clamp is rejected");
        c = config(80);
        c.nearStart = 0.6;
        check(!fixedTargetValid(c), "a start band past the middle is rejected");
        check(fixedTargetValid(config(80)), "the default fixed-target configuration is valid");
        check(fixedTargetValid(FixedTargetConfig{}), "a disabled configuration is valid");
    }

    // Disabled means no plan, never a changed stream.
    {
        FixedTargetScheduler s;
        const auto plan = s.plan(ms(1000), ms(1033), 30.0);
        check(!plan.bypass && plan.count == 0 && plan.realPts100ns == ms(1033), "disabled: the caller sees the plain pair");
    }

    std::printf("%s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
