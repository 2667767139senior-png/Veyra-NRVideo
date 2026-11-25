#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace veyra::engine {
// Fixed-target frame generation ("固定目标帧率") for the NVIDIA VFG backend.
// See docs/FIXED_TARGET_FG_2026-11-25.md.
//
// Multiplier mode puts the generated frames at t = i/M inside every source
// interval, so the output rate follows the picture: a 30 FPS game that dips to
// 25 FPS turns a 60 FPS output into 50, and that cadence change is what the eye
// reads as a stutter. Fixed-target mode instead runs the OUTPUT on a free
// running grid of 1/targetFps and generates whatever grid positions fall inside
// each real interval, so the output rate stays put and only the number of
// generated frames per interval changes (33 ms -> 2.7, 40 ms -> 3.2, 50 ms -> 4
// ticks at 80 FPS).
//
// Everything here is pure arithmetic on the source clock in 100 ns units. The
// presentation timeline (engine/PresentationScheduler.h) maps a frame PTS to a
// host deadline with an affine transform, so ticks that are uniform in source
// time are uniform on the panel too and no second output clock is needed.
//
// Why the real frame takes the next grid point: a grid that free-runs across
// intervals cannot put every real frame on a grid point, so leaving the real
// frame at its own PTS would break the cadence once per interval - for the one
// frame the eye is most sensitive to, because it is also the newest. Snapping
// it forward by less than one period (<= 12.5 ms at 80 FPS) costs a fraction of
// a tick of latency and keeps every submitted interval at one period.
//
// On the t band: the first tick of an interval is always at least one period
// after the previous real frame's slot, so t starts at period/span and rises to
// 1 - period/span. For a 25-60 Hz source at a 60-120 Hz target that is 0.2-0.6
// to 0.8-0.95, and nearStart never triggers. It exists for the pair after a
// reset, where the grid has just been re-anchored.
//
// A near-END band (dropping ticks above t = 0.95 and letting the real frame
// cover them) was tried and removed: dropping a tick whose only alternative is
// the real frame one period later leaves a two-period hole, which is the exact
// stutter this mode exists to remove. A tick at t = 0.998 costs one VFG run and
// shows a picture 0.2 % away from B, which the eye cannot tell from B.
inline constexpr unsigned kFixedTargetMaxTicks=6;
// 100 ns per second, the unit of every PTS in this project.
inline constexpr int64_t kFixedTargetUnit=10000000;

struct FixedTargetConfig {
    bool enabled=false;
    double targetFps=80.0;
    // Interval smoothing strength, 0..1, 0 = off (the default). Clamping and the
    // exponential average exist only for this feature; with 0 the exact source
    // span is used, so a generated frame sits at its true temporal position.
    double smoothing=0.0;
    double clampMinMs=25.0;
    double clampMaxMs=60.0;
    // A pair longer than this is a stall, a load or a pause: no interpolation,
    // hold the last picture and re-synchronise the grid.
    double maxIntervalMs=100.0;
    // Runs per interval. Past this the extra ticks are dropped rather than
    // generated: the output clock keeps running and a real or already generated
    // frame covers the hole, so a slow GPU never accumulates latency.
    unsigned maxRuns=4;
    // A tick at or below this t is A, which is still on screen: no run is spent on
    // it. Only reachable on the pair after a reset (see the note above).
    double nearStart=0.05;

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
        c.maxRuns>=1&&c.maxRuns<=kFixedTargetMaxTicks&&
        std::isfinite(c.nearStart)&&c.nearStart>=0.0&&c.nearStart<0.5;
}

// Runs per interval a target rate needs at the slowest content this mode is
// meant for (25 FPS = 40 ms): 40 ms at 120 FPS is 4.8 ticks, so four runs would
// drop a grid point in every interval. 80 FPS comes out at four, the historical
// default; callers keep maxRuns in step with the target instead of hard-coding
// it, so typing 144 FPS does not silently arrive capped at 4.
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

// Generated slots are parity + 2*(sub-1) and VFG allocates 2*(M-1) of them, so
// a fixed-target graph must be built with a multiplier that covers maxRuns. The
// user's multiplier is meaningless in this mode; the larger of the two is kept
// so the model family and the memory estimate stay the ones they chose.
inline unsigned fixedTargetCapacity(const FixedTargetConfig& c,unsigned requested){
    if(!c.enabled)return requested;
    const unsigned needed=std::min(c.maxRuns,kFixedTargetMaxTicks)+1u;
    return std::max(requested,needed);
}

struct FixedTargetPlan {
    static constexpr unsigned Capacity=kFixedTargetMaxTicks;
    unsigned count=0;
    std::array<double,Capacity> t{};        // VFG Timestep of each tick, in (0,1)
    std::array<int64_t,Capacity> pts100ns{};
    // Where the real frame is submitted. Fixed-target: the first grid point at or
    // after the pair's B, so it sits on the same grid as the ticks. On a bypass
    // it is B itself.
    int64_t realPts100ns=0;
    bool bypass=false;
    const char* reason="off";
};

