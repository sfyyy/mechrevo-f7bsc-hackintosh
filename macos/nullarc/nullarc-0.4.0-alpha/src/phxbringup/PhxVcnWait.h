// SPDX-License-Identifier: MIT
// Fence wait for the decode path: a short spin, then sub-millisecond sleeps,
// then the bring-up IOSleep(1) loop as a fallback. The hardware normally
// retires well inside one 1 ms sleep tick, so the old loop spent a full
// millisecond per sleep even when the fence was already moving (2026-11-23).
//
// The clock and the three waiting primitives are injected: the kernel passes
// mach_absolute_time, BAR0 polling, a pause instruction, IOLockSleepDeadline
// and IOSleep, while tests/vcn_host_test.cpp passes a simulated clock.
#ifndef PHX_VCN_WAIT_H
#define PHX_VCN_WAIT_H

#include <stdint.h>

namespace phx_vcn {

struct VcnWaitParams {
    uint32_t mode;        // 1 short sleep only, 2 spin then short sleep
    uint32_t timeoutMs;   // IOSleep(1) iterations before giving up
    uint32_t sliceUs;     // sub-millisecond sleep slice
    uint64_t spinMaxNs;   // longest spin
    uint64_t hwEwmaNs;    // moving average of the hardware time, P frames only
};

struct VcnWaitResult {
    uint32_t hit;         // 0 timeout, 1 spin (or already done), 2 short sleep, 3 IOSleep fallback
    bool preSleepHit;     // the fence was already in when the pre-sleep ended
    uint32_t sleeps;
    uint64_t spinNs;      // time spent spinning
    uint64_t elapsedNs;   // whole wait
    uint64_t hwEwmaNs;    // updated average, or the input when updateEwma is false
};

// A frame that finishes before the spin ends is a spin hit; one that needs the
// short sleeps is a short hit; the fallback only runs when the short-sleep
// budget is exhausted, which also keeps a dead fence from spinning forever.
constexpr uint64_t kVcnWaitShortBudgetNs = 20000000ull;   // 20 ms
constexpr uint64_t kVcnWaitPreSleepGuardNs = 100000ull;   // do not pre-sleep for a fast fence
constexpr uint64_t kVcnWaitShortSliceMaxNs = 1000000ull;  // 1 ms
// The average is bounded. The fallback stage can run for seconds, and an average
// that large would make every later frame pre-sleep that long while holding the
// decode lock; it also keeps the pre-sleep inside something a P frame can pay.
constexpr uint64_t kVcnWaitMaxHwNs = 4000000ull;          // 4 ms
constexpr uint64_t kVcnWaitMinHwNs = 200000ull;           // 200 us

template <class Clock, class Done, class Pause, class ShortSleep, class FallbackSleep>
inline VcnWaitResult vcnWaitFence(const VcnWaitParams &params, Clock nowNs, Done done,
                                  Pause pause, ShortSleep shortSleep, FallbackSleep fallbackSleep,
                                  bool updateEwma) {
    const uint64_t hwNs = params.hwEwmaNs < kVcnWaitMaxHwNs ? params.hwEwmaNs : kVcnWaitMaxHwNs;
    VcnWaitResult result{};
    result.hwEwmaNs = hwNs;
    const uint64_t started = nowNs();
    uint64_t spinNs = 0;
    if (params.mode >= 2) {
        const uint64_t budget = hwNs + hwNs / 4;   // 1.25 x the average
        spinNs = budget < params.spinMaxNs ? budget : params.spinMaxNs;
    }
    // Sleep up to just before the hardware is expected to finish, then spin the
    // last stretch: a long fence does not burn the CPU, a short one is not
    // rounded up to the next timer tick.
    const uint64_t preSleepNs = hwNs > spinNs + kVcnWaitPreSleepGuardNs ? hwNs - spinNs : 0;
    const uint64_t spinEndNs = preSleepNs + spinNs;
    bool complete = done();
    if (complete) result.hit = 1;   // already retired: the caller still needs a success
    if (!complete && preSleepNs) {
        shortSleep(preSleepNs);
        ++result.sleeps;
        complete = done();
        if (complete) {
            result.hit = 2;             // the sleep, not the spin, brought the fence in
            result.preSleepHit = true;  // ... and the pre-sleep was longer than the hardware
        }
    }
    const uint64_t spinStart = nowNs();
    while (!complete) {
        if (nowNs() - started >= spinEndNs) break;
        pause();
        complete = done();
        if (complete) result.hit = 1;
    }
    const uint64_t spinStop = nowNs();
    // Small slices catch a P frame quickly; the slice doubles so a slow IDR does
    // not turn into hundreds of wakeups.
    uint64_t sliceNs = static_cast<uint64_t>(params.sliceUs) * 1000ull;
    while (!complete && nowNs() - started < kVcnWaitShortBudgetNs) {
        shortSleep(sliceNs);
        ++result.sleeps;
        complete = done();
        if (complete) result.hit = 2;
        sliceNs *= 2;
        if (sliceNs > kVcnWaitShortSliceMaxNs) sliceNs = kVcnWaitShortSliceMaxNs;
    }
    for (uint32_t i = 0; !complete && i < params.timeoutMs; ++i) {
        complete = done();
        if (complete) {
            result.hit = 3;
            break;
        }
        fallbackSleep();
        ++result.sleeps;
    }
    // The fallback probes at the top of each turn and sleeps at the bottom, so a
    // fence that retires during the last sleep is only seen here.
    if (!complete) {
        complete = done();
        if (complete) result.hit = 3;
    }
    result.elapsedNs = nowNs() - started;
    result.spinNs = spinStop > spinStart ? spinStop - spinStart : 0;
    // Learning rule. A fence that is already in when the pre-sleep ends proves the
    // sleep was too long, so halve it: averaging it in would confirm the sleep
    // with itself and lock the average at the cap (measured 2026-11-24: one 9 ms
    // spike pinned every later frame at 8 ms of pre-sleep). Everything else is an
    // upper bound within one slice, so a plain 1/8 average is fine. A timeout
    // teaches nothing.
    if (updateEwma) {
        if (result.preSleepHit) {
            result.hwEwmaNs = hwNs > kVcnWaitMinHwNs ? hwNs / 2 : kVcnWaitMinHwNs;
            if (result.hwEwmaNs < kVcnWaitMinHwNs) result.hwEwmaNs = kVcnWaitMinHwNs;
        } else if (result.hit == 1 || result.hit == 2 || result.hit == 3) {
            const uint64_t sample = result.elapsedNs < kVcnWaitMaxHwNs ? result.elapsedNs : kVcnWaitMaxHwNs;
            result.hwEwmaNs = hwNs - (hwNs >> 3) + (sample >> 3);
        }
    }
    return result;
}

}   // namespace phx_vcn

#endif
