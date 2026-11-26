#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace veyra::engine {
// Adaptive fixed-rate frame generation (AFG) for the NVIDIA VFG backend.
// See docs/AFG_2026-11-26.md.
//
// Multiplier mode spends a constant M-1 runs on every source interval, so the
// output rate follows the picture: 30 FPS content that dips to 25 FPS drags a
// 60 FPS output down to 50, and that cadence change is what the eye reads as a
// stutter.
//
// AFG instead owns an OUTPUT CLOCK, independent of when input arrives:
//
//     slot(n) = origin + round(n * 1000/targetFps)        [source-clock 100 ns]
//
// Every slot is one output frame. The real frames are assigned to slots too
// ("snapped up": a real frame takes the first slot at or after its own
// timestamp, so it is never shown early), and every slot strictly between two
// real frames is filled by a VFG run at
//
//     t = (slot - a) / (b - a)
//
// This is what makes the output cadence uniform rather than merely
// average-correct: at 30 FPS in and 50 FPS out the grid asks for
// 20/33.3 = 0.6, then 0.2 and 0.8, then 0.4 - positions no integer multiplier
// can express, which is why this mode needs VFG's explicit-timestep selector
// (FrameMultiplier=0 plus Timestep) and why the earlier count-only version
// could only alternate 16.7/33.3 ms.
//
// Properties this is built to keep:
//  - Bounded latency: snapping up delays a real frame by at most one period
//    (20 ms at 50 FPS). Cadence::Latency turns snapping off for the cost of an
//    uneven union of grid slots and real timestamps.
//  - No debt: a stall, a long interval or a lost slot is dropped, never repaid,
//    so the queue cannot grow after a hiccup.
//  - No drift: slot times are computed from the origin, never accumulated.
//  - Pure arithmetic on the source clock: no device work, no allocation, so it
//    runs per pair on the engine thread.
inline constexpr unsigned kFixedTargetMaxTicks=7;   // VFG's 8-wide group minus the real frame
// The lookahead ceiling, in output periods. Six periods is 120 ms at 50 FPS: the
// loop below only ever asks for what measured lateness demands, and this is the
// bound that keeps a pathological measurement from turning into a stalled output.
inline constexpr unsigned kFixedTargetMaxGuardPeriods=6;
// 100 ns per second, the unit of every PTS in this project.
inline constexpr int64_t kFixedTargetUnit=10000000;

// How the real frames meet the output grid.
enum class FixedTargetCadence : unsigned {
    // Real frames are moved onto the grid (never earlier). Uniform output,
    // at most one period of added latency. The product default.
    Fixed=0,
    // Real frames keep their own timestamps; only the gaps are filled. Zero
    // added latency, but the output is the union of two clocks and is therefore
    // not perfectly even. Kept for latency-critical use.
    Latency=1,
};

// How much a single input interval may hold back. See fixedTargetStallMs and
// fixedTargetPolicyCap for the two numbers each one actually changes.
enum class FixedTargetBufferPolicy : unsigned {
    // Lowest latency: give up on a gap at 60 ms and never ask for more than two
    // generated frames per pair.
    Tight=0,
    // Rate-derived cap (up to seven) and a 100 ms stall threshold. The default.
    Balanced=1,
    // Willing to interpolate across a 250 ms gap, which keeps the picture moving
    // through a hitch but spends more GPU on the pair that recovers from it.
    Relaxed=2,
};

struct FixedTargetConfig {
    bool enabled=false;
    // 50 FPS: the target this mode was written for (25-30 FPS content from a
    // capture card). Any 1-480 value is accepted; the panel edits 0-160.
    double targetFps=50.0;
    FixedTargetCadence cadence=FixedTargetCadence::Fixed;
    // A pair longer than this is a stall, a load or a pause: no interpolation
    // across it, and the grid re-anchors on the next real frame.
    double maxIntervalMs=100.0;
    // Generated frames per pair. Past this the extra slots are dropped rather
    // than generated: a GPU that cannot keep up must lose frames, not latency.
    unsigned maxRuns=2;
    // How much one input interval may hold back (fixedTargetStallMs /
    // fixedTargetPolicyCap). No policy lets slots accumulate: a growing queue is
    // latency, and latency is the failure this mode exists to avoid.
    FixedTargetBufferPolicy buffer=FixedTargetBufferPolicy::Balanced;
    // Cap this pair's frame count by the measured VFG cost, so a GPU that cannot
    // fill the target loses frames instead of falling behind. Only ever lowers
    // the count the rate asked for.
    bool gpuLoadAdapt=true;
    // A pair whose explicit-timestep evaluation fails is retried once at the plain
    // 2X position rather than failing the whole frame. Off = report the failure
    // (the engine then takes its normal FG recovery path).
    bool fallbackMultiplier=true;

