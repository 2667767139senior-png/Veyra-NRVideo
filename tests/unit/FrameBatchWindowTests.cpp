// Which real-frame pairs may carry generated frames.
//
// The first pair after a graph rebuild is not a pair: warmCapture() clears
// prevValid_ and leaves prevPtsMs_ at 0, so the endpoints are (0, sourceTime).
// Computing a generated timestamp for it used to throw out of the run loop and
// end playback on every VFG pool rebuild, so the window rule gets its own test.
#include "veyra/pipeline/FrameBatch.h"

#include <cstdio>
#include <stdexcept>
#include <string>

using namespace veyra::pipeline;

namespace {
int failures = 0;

void check(bool ok, const char* label)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}

bool throwsWindow(int64_t a, int64_t b, uint32_t sub, uint32_t n)
{
    try {
        (void)FrameBatch::interpolate(a, b, sub, n);
        return false;
    } catch (const std::invalid_argument& error) {
        return std::string(error.what()) == "invalid interpolation interval";
    }
}
}

int main()
{
    // A 30 fps pair on the source timeline is a pair.
    check(FrameBatch::pairInterpolable(0, 333330), "30 fps pair from origin interpolates");
    check(FrameBatch::pairInterpolable(10000000, 10333330), "pair late on the timeline interpolates");

    // The rebuild case: A stays at the warmup value, B is 100 s into the source.
    check(!FrameBatch::pairInterpolable(0, 1001018990), "warmup-to-source pair is not a window");
    check(throwsWindow(0, 1001018990, 1, 2), "warmup-to-source pair refuses instead of inventing a time");

    // Degenerate and over-long windows are refused for the same reason.
    check(!FrameBatch::pairInterpolable(1000, 1000), "empty window is not a window");
    check(!FrameBatch::pairInterpolable(2000, 1000), "reversed window is not a window");
    check(!FrameBatch::pairInterpolable(0, 10000001), "window wider than one second is refused");
    check(FrameBatch::pairInterpolable(0, 10000000), "exactly one second is still accepted");

    // Valid windows keep the documented rounding: 30 -> 60 puts the generated
    // frame on the half period, never past B.
    check(FrameBatch::interpolate(0, 333330, 1, 2) == 166665, "half period lands on the midpoint");
    check(FrameBatch::interpolate(0, 333333, 1, 2) == 166667, "midpoint rounds to nearest tick");
    check(FrameBatch::interpolate(0, 400000, 3, 4) == 300000, "3/4 position is exact");

    // Multiplier bounds belong to the batch capacity.
    check(throwsWindow(0, 333330, 1, 1), "single-frame group has no in-between position");
    check(throwsWindow(0, 333330, 9, 9), "group wider than capacity is refused");

    std::printf(failures ? "frame batch window: %d FAILURES\n" : "frame batch window: all checks passed\n", failures);
    return failures ? 1 : 0;
}
