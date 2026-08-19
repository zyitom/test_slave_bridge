#pragma once

// Host side of the shared USB-SOF time base (firmware/rmcs_board/SOF_TIMEBASE.md).
//
// One process-wide axis, deliberately. Every board resolves the wrap of its own
// hardware microframe counter against the anchor this object produces, so two
// boards agree in ABSOLUTE terms only if both anchors came from the same origin.
// A per-board axis would leave each board internally consistent and mutually
// offset by whole seconds -- the failure that is hardest to notice, because
// every board would report a plausible timeline.
//
// Note what the anchor does NOT have to be: accurate. A board keeps the low 14
// bits from FRINDEX and takes only the wrap from the host, so the anchor may be
// off by up to +-1.024 s without changing the result on any board. Host clock
// error therefore cannot degrade cross-board synchronisation. It can only make
// the mapping to wall-clock time wrong, which is a separate and much weaker
// requirement.
//
// UNIX TIME. observe() builds a least-squares fit of (microframe -> host
// steady_clock) from the boards' own reports, timestamped at the midpoint of the
// anchor/status round trip so the USB transit bias mostly cancels. Wall-clock
// conversion then adds a single steady->system offset sampled ONCE, at
// construction: tracking it live would import every NTP step and slew straight
// into the timeline. Consequences worth stating plainly:
//
//   * alignment to THIS MACHINE's Unix clock as it was at startup: microseconds;
//   * alignment to true UTC: whatever this machine's own clock discipline is
//     worth, which for plain NTP is milliseconds. A microsecond-accurate UTC
//     mapping needs PTP or a PPS reference, and no amount of work here supplies
//     it.

#include <chrono>
#include <cstdint>
#include <mutex>

#include <librmcs/export.hpp>

namespace librmcs::host::time {

class LIBRMCS_API Timeline {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr std::chrono::nanoseconds kMicroframePeriod{125'000};

    // Samples kept for the fit. At one exchange per board per keepalive period
    // (250 ms) and two boards that is a ~32 s window: long enough that the
    // ~50 us of USB round-trip jitter averages down to single-digit microseconds
    // of offset error, short enough to follow the crystals apart.
    static constexpr std::size_t kSampleCapacity = 256;

    static Timeline& instance();

    // The anchor to send to every board this round. Open loop against the
    // process origin until the fit has samples, fitted afterwards -- the
    // handover matters because an open-loop axis drifts against the real
    // microframe rate by the host crystal's error, which would eventually walk
    // past the +-1.024 s the wrap resolution tolerates.
    uint64_t anchor_for(Clock::time_point when) const;
    uint64_t anchor_now() const { return anchor_for(Clock::now()); }

    // One board's kTimeStatus, timestamped at the midpoint of the round trip
    // that produced it.
    void observe(uint64_t microframe, Clock::time_point sampled_at);

    bool locked() const;
    std::size_t sample_count() const;

    // Fitted host time of a microframe, and the inverse. Both fall back to the
    // open-loop origin before the fit has converged.
    Clock::time_point host_time_of(uint64_t microframe) const;
    std::chrono::system_clock::time_point unix_time_of(uint64_t microframe) const;
    uint64_t microframe_at_unix(std::chrono::system_clock::time_point when) const;

    // Nanoseconds per microframe as measured against this host's steady clock;
    // 125000 exactly would mean the two crystals agree. Zero before lock.
    double measured_period_ns() const;

private:
    Timeline();

    struct Sample {
        uint64_t microframe;
        int64_t host_ns;
    };

    void refit_locked();

    mutable std::mutex mutex_;
    Clock::time_point origin_;
    std::chrono::system_clock::time_point unix_origin_;

    Sample samples_[kSampleCapacity];
    std::size_t sample_head_ = 0;
    std::size_t sample_count_ = 0;

    bool fitted_ = false;
    uint64_t fit_reference_microframe_ = 0;
    double fit_reference_ns_ = 0.0;
    double fit_period_ns_ = 0.0;
};

// Shorthand for Timeline::instance().
LIBRMCS_API Timeline& timeline();

} // namespace librmcs::host::time