// Runs on the engine thread, once per accepted pair, in source-clock order.
class FixedTargetScheduler {
public:
    struct Stats {
        uint64_t intervals=0,ticks=0,droppedByCap=0;
        uint64_t bypassLong=0,bypassAtTarget=0,bypassInvalid=0,bypassOff=0,resyncs=0;
        unsigned lastTicks=0;
        double lastSpanMs=0,lastSmoothedSpanMs=0;
    };

    void configure(const FixedTargetConfig& config){
        // Re-anchor when the mode turns on or the period changes; a re-configure
        // with the same period keeps the grid, so an unrelated settings edit does
        // not restart the output clock.
        const bool reanchor=!config_.enabled||(config.enabled&&config.targetFps!=config_.targetFps);
        config_=config;
        if(reanchor){phaseValid_=false;smoothedSpan_=0;}
    }
    const FixedTargetConfig& config()const{return config_;}
    void reset(){const auto config=config_;*this=FixedTargetScheduler{};config_=config;}

    FixedTargetPlan plan(int64_t a100ns,int64_t b100ns,double measuredContentFps){
        FixedTargetPlan out;
        out.realPts100ns=b100ns;
        if(!config_.enabled){out.reason="off";return out;}
        // A zero target is the panel's "insert nothing" value. The grid period
        // would be 1/0, so it has to be caught before the division, and the pair
        // is submitted exactly as it came in.
        if(!(config_.targetFps>0.0)){
            ++stats_.bypassOff;out.bypass=true;out.reason="targetOff";
            phaseValid_=false;return out;
        }
        const int64_t span=b100ns-a100ns;
        if(span<=0){
            ++stats_.bypassInvalid;out.bypass=true;out.reason="interval";
            phaseValid_=false;return out;
        }
        ++stats_.intervals;
        const auto period=std::max<int64_t>(1,std::llround(double(kFixedTargetUnit)/config_.targetFps));
        stats_.lastSpanMs=double(span)/10000.0;
        if(span>int64_t(config_.maxIntervalMs*10000.0)){
            // Stall, load or pause. Never interpolate across it, and move the grid
            // to the frame that follows instead of catching up on missed ticks.
            ++stats_.bypassLong;++stats_.resyncs;out.bypass=true;out.reason="longInterval";
            phase_=b100ns;phaseValid_=true;smoothedSpan_=0;
            return out;
        }
        // The picture already arrives at or above the output rate (menus, a real
        // 60 FPS source, a target below the source): nothing to insert, and a run
        // would only add delay. This is the automatic bypass.
        if(std::isfinite(measuredContentFps)&&measuredContentFps>=config_.targetFps*0.98){
            ++stats_.bypassAtTarget;out.bypass=true;out.reason="sourceAtTarget";
            phase_=b100ns;phaseValid_=true;smoothedSpan_=0;
            return out;
        }
        // Denominator of t. With smoothing off it is the exact span, so a tick at
        // source time T lands at t = (T-a)/span and the motion speed is exact by
        // construction. Smoothing trades a little of that exactness for a steadier
        // speed while a game keeps flipping between 30 and 25 FPS.
        int64_t denominator=span;
        if(config_.smoothing>0.0){
            const int64_t low=std::llround(config_.clampMinMs*10000.0),high=std::llround(config_.clampMaxMs*10000.0);
            const int64_t clamped=std::clamp(span,std::min(low,high),std::max(low,high));
            if(smoothedSpan_<=0)smoothedSpan_=double(clamped);
            else smoothedSpan_+=(double(clamped)-smoothedSpan_)*std::clamp(config_.smoothing,0.0,1.0);
            denominator=std::max<int64_t>(1,std::llround(smoothedSpan_));
        }
        stats_.lastSmoothedSpanMs=double(denominator)/10000.0;
        if(!phaseValid_){phase_=a100ns+period;phaseValid_=true;}
        if(phase_<=a100ns)phase_+=((a100ns-phase_)/period+1)*period;
        for(auto tick=phase_;tick<b100ns;tick+=period){
            const double t=double(tick-a100ns)/double(denominator);
            phase_=tick+period;
            if(t<=config_.nearStart)continue;                // A is still on screen
            if(out.count>=config_.maxRuns||out.count>=FixedTargetPlan::Capacity){
                ++stats_.droppedByCap;continue;               // keep the clock, skip the work
            }
            out.t[out.count]=std::clamp(t,0.001,0.999);
            out.pts100ns[out.count]=tick;++out.count;++stats_.ticks;
        }
        // The loop leaves phase_ on the first grid point at or after B: every
        // iteration advances it by exactly one period. The real frame takes that
        // point, so it shares the grid with the ticks and the next pair starts one
        // period later - every submitted interval is then one period.
        out.realPts100ns=phase_;phase_+=period;
        stats_.lastTicks=out.count;
        out.reason=out.count?"ticks":"none";
        return out;
    }

    const Stats& stats()const{return stats_;}

private:
    FixedTargetConfig config_{};
    Stats stats_{};
    int64_t phase_=0;      // next free grid point
    bool phaseValid_=false;
    double smoothedSpan_=0;
};
}
