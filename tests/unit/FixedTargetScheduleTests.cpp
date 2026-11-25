// SPDX-License-Identifier: GPL-3.0-or-later
// Fixed-target frame generation (engine/FixedTargetSchedule.h): the adaptive
// per-interval frame count, the carried remainder, the cap and the bypass rules.
//
// tools/fixed_target_sim.py runs the same cases over a wider matrix (target
// 60-160 FPS x source 20-60 FPS x maxRuns 2-6); keep the two in step.
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

// Feeds a source cadence and returns every plan, exactly as the engine sees them
// one pair at a time. Counts are the generated frames of each pair; the group VFG
// runs with is count + 1 because the real frame closes it.
static std::vector<FixedTargetPlan> run(FixedTargetScheduler& s, const std::vector<double>& spansMs,
                                        int64_t start = ms(1000), double contentFps = 30.0) {
    std::vector<FixedTargetPlan> out;
    int64_t a = start;
    for (double d : spansMs) {
        const int64_t b = a + ms(d);
        out.push_back(s.plan(a, b, contentFps));
        a = b;
    }
    return out;
}

// Output frames a cadence actually produced, over the wall time it covered: the
// number the user reads off a frame counter (real frames included).
static double observedFps(const std::vector<FixedTargetPlan>& plans, double totalMs) {
    double frames = 0;
    for (const auto& p : plans) frames += double(p.count) + (p.bypass ? 0.0 : 1.0);
    return frames / (totalMs / 1000.0);
}

