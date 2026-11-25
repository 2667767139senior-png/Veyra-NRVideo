#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace veyra::engine {
// Fixed-target frame generation ("固定目标帧率") for the NVIDIA VFG backend.
// See docs/FIXED_TARGET_FG_2026-11-25.md.
//
// Multiplier mode spends a constant M-1 runs on every source interval, so the
// output rate follows the picture: 30 FPS content that dips to 25 FPS drags a
// 60 FPS output down to 50, and that cadence change is what the eye reads as a
// stutter. Fixed-target mode instead decides the number of generated frames PER
// INTERVAL from the interval itself - the way adaptive frame generation does:
//
//     want  = spanMs * targetFps / 1000 + carry
//     count = clamp(round(want) - 1, 0, maxRuns)     // B itself is the last frame
//     carry = want - (count + 1)                     // error handed to the next pair
//
// A 40 ms pair at 80 FPS asks for 3.2 frames, so it gets two generated frames
// (VFG multiplier 3) and 0.2 frames of credit; the next asks 3.4, then 3.6, so
// the multiplier walks 3,3,4,3,3,... and the average output rate is exactly the
// target. A 33 ms pair at the same target asks 2.67 and walks 3,2,3,2. That is
// what absorbs the 25/30 FPS wobble: a longer interval gets one more generated
// frame, and nothing else changes.
//
// What this deliberately does NOT do:
//  - It does not run the output on its own grid. An earlier version planned a
//    free-running 1/targetFps grid and snapped the real frame onto it, which
//    re-times the picture (up to one period of extra latency) and makes the
//    output clock something the engine has to keep in sync. Here the real frame
//    keeps its own timestamp and its own latency; only the count varies.
//  - It does not use VFG's explicit-timestep mode. The generated frames sit at
//    t = i/(count+1) for i = 1..count, which is exactly what
//    FrameMultiplier=count+1 with FrameIndex=i produces - the same path
//    multiplier mode has always used, so nothing new rides on the SDK.
//  - It does not let an unschedulable pair build latency: when count hits
//    maxRuns the leftover credit is dropped (stats.droppedByCap), never owed.
//
// Everything here is pure arithmetic on the source clock in 100 ns units: no
// device work, no allocation, so it can run per pair on the engine thread.
inline constexpr unsigned kFixedTargetMaxTicks=6;
// 100 ns per second, the unit of every PTS in this project.
inline constexpr int64_t kFixedTargetUnit=10000000;

struct FixedTargetConfig {
    bool enabled=false;
    double targetFps=80.0;
    // Interval smoothing strength, 0..1, 0 = off (the default). With 0 the exact
    // source span decides the count, so the generated frames sit at their true
    // temporal positions. Smoothing averages the span a little for content whose
    // frame time keeps flipping, at the cost of the count lagging a real change
    // by a pair or two.
    double smoothing=0.0;
    double clampMinMs=25.0;
    double clampMaxMs=60.0;
    // A pair longer than this is a stall, a load or a pause: no interpolation,
    // the real frame goes out as it came in and the credit is forgotten.
    double maxIntervalMs=100.0;
    // Generated frames per pair. Past this the extra frames are dropped rather
    // than generated: a GPU that cannot keep up must lose frames, not latency.
    unsigned maxRuns=4;

    bool operator==(const FixedTargetConfig&)const=default;
};

inline bool fixedTargetValid(const FixedTargetConfig& c){
    if(!c.enabled)return true;
    // 0 is accepted and means "no generation" (plan() reports targetOff): the
    // panel offers the type-in field 0-160, and a target that low can only be
    // read as a request to stop inserting rather than as an error.
    return std::isfinite(c.targetFps)&&c.targetFps>=0.0&&c.targetFps<=480.0&&
        std::isfinite(c.smoothing)&&c.smoothing>=0.0&&c.smoothing<=1.0&&
        std::isfinite(c.clampMinMs)&&std::isfinite(c.clampMaxMs)&&c.clampMinMs>0.0&&c.clampMaxMs>=c.clampMinMs&&
        std::isfinite(c.maxIntervalMs)&&c.maxIntervalMs>=c.clampMaxMs&&
        c.maxRuns>=1&&c.maxRuns<=kFixedTargetMaxTicks;
}

// Frames per pair a target rate needs at the slowest content this mode is meant
// for (25 FPS = 40 ms): 40 ms at 120 FPS is 4.8 frames, so a cap of four would
// drop one in every interval. 80 FPS comes out at four, the historical default;
// callers keep maxRuns in step with the target instead of hard-coding it, so
// typing 144 FPS does not silently arrive capped at 4.
inline unsigned fixedTargetRunsFor(double targetFps){
    if(!(targetFps>0.0)||!std::isfinite(targetFps))return 4u;
    return std::clamp(static_cast<unsigned>(std::ceil(targetFps/25.0)),2u,kFixedTargetMaxTicks);
}

// The two fields the graph is built around: whether the mode runs at all, and how
// many generated slots a pair may ask for (fixedTargetCapacity below). Everything
// else is read once per pair by the scheduler, so tuning it while playing must not
// rebuild the graph (see EnhancementSettings::sameVideoConfiguration).
inline bool fixedTargetSameStructure(const FixedTargetConfig& a,const FixedTargetConfig& b){
    return a.enabled==b.enabled&&a.maxRuns==b.maxRuns;
}