    bool operator==(const FixedTargetConfig&)const=default;
};

inline bool fixedTargetValid(const FixedTargetConfig& c){
    if(!c.enabled)return true;
    // 0 is accepted and means "no generation" (plan() reports targetOff): the
    // panel offers the type-in field 0-160, and a target that low can only be
    // read as a request to stop inserting rather than as an error.
    return std::isfinite(c.targetFps)&&c.targetFps>=0.0&&c.targetFps<=480.0&&
        std::isfinite(c.maxIntervalMs)&&c.maxIntervalMs>=20.0&&c.maxIntervalMs<=1000.0&&
        c.maxRuns>=1&&c.maxRuns<=kFixedTargetMaxTicks&&
        unsigned(c.buffer)<=2u;
}

// The gap that counts as a stall, per buffer policy.
inline double fixedTargetStallMs(FixedTargetBufferPolicy p){
    return p==FixedTargetBufferPolicy::Tight?60.0:p==FixedTargetBufferPolicy::Relaxed?250.0:100.0;
}
// The per-pair ceiling, applied on top of the rate-derived cap.
inline unsigned fixedTargetPolicyCap(FixedTargetBufferPolicy p,unsigned rateCap){
    return p==FixedTargetBufferPolicy::Tight?std::min(rateCap,2u):rateCap;
}

// Frames per pair a target rate needs at the slowest content this mode is meant
// for (25 FPS = 40 ms): 40 ms at 120 FPS is 4.8 frames, so a cap of four would
// drop one in every interval. 50 FPS comes out at two (40 ms x 50 = 2.0 frames),
// 80 at four, 160 at seven - capped by kFixedTargetMaxTicks. Callers keep
// maxRuns in step with the target instead of hard-coding it, so typing 144 FPS
// does not silently arrive capped at 4.
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
    // Generated frames for this pair, in presentation order.
    unsigned count=0;
    // count+1, i.e. the VFG group width this pair asks for. 1 means "this pair
    // generates nothing", which the graph turns into zero VFG runs.
    unsigned outputMultiplier=1;
    // Presentation time of each generated frame, already on the output grid.
    std::array<int64_t,Capacity> pts100ns{};
    // VFG Timestep for the same frame: (pts - a) / (b - a), strictly inside (0,1).
    std::array<double,Capacity> timestep{};
    // Grid index of each generated frame and of the real frame, so the per-frame
    // trace can name a slot instead of an absolute time: "slot 1234" survives a
    // re-anchor, "29640 ms" does not.
    std::array<long long,Capacity> slotIndex{};
    long long realSlot=0;
    // Presentation time to use for the real frame (0 = keep its own timestamp).
    // Slots are snapped up, so this is at or after b, plus the guard below.
    int64_t realPts100ns=0;
    // Presentation lookahead added to EVERY frame of this run, in 100 ns.
    //
    // A generated frame between A and B cannot exist before B arrives, so a slot
    // at a+period is due in media time roughly one input interval before its own
    // inputs do. Without this shift the presenter shows it as soon as it is ready
    // and the output alternates late/early around a correct mean - measured
    // 2026-11-26 on a 30 -> 50 run (RTX 3080 Laptop, 1440p): P50 19.9 ms but
    // P95 27.1 ms, i.e. 27/13 alternation, which is exactly the judder this mode
    // exists to remove. Shifting the whole output by the same number of periods
    // restores the spacing and costs exactly that delay.
    //
    // Whole periods, so the output stays on the slot lattice. Applied only to the
    // fixed cadence: the latency policy deliberately keeps the source clock.
    //
    // The base is one input interval (that is what it takes for the second half
    // of a pair to exist), and the engine adds measured periods on top
    // (FixedTargetScheduler::setGuardExtraPeriods) until no output frame is
    // presented late. A fixed base is not enough: on the 2026-11-26 720p30 run
    // the base gave 26.7 ms of lead against a measured 24-30 ms graph time, so
    // one batch in three still missed by ~7 ms.
    int64_t guard100ns=0;
    bool bypass=false;
    const char* reason="off";
    // Slots this pair wanted but did not get, for the log: past the per-pair cap,
    // or too close to an endpoint for a meaningful interpolation.
    unsigned droppedCap=0,droppedNear=0;
    // The subset of droppedCap that the measured GPU cost removed (gpuLoadAdapt).
    unsigned droppedLoad=0;
    bool operator==(const FixedTargetPlan&)const=default;
};

