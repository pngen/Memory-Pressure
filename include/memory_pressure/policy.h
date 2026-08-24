#pragma once
// Versioned pressure policy: thresholds, hysteresis, scoring weights,
// aggregate rules, response rules, provider freshness, and event bounds.
//
// A policy is plain immutable data.  It is validated before publication and
// referenced by version on every snapshot.

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "memory_pressure/types.h"

namespace memory_pressure {

// Utilization thresholds (fractions in [0,1]) for each pressure transition.
// Threshold comparison uses "reach on >= enter, leave on <= exit" so that a
// non-empty window of separation produces hysteresis.
struct Thresholds {
    double normal_exit    = 0.40;   // leave ELEVATED -> NORMAL
    double elevated_enter = 0.60;   // enter ELEVATED
    double elevated_exit  = 0.50;   // leave ELEVATED -> NORMAL
    double high_enter     = 0.85;   // enter HIGH
    double high_exit      = 0.78;   // leave HIGH -> ELEVATED
    double critical_enter = 0.92;   // enter CRITICAL
    double critical_exit  = 0.86;   // leave CRITICAL -> HIGH
    double exhausted_enter = 0.985; // enter EXHAUSTED
    double exhausted_exit  = 0.93;  // leave EXHAUSTED -> CRITICAL

    // Re-entrancy guard: raw utilizations above this are treated conservatively.
};

// Hysteresis / debounce configuration.  Transitions are sticky: a raw
// threshold crossing must be confirmed before pressure state changes.
struct HysteresisConfig {
    std::uint32_t min_dwell_observations = 2;   // confirmations required
    double min_dwell_duration_ms = 0.0;          // minimum time in a level
    double escalation_delay_ms = 0.0;            // delay before escalating
    double recovery_delay_ms = 0.0;              // delay before recovering
    bool immediate_emergency_escalation = true;  // bypass delay (not dwell) on EXHAUSTED
    std::uint32_t max_debounce_observations = 4; // bound on confirmations before acting
};

// Normalized score-component weights.  All must be finite and non-negative.
struct ScoreWeights {
    double utilization = 1.0;
    double free_deficit = 1.0;
    double reserve_deficit = 1.0;
    double growth_rate = 0.5;
    double allocation_failure = 2.0;
    double fragmentation = 0.25;
    double reclaimable_deficit = 0.5;
    double confidence_penalty = 0.5;
    double stale_penalty = 0.75;
};

// Aggregate pressure modeling configuration.
struct AggregateConfig {
    bool consider_hierarchy = true;
    std::map<TierRole, double> role_weight;      // role multiplier (default 1.0)
    double bottleneck_penalty = 1.0;             // extra weight on the max-criticality domain
    double reserve_exhaustion_penalty = 1.25;    // extra weight if a critical domain is reserve-exhausted
    double response_feasibility_penalty = 0.75;  // discount if a response is infeasible
};

// A single response rule: when a domain reaches a given level, emit actions.
struct ResponseRule {
    PressureLevel level = PressureLevel::High;
    std::vector<ResponseAction> actions;
};

// Provider freshness bounds (microseconds).  A provider that exceeds its
// freshness bound is treated as stale.
struct FreshnessConfig {
    std::uint64_t default_max_age_us = 10 * 1000000ULL; // 10 s default
    std::map<std::string, std::uint64_t> per_provider_max_age_us; // provider name -> bound
};

// The complete versioned policy.
struct PressurePolicy {
    std::uint64_t version = 1;
    std::string name = "default";
    Thresholds thresholds;
    HysteresisConfig hysteresis;
    ScoreWeights weights;
    AggregateConfig aggregate;
    FreshnessConfig freshness;
    std::vector<ResponseRule> response_rules;
    std::uint64_t max_queued_events = 4096;
    std::uint64_t max_relief_bytes = (std::numeric_limits<std::uint64_t>::max)(); // bound on relief requests

    // Serialization/snapshot identification.
    std::string revision();

    constexpr bool operator==(const PressurePolicy& o) const noexcept {
        return version == o.version && name == o.name &&
               thresholds.elevated_enter == o.thresholds.elevated_enter &&
               thresholds.high_enter == o.thresholds.high_enter &&
               thresholds.critical_enter == o.thresholds.critical_enter &&
               thresholds.exhausted_enter == o.thresholds.exhausted_enter;
    }
};

// Validation.  Returns an empty optional when valid, otherwise the first
// problem described in prose.
std::optional<std::string> validate_policy(const PressurePolicy& p) noexcept;

// A conservative, sane default policy.
PressurePolicy default_policy();

} // namespace memory_pressure
