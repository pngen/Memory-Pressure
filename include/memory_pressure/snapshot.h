#pragma once
// Immutable-ish pressure snapshots and cross-snapshot diffing.
//
// Queries always operate against a specific snapshot, which avoids mutable
// global-state races.  A snapshot is created by the runtime and then treated
// as immutable by consumers.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "memory_pressure/domain.h"
#include "memory_pressure/types.h"

namespace memory_pressure {

// A short summary of the events observed while producing a snapshot.  Kept
// bounded so a snapshot's size is independent of event volume.
struct SnapshotEventSummary {
    std::vector<std::string> classes;   // event class names (bounded count)
    std::uint32_t count = 0;            // total events produced during the cycle
    std::uint32_t dropped = 0;          // events dropped due to queue bounds
};

struct Snapshot {
    std::uint64_t id = 0;
    std::uint64_t generation = 0;
    std::uint64_t timestamp_ms = 0;

    std::vector<DomainState> domains;                 // one interpreted domain per entry
    std::map<std::string, ProviderStatus> provider_health;

    PressureLevel aggregate_level = PressureLevel::Unknown;
    double aggregate_score = 0.0;
    std::string aggregate_explanation;

    std::uint64_t policy_version = 0;
    std::string policy_name;

    std::vector<std::string> warnings;
    bool stale_data = false;
    bool partial_data = false;
    Confidence confidence = Confidence::Unknown;
    Provenance provenance = Provenance::Unknown;

    SnapshotEventSummary events;

    // Look up an interpreted domain by id.  Returns nullptr when absent.
    const DomainState* find_domain(const PressureDomainId& id) const noexcept;
    DomainState* find_domain(const PressureDomainId& id) noexcept;
};

// Structural difference between two snapshots, used to drive generation bumps
// and events.  Only *material* differences are reported.
struct SnapshotDiff {
    std::vector<PressureDomainId> added_domains;
    std::vector<PressureDomainId> removed_domains;
    std::vector<PressureDomainId> level_changed;      // domains whose level changed
    std::vector<std::string> budget_changed;          // domain ids (hex) whose budget changed
    std::vector<PressureDomainId> reserve_changed;    // domains whose reserves changed
    bool aggregate_changed = false;
    bool policy_changed = false;
    bool provider_health_changed = false;
    bool any() const noexcept {
        return !added_domains.empty() || !removed_domains.empty() ||
               !level_changed.empty() || !budget_changed.empty() ||
               !reserve_changed.empty() || aggregate_changed ||
               policy_changed || provider_health_changed;
    }
};

SnapshotDiff diff_snapshots(const Snapshot& a, const Snapshot& b);

} // namespace memory_pressure