// Runs on the engine thread, once per accepted pair, in source-clock order.
class FixedTargetScheduler {
public:
    struct Stats {
        uint64_t intervals=0,generated=0,droppedCap=0,droppedNear=0,resyncs=0;
        uint64_t bypassLong=0,bypassAtTarget=0,bypassInvalid=0,bypassOff=0;
        // Slots removed because the measured VFG cost said the GPU could not take
        // them. This is the number the load adaptation exists to make visible.
        uint64_t droppedLoad=0,loadLimitedPairs=0;
        unsigned lastGenerated=0;
        double lastSpanMs=0,lastPeriodMs=20.0;
        // How far the real frame was pushed onto the grid. This IS the latency
        // the cadence policy adds, in milliseconds.
        double lastSnapMs=0;
        // The lookahead the fixed cadence adds to every frame (whole periods).
        double lastGuardMs=0;
        // Base plus the measured extra, in periods; 1..6.
        unsigned lastGuardPeriods=0;
        int64_t lastSlotIndex=0;
    };

    void configure(const FixedTargetConfig& config){
        // A rate change keeps the grid: re-anchoring on every slider drag would
        // restart the slot numbering mid-play. A disable does clear it.
        if(!config.enabled)anchored_=false;
        config_=config;
    }
    const FixedTargetConfig& config()const{return config_;}
    void reset(){
        // The measured extra survives a reset: a device or frame reset is exactly
        // when the output would otherwise stall for the two seconds it takes the
        // loop to climb back to a lookahead this hardware already needed.
        const auto config=config_;const unsigned extra=requestedExtra_;
        *this=FixedTargetScheduler{};config_=config;requestedExtra_=extra;
    }
    // Measured load cap: the engine sets this from VFG's own per-frame GPU cost
    // (the Fg1 stage P95) once a second. 0 disables the artificial cap, and the
    // policy is off entirely when config().gpuLoadAdapt is false. It can only
    // lower the count the rate asked for - a target the GPU can meet is never
    // reduced by a measurement, only by a real cost that does not fit.
    void setLoadCap(unsigned cap){loadCap_=cap;}
    // Extra lookahead periods on top of the one-interval base (0..3). The engine
    // derives this from the measured lateness of the frames it actually
    // presented, so the delay is the smallest one that measurement calls for
    // instead of a constant picked for the worst case.
    //
    // A change takes effect on the next pair and shifts the whole output by that
    // many periods: exactly one interval is long (increase) or short (decrease)
    // at the transition, never accumulating. The engine moves this at most one
    // period per control window, with a much slower decrease than increase.
    void setGuardExtraPeriods(unsigned extra){
        extra=std::min(extra,kFixedTargetMaxGuardPeriods-1u);
        if(extra==requestedExtra_)return;
        requestedExtra_=extra;
        refreshGuard();
    }
    // The extra that is actually in force: the request minus whatever the ceiling
    // left no room for, so the log and the panel never claim a period the grid is
    // not really shifted by.
    unsigned guardExtraPeriods()const{return appliedExtra_;}
    // The lookahead one input interval costs on this grid, in periods: the part
    // that is not adaptive.
    unsigned guardBasePeriods()const{return guardSlots_;}
    // The ceiling plan() applies to a pair, before its own capacity check.
    unsigned effectiveMaxRuns()const{
        if(!config_.gpuLoadAdapt||!loadCap_)return config_.maxRuns;
        return std::min(loadCap_,config_.maxRuns);
    }

