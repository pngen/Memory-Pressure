#pragma once
// Hysteresis / debounce state machine for a single pressure domain.
//
// Hysteresis is mandatory: without it a domain can flap around a threshold.
// This machine consumes a raw level (the threshold-band result) and returns a
// sticky, debounced resolved level.  It is deterministic for an identical
// sequence of raw levels and timestamps.

#include <cstdint>

#include "memory_pressure/policy.h"
#include "memory_pressure/types.h"

namespace memory_pressure {

// Determine the "raw" target level for a utilization given the current resolved
// level and the threshold set.  This encodes the enter/exit threshold
// hysteresis: escalation uses enter thresholds, de-escalation uses exit
// thresholds, and the band between them is sticky.
PressureLevel evaluate_level(double utilization, PressureLevel current, const Thresholds& t) noexcept;

class HysteresisState {
public:
    explicit HysteresisState(const HysteresisConfig& cfg);

    // Feed a raw level at the given time.  Returns the resolved level.
    PressureLevel update(PressureLevel raw, std::uint64_t now_ms);
    // The currently resolved level (Unknow before first observation).
    PressureLevel current() const noexcept { return level_; }
    bool in_hold() const noexcept { return pending_ != PressureLevel::Unknown; }
    std::uint32_t pending_observations() const noexcept { return pending_obs_; }
    void reset() noexcept;

private:
    HysteresisConfig cfg_;
    PressureLevel level_ = PressureLevel::Normal;
    PressureLevel pending_ = PressureLevel::Unknown;
    std::uint32_t pending_obs_ = 0;
    std::uint64_t pending_since_ms_ = 0;
};

} // namespace memory_pressure
