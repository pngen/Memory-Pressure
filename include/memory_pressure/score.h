#pragma once
// Versioned, normalized pressure scoring.
//
// The score is a *weighted* composite of normalized components.  It is not a
// universal truth; raw components are always exposed and weightings are
// configurable via policy.

#include <cstdint>

#include "memory_pressure/policy.h"
#include "memory_pressure/types.h"

namespace memory_pressure {

struct PressureScore {
    int score_version = 1;
    double value = 0.0;                 // weighted normalized [0,1]
    double utilization_component = 0.0;
    double free_deficit_component = 0.0;
    double reserve_deficit_component = 0.0;
    double growth_component = 0.0;
    double allocation_failure_component = 0.0;
    double confidence_penalty = 0.0;
    double stale_penalty = 0.0;
};

// Compute the versioned score for a domain given the raw observation, the
// governing budget, and the configured weights.  All components are normalized
// to [0,1] before weighting; the result is clamped to [0,1].
PressureScore score_domain(double utilization,
                           bool governed,
                           double free_fraction,
                           double reserve_deficit_fraction,
                           double growth_factor,
                           int allocation_failures,
                           Confidence confidence,
                           Validity validity,
                           const ScoreWeights& w);

} // namespace memory_pressure