    // a100ns/b100ns: the previous and current real frame timestamps.
    // measuredContentFps: the engine's smoothed input rate, used only to recognise
    // a source that already runs at or above the target.
    FixedTargetPlan plan(int64_t a100ns,int64_t b100ns,double measuredContentFps){
        FixedTargetPlan out;
        // Off is a bypass like any other: nothing is planned, and the log line reads
        // the same field whatever the reason was.
        if(!config_.enabled){out.bypass=true;out.reason="off";return out;}
        // A zero target is the panel's "insert nothing" value; catching it here
        // keeps the conversion to a period out of a degenerate range.
        if(!(config_.targetFps>0.0)){
            ++stats_.bypassOff;out.bypass=true;out.reason="targetOff";anchored_=false;return out;
        }
        const int64_t span=b100ns-a100ns;
        if(span<=0){
            ++stats_.bypassInvalid;out.bypass=true;out.reason="interval";anchored_=false;return out;
        }
        const double spanMs=double(span)/10000.0;
        stats_.lastSpanMs=spanMs;
        if(spanMs>config_.maxIntervalMs){
            // Stall, load or pause. Never interpolate across it, and do not pay the
            // missed slots back afterwards: the grid re-anchors on the next frame.
            ++stats_.bypassLong;out.bypass=true;out.reason="longInterval";
            anchored_=false;return out;
        }
        // The picture already arrives at or above the target (menus, a real 60 FPS
        // source, a target below the source): nothing to insert, and a run would
        // only spend GPU on a frame that is a copy of B.
        if(std::isfinite(measuredContentFps)&&measuredContentFps>=config_.targetFps*0.98){
            ++stats_.bypassAtTarget;out.bypass=true;out.reason="sourceAtTarget";
            anchored_=false;return out;
        }
        const double period=double(kFixedTargetUnit)/config_.targetFps;
        if(!anchored_){
            // The first real frame of a run owns slot 0: the output clock starts
            // where the picture does, so no slot is ever in the past.
            origin_=a100ns;lastSlot_=0;anchored_=true;++stats_.resyncs;
            // Lookahead, in whole periods: one input interval is what it takes for
            // the second half of a pair to exist. Clamped to 1..3 periods so a
            // pathological first interval cannot stall the output.
            // (Named slotSpan, not "slots": Qt defines that as an empty macro, and
            // this header is included from Qt translation units.)
            const double slotSpan=std::ceil(spanMs/(period/10000.0)-1e-9);
            guardSlots_=static_cast<unsigned>(std::llround(std::clamp(slotSpan,1.0,3.0)));
            periodTicks_=static_cast<int64_t>(std::llround(period));
            refreshGuard();
        }
        // The latency policy keeps the source clock, so it takes no lookahead (its
        // output is the union of two clocks by design).
        const int64_t guard=config_.cadence==FixedTargetCadence::Fixed?guardTicks_:0;
        out.guard100ns=guard;
        const auto slotTime=[&](long long index){
            return origin_+static_cast<int64_t>(std::llround(double(index)*period));
        };
        // First slot at or after b: the real frame is never shown early.
        // The epsilon matters: when b lands exactly on a slot (it does every few
        // pairs whenever 1000/targetFps is not a whole 100 ns count) the division
        // can come out as 18.000000000000004 and ceil would hand this pair an
        // empty plan plus a real frame one slot late - a 40 ms hole at 50 FPS.
        long long slotB=static_cast<long long>(std::ceil(double(b100ns-origin_)/period-1e-6));
        // Never behind the previous real frame: two inputs faster than the target
        // would otherwise claim the same slot and the batch would go backwards.
        if(slotB<=lastSlot_)slotB=lastSlot_+1;
        // Guard against a degenerate origin (b before the anchor after a bad
        // timestamp): snap forward instead of planning negative work.
        if(slotB<1)slotB=1;

        // Only a degenerate position is refused. A 2% guard was tried first and
        // cost 2% of the slots on a 29.97 -> 50 grid (a slot lands 0.2 ms from A
        // every few pairs), which is a hole in the cadence - the one thing this
        // mode exists to prevent. A frame 0.2 ms from A is a repeat, not a hole.
        constexpr double minT=0.005;
        // Local scratch: one slot per tick the plan can hold.
        constexpr unsigned kCap=FixedTargetPlan::Capacity;
        std::array<int64_t,kCap> pts{};
        std::array<double,kCap> steps{};
        unsigned found=0;
        for(long long index=lastSlot_+1;index<slotB;++index){
            const int64_t at=slotTime(index);
            double t=double(at-a100ns)/double(span);
            // A slot exactly on an endpoint has nothing to interpolate. Anything
            // else is clamped into the selector's open range rather than dropped:
            // a 0.2 ms error in position is invisible, a hole in the cadence is
            // not. (At 160 FPS this fires on about 1% of slots.)
            if(t<=0.0||t>=1.0){++out.droppedNear;++stats_.droppedNear;continue;}
            t=std::clamp(t,minT,1.0-minT);
            if(found==kCap){++out.droppedCap;++stats_.droppedCap;continue;}
            pts[found]=at;steps[found]=t;++found;
        }
        unsigned first=0;
        const unsigned limit=effectiveMaxRuns();
        if(found>limit){
            // Keep the newest slots: they are the ones still worth presenting, and
            // dropping the oldest leaves one hole instead of a growing backlog.
            const unsigned excess=found-limit;
            if(limit<config_.maxRuns){
                // The GPU said it cannot take them: this is the adaptation working,
                // not a configuration error, so it gets its own counter.
                out.droppedLoad+=excess;stats_.droppedLoad+=excess;++stats_.loadLimitedPairs;
            }else{
                out.droppedCap+=excess;
                stats_.droppedCap+=excess;
            }
            first=found-limit;found=limit;
        }
        out.count=found;
        for(unsigned i=0;i<found;++i){out.pts100ns[i]=pts[first+i]+guard;out.timestep[i]=steps[first+i];out.slotIndex[i]=lastSlot_+1+first+i;}
        out.outputMultiplier=found+1u;
        out.realSlot=slotB;
        if(config_.cadence==FixedTargetCadence::Fixed){
            out.realPts100ns=slotTime(slotB)+guard;
            stats_.lastSnapMs=double(out.realPts100ns-guard-b100ns)/10000.0;
        }else stats_.lastSnapMs=0.0;
        stats_.lastGuardMs=double(guard)/10000.0;
        lastSlot_=slotB;
        out.reason=found?"grid":"none";
        stats_.lastGenerated=found;
        stats_.lastPeriodMs=period/10000.0;
        stats_.lastSlotIndex=slotB;
        stats_.generated+=found;
        ++stats_.intervals;
        return out;
    }

