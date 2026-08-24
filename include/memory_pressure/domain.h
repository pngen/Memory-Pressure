#pragma once
// Pressure domain observations and interpreted domain state.
//
// An Observation is the raw, provider-reported truth.  DomainState is the
// interpretation produced by applying budget, reserves, policy, and hysteresis
// to an observation.  The runtime keeps observations and state separate so
// that providers can change without invalidating historical state.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "memory_pressure/budget.h"
#include "memory_pressure/types.h"

namespace memory_pressure {

// Raw provider-reported truth for a single domain.  Not all fields are
// meaningful for every provider; unobserved fields use std::nullopt so that
// "unknown" is never silently conflated with "zero".
struct DomainObservation {
    PressureDomainId id;
    DomainType type = DomainType::Unknown;
    std::string provider;                 // provider identity (e.g. "windows.host")
    std::string native_resource_id;       // stable native identity (e.g. device UUID)
    std::uint64_t total_capacity = 0;     // provider-reported total
    std::uint64_t committed = 0;          // committed / in-use bytes
    std::uint64_t resident = 0;           // resident bytes (where applicable)
    std::optional<std::uint64_t> reclaimable;     // provider-supplied reclaimable
    std::uint64_t unavailable = 0;        // unavailable bytes
    std::uint64_t available = 0;          // provider-reported free bytes
    std::optional<std::uint64_t> usable_capacity; // provider-reported usable ceiling
    std::optional<std::uint64_t> reserved;        // provider-reported reserved
    Confidence confidence = Confidence::Unknown;
    Provenance provenance = Provenance::Unknown;
    Validity validity = Validity::Valid;
    std::uint64_t observed_at_ms = 0;
    int allocation_failures = 0;          // recent allocation failure count
    double growth_bytes_per_s = 0.0;      // provider-supplied recent growth rate
    std::string detail;                   // free-form provider detail / error text
};

// A governing budget attached to a domain (budget may be absent -> no policy
// governance, only raw observation).
struct DomainBudgetAssignment {
    bool present = false;
    Budget budget;
};

// Interpreted pressure state for one domain produced by the runtime.
struct DomainState {
    PressureDomainId id;
    DomainType type = DomainType::Unknown;
    std::string provider;
    std::string native_resource_id;

    Budget budget;                 // governing budget (hard_capacity==0 => ungoverned)
    bool governed = false;         // true when a budget is active

    std::uint64_t total_capacity = 0;   // governed ceiling if governed, else provider total
    std::uint64_t usable_capacity = 0;  // after reserves
    std::uint64_t reserved_bytes = 0;   // reserves total
    std::uint64_t committed = 0;
    std::uint64_t resident = 0;
    std::uint64_t available = 0;
    std::uint64_t reclaimable = 0;
    double utilization = 0.0;     // committed / usable (0 when ungoverned/none)

    PressureLevel level = PressureLevel::Unknown;
    PressureLevel previous_level = PressureLevel::Unknown;
    bool in_hysteresis_hold = false;
    std::uint64_t level_hold_observations = 0;

    double score = 0.0;           // normalized [0,1]
    double utilization_component = 0.0;
    double free_deficit_component = 0.0;
    double reserve_deficit_component = 0.0;
    double growth_component = 0.0;
    double allocation_failure_component = 0.0;
    double stigma = 0.0;          // cumulative penalty (stale/confidence/etc.)

    TrendDirection trend = TrendDirection::Unknown;
    double trend_rate = 0.0;      // bytes/sec
    double trend_confidence = 0.0; // [0,1]
    std::uint64_t trend_window_samples = 0;

    Confidence confidence = Confidence::Unknown;
    Provenance provenance = Provenance::Unknown;
    Validity validity = Validity::Valid;
    std::uint64_t timestamp_ms = 0;

    std::vector<ResponseAction> responses;
    std::string explanation;      // structured but human-readable summary

    std::uint64_t entered_level_at_ms = 0;   // when current level was entered
    std::uint64_t peak_level = 0;            // severity rank of peak within current episode
};

// Serialization helpers (defined in serialize.cpp).
std::uint64_t compute_domain_available(const Budget& b, const DomainObservation& o) noexcept;

} // namespace memory_pressure
