#pragma once
// Explicit pressure responses and the structured signals Memory Pressure emits
// for other runtimes to consume.
//
// Every action here is a *recommendation*: Memory Pressure declares how much
// relief is needed and how urgent it is, but never directly selects objects to
// reclaim/demote, moves bytes, or places work.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "memory_pressure/types.h"

namespace memory_pressure {

// A bounded, inspectable backpressure signal for a target domain.
struct Backpressure {
    PressureDomainId target;
    PressureLevel severity = PressureLevel::Unknown;
    std::string reason;
    ResponseAction recommended_response = ResponseAction::None;
    std::uint64_t max_new_allocation = 0;     // recommended max for one new allocation
    double admission_reduction = 0.0;          // [0,1] suggested reduction in admission rate
    std::uint64_t defer_duration_ms = 0;       // suggested defer duration
    std::uint64_t generation = 0;              // pressure generation that produced this
    std::uint64_t issued_at_ms = 0;
    std::uint64_t expiry_ms = 0;               // when this signal is stale
    Confidence confidence = Confidence::Unknown;
    TierRole target_role = TierRole::Root;     // positional hint in the tier hierarchy
};

// The result of a memory-specific admission query.
struct AdmissionHint {
    AdmissionDecision decision = AdmissionDecision::Reject;
    std::string explanation;
    std::uint64_t requested_bytes = 0;
    std::uint64_t safe_bytes = 0;          // max new allocation that is safe right now
    PressureLevel domain_level = PressureLevel::Unknown;
    bool reserve_available = false;
};

// A structured relief request.  Reclaim and demotion are the two archetypes.
struct ReliefRequest {
    enum class Kind : std::uint8_t { Reclaim = 0, Demotion = 1 };
    Kind kind = Kind::Reclaim;
    PressureDomainId domain;
    std::uint64_t target_bytes = 0;           // bytes to free / demote
    PressureLevel urgency = PressureLevel::Unknown;
    std::uint64_t generation = 0;
    std::string reason;
    std::uint64_t minimum_useful_relief = 0;  // below this the request is not useful
    std::optional<DomainType> preferred_tier; // destination tier for demotion
    std::uint64_t deadline_us = 0;            // deadline class (0 = none)
    Confidence confidence = Confidence::Unknown;

    bool is_reclaim() const noexcept { return kind == Kind::Reclaim; }
    bool is_demotion() const noexcept { return kind == Kind::Demotion; }
};

inline const char* to_string(ReliefRequest::Kind v) noexcept {
    return v == ReliefRequest::Kind::Reclaim ? "RECLAIM" : "DEMOTION";
}

// A policy-driven recommendation bundle for a domain at a given pressure state.
struct DomainResponse {
    PressureDomainId domain;
    PressureLevel level = PressureLevel::Unknown;
    std::vector<ResponseAction> actions;
    std::string explanation;
    std::optional<ReliefRequest> relief;   // set when reclaim/demotion is issued
};

} // namespace memory_pressure