int main() {
    // Off by default: the caller sees the pair exactly as it came in.
    {
        FixedTargetScheduler s;
        const auto plan = s.plan(ms(1000), ms(1033), 30.0);
        check(!plan.bypass && plan.count == 0 && plan.outputMultiplier == 1,
              "disabled: no frames, and a group width of 1 (generate nothing)");
        check(std::string(plan.reason) == "off", "disabled: reason=off");
    }

    // 30 FPS -> 80 FPS: 33.33 ms asks for 2.67 frames, so the group walks 3,2,3,2
    // and the long-run output rate is the target, not the source times a constant.
    {
        FixedTargetScheduler s;
        s.configure(config(80));
        std::vector<double> spans(60, 1000.0 / 30.0);
        const auto plans = run(s, spans);
        bool countSane = true, uniform = true, monotonicT = true;
        for (const auto& p : plans) {
            if (p.count < 1 || p.count > 3) countSane = false;
            for (unsigned i = 0; i < p.count; ++i) {
                const double want = double(i + 1) / double(p.count + 1);
                if (std::abs(p.t[i] - want) > 1e-9) uniform = false;
                if (!(p.t[i] > 0.0 && p.t[i] < 1.0)) monotonicT = false;
                if (i && !(p.t[i] > p.t[i - 1])) monotonicT = false;
            }
        }
        check(countSane, "30->80: 1-3 generated frames per pair");
        check(uniform, "30->80: positions are i/(count+1), i.e. VFG's own t=i/M");
        check(monotonicT, "30->80: every t inside (0,1) and rising");
        const double fps = observedFps(plans, 60 * 1000.0 / 30.0);
        check(std::abs(fps - 80.0) < 0.5, "30->80: average output rate is the target");
        check(s.stats().droppedByCap == 0, "30->80: nothing dropped");
    }

    // 30 FPS -> 50 FPS, the shipped default: 33.33 ms asks for 1.67 frames, so the
    // count walks 1,0,1,1,0,1 and every third-ish pair generates NOTHING. This is
    // the only target in the panel that reaches count = 0 on ordinary content, so
    // it is also the only case that proves the "no Run for this pair" path.
    {
        FixedTargetScheduler s;
        s.configure(config(50, 2));
        std::vector<double> spans(60, 1000.0 / 30.0);
        const auto plans = run(s, spans);
        unsigned empty = 0, filled = 0;
        bool countSane = true, widthMatches = true;
        for (const auto& p : plans) {
            if (p.count > 1) countSane = false;
            if (p.count == 0) ++empty; else ++filled;
            if (p.outputMultiplier != p.count + 1) widthMatches = false;
            for (unsigned i = 0; i < p.count; ++i)
                if (std::abs(p.t[i] - 0.5) > 1e-9) widthMatches = false;
        }
        check(countSane, "30->50: 0-1 generated frames per pair");
        check(empty > 0 && filled > 0, "30->50: some pairs generate nothing, some generate one");
        check(widthMatches, "30->50: group width is count+1 and t=0.5 when one frame is made");
        const double fps = observedFps(plans, 60 * 1000.0 / 30.0);
        check(std::abs(fps - 50.0) < 0.5, "30->50: average output rate is the target");
        check(s.stats().droppedByCap == 0, "30->50: the cap never bites at two runs");
    }

    // The wobble this mode exists for: 30 FPS dipping to 25 FPS. A constant
    // multiplier drags the output down with it; the adaptive count does not.
    {
        FixedTargetScheduler s;
        s.configure(config(80));
        std::vector<double> spans;
        double totalMs = 0;
        for (int i = 0; i < 40; ++i) {
            const double d = (i % 2) ? 40.0 : 1000.0 / 30.0;   // 25 FPS / 30 FPS
            spans.push_back(d); totalMs += d;
        }
        const auto plans = run(s, spans);
        const double fps = observedFps(plans, totalMs);
        check(std::abs(fps - 80.0) < 0.6, "25/30 wobble at 80: the rate stays on target");
        unsigned long long shortSum = 0, longSum = 0;
        for (size_t i = 0; i < plans.size(); ++i) ((i % 2) ? longSum : shortSum) += plans[i].count + 1;
        check(longSum > shortSum, "25/30 wobble: the longer intervals really do get more frames");
    }

    // 40 ms at 80 FPS is 3.2 frames: two generated, 0.2 carried. The carried
    // remainder is what makes the average exact instead of 3.0.
    {
        FixedTargetScheduler s;
        s.configure(config(80));
        const auto plans = run(s, std::vector<double>(5, 40.0));
        const unsigned sum = plans[0].count + plans[1].count + plans[2].count + plans[3].count + plans[4].count;
        check(sum == 11, "40 ms pairs: 2/2/3/2/2 generated -> 16 output frames over 200 ms");
        check(s.stats().lastCarry > -1.0 && s.stats().lastCarry < 1.0, "the carried remainder stays under one frame");
    }

    // Cap: 20 FPS content at 160 FPS wants 8 frames; the cap must drop frames and
    // forget the debt, never owe them back as latency.
    {
        FixedTargetScheduler s;
        s.configure(config(160, 6));
        const auto plans = run(s, std::vector<double>(20, 50.0), ms(1000), 20.0);
        bool capped = true;
        for (const auto& p : plans) if (p.count > 6) capped = false;
        check(capped, "20->160: no pair exceeds maxRuns");
        check(s.stats().droppedByCap > 0, "20->160: the overflow is counted as dropped");
        check(std::abs(s.stats().lastCarry) < 1e-9, "the cap leaves no debt behind");
        check(plans.back().count == 6, "the pairs after a capped one ask for the same, not for more");
    }

    // A stall: no interpolation across it, and the remainder is forgotten.
    {
        FixedTargetScheduler s;
        s.configure(config(80));
        run(s, std::vector<double>(3, 1000.0 / 30.0));
        const auto stall = s.plan(ms(2000), ms(2000) + ms(120), 30.0);
        check(stall.bypass && stall.count == 0, "120 ms gap: bypassed, no frames generated");
        check(std::string(stall.reason) == "longInterval", "120 ms gap: reason=longInterval");
        const auto after = s.plan(ms(2000) + ms(120), ms(2000) + ms(120) + ms(1000.0 / 30.0), 30.0);
        check(!after.bypass && after.count >= 1 && after.count <= 3, "after a stall the next normal pair plans normally");
    }

    // Source already at or above the target (menus, a real 60 FPS source).
    {
        FixedTargetScheduler s;
        s.configure(config(60));
        const auto plan = s.plan(ms(1000), ms(1016), 60.0);
        check(plan.bypass && plan.count == 0, "60 FPS source at a 60 FPS target: auto bypass");
        check(std::string(plan.reason) == "sourceAtTarget", "auto bypass reports sourceAtTarget");
        check(s.stats().bypassAtTarget == 1, "auto bypass is counted");
    }

    // 0 is the panel's "insert nothing", not an error and not a division.
    {
        FixedTargetScheduler s;
        s.configure(config(0));
        const auto plan = s.plan(ms(1000), ms(1040), 25.0);
        check(plan.bypass && plan.count == 0, "target 0: bypassed");
        check(std::string(plan.reason) == "targetOff", "target 0: reason=targetOff");
    }

    // Non-positive intervals come from a reset or a duplicate, never from a plan.
    {
        FixedTargetScheduler s;
        s.configure(config(80));
        check(s.plan(ms(1000), ms(1000), 30.0).bypass, "zero-length interval: bypassed");
        check(s.plan(ms(1000), ms(990), 30.0).bypass, "negative interval: bypassed");
    }

    // Smoothing: with 0 the exact span decides; a clamped average damps the count.
    {
        FixedTargetScheduler plain, smooth;
        plain.configure(config(80));
        auto c = config(80);
        c.smoothing = 0.6;
        c.clampMinMs = 30;
        c.clampMaxMs = 45;
        smooth.configure(c);
        const std::vector<double> spans = {40.0, 25.0, 25.0, 40.0};
        const auto a = run(plain, spans);
        const auto b = run(smooth, spans);
        check(a[1].count < b[1].count, "smoothing keeps the count from collapsing after one short interval");
        check(smooth.stats().lastSmoothedSpanMs > 25.0, "the smoothed span stays inside the clamps");
    }

    // Structure, capacity and validation.
    {
        check(fixedTargetRunsFor(80) == 4, "runsFor(80) keeps the historical four");
        check(fixedTargetRunsFor(120) == 5, "runsFor(120) covers 40 ms at 120 FPS");
        check(fixedTargetRunsFor(160) == 6, "runsFor(160) is the ceiling");
        FixedTargetConfig c = config(100, 4);
        check(fixedTargetCapacity(c, 2) == 5, "capacity covers maxRuns + the real frame");
        check(fixedTargetCapacity(c, 8) == 8, "a larger requested multiplier is kept");
        c.maxRuns = kFixedTargetMaxTicks;
        check(fixedTargetCapacity(c, 2) == kFixedTargetMaxTicks + 1, "capacity follows maxRuns");
        FixedTargetConfig off;
        check(fixedTargetCapacity(off, 2) == 2, "multiplier mode keeps the requested multiplier");
        check(fixedTargetSameStructure(config(80), config(144)), "the rate is not structural");
        check(!fixedTargetSameStructure(config(80), config(80, 6)), "the cap is structural");

        FixedTargetConfig v = config(80);
        check(fixedTargetValid(v), "a normal config is valid");
        v.targetFps = 0;
        check(fixedTargetValid(v), "target 0 is valid (it means insert nothing)");
        v = config(80); v.targetFps = 481;
        check(!fixedTargetValid(v), "a target above 480 is rejected");
        v = config(80); v.maxRuns = 0;
        check(!fixedTargetValid(v), "maxRuns 0 is rejected");
        v = config(80); v.smoothing = 1.5;
        check(!fixedTargetValid(v), "smoothing outside 0..1 is rejected");
        v = config(80); v.clampMaxMs = 10;
        check(!fixedTargetValid(v), "a clamp ceiling below the floor is rejected");
        v = config(80); v.maxIntervalMs = 30;
        check(!fixedTargetValid(v), "maxIntervalMs below the clamp ceiling is rejected");
        FixedTargetConfig disabled;
        check(fixedTargetValid(disabled), "the disabled config is always valid");
    }

    if (failures == 0) std::printf("all fixed-target schedule tests passed\n");
    else std::printf("%d fixed-target schedule test(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