    const Stats& stats()const{return stats_;}

private:
    // Base periods + measured extra, in 100 ns. Called at the anchor and on every
    // extra change; plan() only reads the result so the whole run of a pair uses
    // one guard value.
    void refreshGuard(){
        if(!anchored_){guardTicks_=0;appliedExtra_=0;return;}
        const unsigned periods=std::min(guardSlots_+requestedExtra_,kFixedTargetMaxGuardPeriods);
        guardTicks_=int64_t(periods)*periodTicks_;
        appliedExtra_=periods-guardSlots_;
        stats_.lastGuardPeriods=periods;
    }

    FixedTargetConfig config_{};
    Stats stats_{};
    bool anchored_=false;
    int64_t origin_=0;
    long long lastSlot_=0;
    // Lookahead in 100 ns, set when the grid anchors (see FixedTargetPlan::guard100ns).
    int64_t guardTicks_=0;
    // Base lookahead in periods (what one input interval costs on the grid) and one
    // period in 100 ns, kept so a change of the extra can recompute guardTicks_
    // without waiting for the next anchor.
    unsigned guardSlots_=0;
    int64_t periodTicks_=2000000;
    unsigned requestedExtra_=0;
    unsigned appliedExtra_=0;
    unsigned loadCap_=0;
};

// Measures what actually left the presenter, which is the only number that
// answers "is the output even": the interval between consecutive Present calls.
// A correct average with a bad tail is exactly the failure this project must not
// report as success, so the percentiles are recorded next to it.
class AfgOutputPace {
public:
    static constexpr unsigned Capacity=256;
    // 2 ms buckets up to 30 ms, then everything longer. The shape is what tells
    // "uniform" apart from "correct mean": a 27/13 alternation has a perfect mean
    // and two peaks, and no percentile alone makes that obvious.
    static constexpr unsigned Buckets=16;
    struct Summary {
        unsigned samples=0;
        double p50=0,p95=0,p99=0,mean=0,stddev=0;
        // Intervals that were not one period: a late frame, a dropped slot or a
        // stall. Counted against the target period, with a 10% tolerance, inside
        // the same window the percentiles come from.
        unsigned offCadence=0;
        std::array<unsigned,Buckets> histogram{};
        double bucketMs=2.0;
    };
    void reset(){count_=0;head_=0;last_=0;}
    void record(int64_t now,double periodMs){
        if(last_>0&&now>last_){
            const double interval=double(now-last_)/10000.0;
            if(interval>0.0&&interval<1000.0){
                const bool off=periodMs>0.0&&std::abs(interval-periodMs)>periodMs*0.1;
                if(count_<Capacity){samples_[(head_+count_)%Capacity]=interval;off_[((head_+count_)%Capacity)]=off?1:0;++count_;}
                else{samples_[head_]=interval;off_[head_]=off?1:0;head_=(head_+1)%Capacity;}
            }
        }
        last_=now;
    }
    Summary summary()const{
        Summary out;
        out.samples=count_;
        if(!count_)return out;
        std::array<double,Capacity> sorted{};
        double sum=0;
        for(unsigned i=0;i<count_;++i){
            const double v=samples_[(head_+i)%Capacity];
            sorted[i]=v;sum+=v;
            out.offCadence+=off_[(head_+i)%Capacity]?1u:0u;
            const double bucket=v/out.bucketMs;
            ++out.histogram[bucket>=double(Buckets-1)?Buckets-1:unsigned(bucket)];
        }
        std::sort(sorted.begin(),sorted.begin()+count_);
        const auto at=[&](double q){return sorted[std::min(count_-1,unsigned(q*double(count_-1)+0.5))];};
        out.p50=at(0.50);out.p95=at(0.95);out.p99=at(0.99);
        out.mean=sum/double(count_);
        double var=0;
        for(unsigned i=0;i<count_;++i){const double d=sorted[i]-out.mean;var+=d*d;}
        out.stddev=std::sqrt(var/double(count_));
        return out;
    }
private:
    std::array<double,Capacity> samples_{};
    std::array<uint8_t,Capacity> off_{};
    unsigned count_=0,head_=0;
    int64_t last_=0;
};