// Generated slots are parity + 2*(sub-1) and VFG allocates 2*(M-1) of them, so a
// fixed-target graph must be built with a multiplier that covers maxRuns + 1 (the
// real frame closes the group). The user's multiplier is meaningless in this mode;
// the larger of the two is kept so the model family and the memory estimate stay
// the ones they chose.
inline unsigned fixedTargetCapacity(const FixedTargetConfig& c,unsigned requested){
    if(!c.enabled)return requested;
    const unsigned needed=std::min(c.maxRuns,kFixedTargetMaxTicks)+1u;
    return std::max(requested,needed);
}

struct FixedTargetPlan {
    static constexpr unsigned Capacity=kFixedTargetMaxTicks;
    // Generated frames for this pair. The group VFG runs with is count+1 wide,
    // because the real frame is its last member.
    unsigned count=0;
    unsigned outputMultiplier=2;                 // count+1, never below 2
    std::array<double,Capacity> t{};             // i/(count+1); telemetry only
    bool bypass=false;
    const char* reason="off";
};

// Runs on the engine thread, once per accepted pair, in source-clock order.
class FixedTargetScheduler {
public:
    struct Stats {
        uint64_t intervals=0,ticks=0,droppedByCap=0;
        uint64_t bypassLong=0,bypassAtTarget=0,bypassInvalid=0,bypassOff=0;
        unsigned lastTicks=0;
        double lastSpanMs=0,lastSmoothedSpanMs=0,lastCarry=0;
    };

    void configure(const FixedTargetConfig& config){
        // Only the mode and the cap are structural; a rate change is per pair and
        // must not disturb the credit, or every slider drag would restart the
        // average.
        if(!config.enabled&&config_.enabled)carry_=0;
        config_=config;
        if(!config.enabled)smoothedSpan_=0;
    }
    const FixedTargetConfig& config()const{return config_;}
    void reset(){const auto config=config_;*this=FixedTargetScheduler{};config_=config;}

    FixedTargetPlan plan(int64_t a100ns,int64_t b100ns,double measuredContentFps){
        FixedTargetPlan out;
        if(!config_.enabled){out.reason="off";return out;}
        // A zero target is the panel's "insert nothing" value; catching it here
        // keeps the conversion to a frame count out of a degenerate range.
        if(!(config_.targetFps>0.0)){
            ++stats_.bypassOff;out.bypass=true;out.reason="targetOff";carry_=0;return out;
        }
        const int64_t span=b100ns-a100ns;
        if(span<=0){
            ++stats_.bypassInvalid;out.bypass=true;out.reason="interval";carry_=0;return out;
        }
        ++stats_.intervals;
        const double spanMs=double(span)/10000.0;
        stats_.lastSpanMs=spanMs;
        if(spanMs>config_.maxIntervalMs){
            // Stall, load or pause. Never interpolate across it, and do not pay the
            // missed frames back afterwards: the credit is a rounding remainder,
            // not a debt.
            ++stats_.bypassLong;out.bypass=true;out.reason="longInterval";
            carry_=0;smoothedSpan_=0;
            return out;
        }
        // The picture already arrives at or above the target (menus, a real 60 FPS
        // source, a target below the source): nothing to insert, and a run would
        // only spend GPU on a frame that is a copy of B.
        if(std::isfinite(measuredContentFps)&&measuredContentFps>=config_.targetFps*0.98){
            ++stats_.bypassAtTarget;out.bypass=true;out.reason="sourceAtTarget";
            carry_=0;smoothedSpan_=0;
            return out;
        }
        double usedSpanMs=spanMs;
        if(config_.smoothing>0.0){
            const double low=std::min(config_.clampMinMs,config_.clampMaxMs),high=std::max(config_.clampMinMs,config_.clampMaxMs);
            const double clamped=std::clamp(spanMs,low,high);
            if(smoothedSpan_<=0.0)smoothedSpan_=clamped;
            else smoothedSpan_+=(clamped-smoothedSpan_)*std::clamp(config_.smoothing,0.0,1.0);
            usedSpanMs=smoothedSpan_;
        }
        stats_.lastSmoothedSpanMs=usedSpanMs;
        const double want=usedSpanMs*config_.targetFps/1000.0+carry_;
        long long count=std::llround(want)-1;
        if(count<0)count=0;
        if(count>static_cast<long long>(config_.maxRuns)){
            stats_.droppedByCap+=uint64_t(count)-config_.maxRuns;
            count=config_.maxRuns;
            carry_=0;                                  // lost frames, not owed frames
        }else{
            // The remainder is bounded to one frame of credit either way: a single
            // mis-measured interval must not swing the next group by more than one.
            carry_=std::clamp(want-double(count+1),-1.0,1.0);
        }
        out.count=unsigned(count);
        out.outputMultiplier=std::max(2u,out.count+1u);
        for(unsigned i=0;i<out.count;++i)out.t[i]=double(i+1)/double(out.outputMultiplier);
        out.reason=out.count?"adaptive":"none";
        stats_.lastTicks=out.count;
        stats_.lastCarry=carry_;
        stats_.ticks+=out.count;
        return out;
    }

    const Stats& stats()const{return stats_;}

private:
    FixedTargetConfig config_{};
    Stats stats_{};
    double carry_=0;           // fractional output frame owed to the next pair
    double smoothedSpan_=0;
};
}
