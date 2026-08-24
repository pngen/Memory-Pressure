#pragma once
// Versioned JSON serialization for policy, snapshots, events, observations,
// statistics, and pressure traces.  The reader rejects malformed, oversized,
// non-finite, unknown-schema-version, and semantically-impossible input.
//
// Capacity values are serialized as integers and read back exactly for values
// below 2^53 (so capacities through ~9 PB round-trip losslessly); see
// LIMITATIONS.md.

#include <cstdint>
#include <optional>
#include <string>
#include "memory_pressure/budget.h"
#include "memory_pressure/domain.h"
#include "memory_pressure/events.h"
#include "memory_pressure/json.h"
#include "memory_pressure/policy.h"
#include "memory_pressure/runtime.h"
#include "memory_pressure/snapshot.h"
#include "memory_pressure/trace.h"

namespace memory_pressure {

// Current schema versions.  Increment when the wire format changes.
inline constexpr std::int64_t kSnapshotSchemaVersion = 1;
inline constexpr std::int64_t kPolicySchemaVersion = 1;
inline constexpr std::int64_t kEventSchemaVersion = 1;
inline constexpr std::int64_t kTraceSchemaVersion = 1;

// --- policy ---
Json policy_to_json(const PressurePolicy& p);
std::optional<PressurePolicy> policy_from_json(const Json& j);

// --- budget / reserve ---
Json budget_to_json(const Budget& b);
std::optional<Budget> budget_from_json(const Json& j);

// --- observations / domain state ---
Json observation_to_json(const DomainObservation& o);
std::optional<DomainObservation> observation_from_json(const Json& j);
Json domain_state_to_json(const DomainState& d);

// --- snapshot ---
Json snapshot_to_json(const Snapshot& s);
std::optional<Snapshot> snapshot_from_json(const Json& j);

// --- events / stats ---
Json event_to_json(const PressureEvent& e);
Json stats_to_json(const RuntimeStats& s);

// --- trace ---
Json trace_to_json(const PressureTrace& t);
std::optional<PressureTrace> trace_from_json(const Json& j);

} // namespace memory_pressure
