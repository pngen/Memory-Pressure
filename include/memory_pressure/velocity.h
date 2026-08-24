#pragma once
// Bounded, conservative trend / velocity estimation.
//
// Pressure is not only current utilization.  Memory Pressure keeps a short,
// bounded window of (time, committed) samples and derives a conservative slope
// plus a *projection* to a target threshold when the fit is reliable enough.
// It deliberately stops short of speculative forecasting.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

#include "memory_pressure/types.h"

namespace memory_pressure {

struct TrendEstimate {
    TrendDirection direction = TrendDirection::Unknown;
    double rate = 0.0;                 // bytes/second (positive = growth)
    double confidence = 0.0;           // [0,1]
    std::size_t samples = 0;
    std::optional<std::uint64_t> projected_healthy_ms; // ms until a target util is reached? handled outside
};

class TrendEstimator {
public:
    explicit TrendEstimator(std::size_t window_samples = 16, std::uint64_t max_window_age_ms = 60 * 1000ULL);

    void add_sample(std::uint64_t committed, std::uint64_t timestamp_ms);
    void reset();

    TrendEstimate estimate() const noexcept;

private:
    struct Sample { std::uint64_t t_ms; std::uint64_t committed; };
    std::size_t window_samples_;
    std::uint64_t max_window_age_ms_;
    std::deque<Sample> samples_;
};

} // namespace memory_pressure