// How late each output frame reached Present, measured against its own slot.
// This is what the lookahead loop is driven by, and deliberately not a model of
// the pipeline: it measures the symptom (a frame that missed its slot, and by
// how much) so it keeps working when the GPU, the driver, the source rate or the
// scene changes underneath it. Positive = late; a frame presented early (which
// the slot wait prevents anyway) is recorded as its negative value and ignored
// by the p95 the loop reads.
class AfgLateness {
public:
    static constexpr unsigned Capacity=128;
    // One control window's worth of frames at 50 FPS, so the summary follows a
    // change inside a second or two rather than averaging it away.
    static constexpr double kLateMs=1.0;
    struct Summary {
        unsigned samples=0,missed=0;
        double p50=0,p95=0,worst=0,mean=0;
    };
    void reset(){count_=0;}
    void add(double ms){
        if(!std::isfinite(ms))return;
        if(count_<Capacity)samples_[count_++]=ms;
        else{
            std::memmove(samples_.data(),samples_.data()+1,sizeof(double)*(Capacity-1));
            samples_[Capacity-1]=ms;
        }
    }
    Summary summary()const{
        Summary out;
        out.samples=count_;
        if(!count_)return out;
        std::array<double,Capacity> sorted{};
        double sum=0;
        for(unsigned i=0;i<count_;++i){
            sorted[i]=samples_[i];sum+=samples_[i];
            if(samples_[i]>kLateMs)++out.missed;
        }
        std::sort(sorted.begin(),sorted.begin()+count_);
        const auto at=[&](double q){return sorted[std::min(count_-1,unsigned(q*double(count_-1)+0.5))];};
        out.p50=at(0.50);out.p95=at(0.95);out.worst=sorted[count_-1];
        out.mean=sum/double(count_);
        return out;
    }
private:
    std::array<double,Capacity> samples_{};
    unsigned count_=0;
};
}
