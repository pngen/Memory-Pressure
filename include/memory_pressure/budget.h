#pragma once
// Governed budgets and explicit reserves.
//
// A budget is the *governed* capacity ceiling for a domain.  It may be lower
// than the physical device maximum.  Reserves reduce allocatable headroom, so
// a domain can be under pressure even when the raw device reports free bytes.
//
// Budgets and reserves are plain data; the runtime applies policy on top of
// them.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "memory_pressure/types.h"

namespace memory_pressure {

enum class ReserveKind : std::uint8_t {
    Emergency           = 0,
    Migration           = 1,
    Checkpoint          = 2,
    TransferStaging     = 3,
    Recovery            = 4,
    Fragmentation       = 5,
    System              = 6,
    Application         = 7,
    Custom              = 8
};

inline const char* to_string(ReserveKind v) noexcept {
    switch (v) {
        case ReserveKind::Emergency:       return "EMERGENCY";
        case ReserveKind::Migration:       return "MIGRATION";
        case ReserveKind::Checkpoint:      return "CHECKPOINT";
        case ReserveKind::TransferStaging: return "TRANSFER_STAGING";
        case ReserveKind::Recovery:        return "RECOVERY";
        case ReserveKind::Fragmentation:   return "FRAGMENTATION";
        case ReserveKind::System:          return "SYSTEM";
        case ReserveKind::Application:     return "APPLICATION";
        case ReserveKind::Custom:          return "CUSTOM";
    }
    return "CUSTOM";
}

inline std::optional<ReserveKind> reserve_kind_from_string(std::string_view s) noexcept {
    if (s == "EMERGENCY")       return ReserveKind::Emergency;
    if (s == "MIGRATION")       return ReserveKind::Migration;
    if (s == "CHECKPOINT")      return ReserveKind::Checkpoint;
    if (s == "TRANSFER_STAGING") return ReserveKind::TransferStaging;
    if (s == "RECOVERY")        return ReserveKind::Recovery;
    if (s == "FRAGMENTATION")   return ReserveKind::Fragmentation;
    if (s == "SYSTEM")          return ReserveKind::System;
    if (s == "APPLICATION")     return ReserveKind::Application;
    if (s == "CUSTOM")          return ReserveKind::Custom;
    return std::nullopt;
}

// A reserve is a portion of governed capacity that is not available for
// general allocation.
struct Reserve {
    ReserveKind kind = ReserveKind::Custom;
    std::string name;
    std::uint64_t bytes = 0;

    constexpr bool operator==(const Reserve& o) const noexcept {
        return kind == o.kind && name == o.name && bytes == o.bytes;
    }
};

// A governed budget for a single pressure domain.
struct Budget {
    std::uint64_t hard_capacity = 0;        // governed ceiling (can be < physical max)
    std::optional<std::uint64_t> soft_capacity; // soft limit; above this triggers soft pressure
    std::vector<Reserve> reserves;          // explicit reserves reducing headroom

    std::uint64_t emergency_reserve_bytes = 0; // convenience: emergency reserve amount
    std::uint64_t minimum_free_reserve = 0;    // hard floor that must remain free
    std::uint64_t reclaim_target = 0;          // desired bytes to reclaim under stress
    std::uint64_t demotion_target = 0;         // desired bytes to demote under stress
    std::uint64_t admission_headroom = 0;      // headroom reserved for new work

    std::string owner;                       // configured owner / policy label
    std::uint64_t version = 0;               // budget version (bumped on change)

    // Total bytes held aside by explicit reserves.
    std::uint64_t reserve_bytes() const noexcept {
        std::uint64_t total = emergency_reserve_bytes;
        for (const auto& r : reserves) total += r.bytes;
        return total;
    }

    // The capacity that is actually available for general allocation after
    // reserves are set aside.
    std::uint64_t usable_capacity() const noexcept {
        const std::uint64_t res = reserve_bytes();
        return hard_capacity >= res ? hard_capacity - res : 0;
    }

    // Available headroom given a committed amount.
    std::uint64_t available(std::uint64_t committed) const noexcept {
        const std::uint64_t usable = usable_capacity();
        return committed < usable ? usable - committed : 0;
    }

    constexpr bool operator==(const Budget& o) const noexcept {
        return hard_capacity == o.hard_capacity &&
               soft_capacity == o.soft_capacity &&
               reserve_bytes() == o.reserve_bytes() &&
               emergency_reserve_bytes == o.emergency_reserve_bytes &&
               minimum_free_reserve == o.minimum_free_reserve &&
               reclaim_target == o.reclaim_target &&
               demotion_target == o.demotion_target &&
               admission_headroom == o.admission_headroom &&
               owner == o.owner && version == o.version;
    }
};

// Validate a budget for internal consistency.  Returns an empty optional when
// valid, otherwise a human-readable description of the first problem found.
std::optional<std::string> validate_budget(const Budget& b) noexcept;

// Build a standard domain budget.
Budget default_budget();

} // namespace memory_pressure
