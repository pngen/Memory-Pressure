#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "memory_pressure/budget.h"
#include "memory_pressure/domain.h"
#include "memory_pressure/events.h"
#include "memory_pressure/hysteresis.h"
#include "memory_pressure/policy.h"
#include "memory_pressure/providers/provider.h"
#include "memory_pressure/response.h"
#include "memory_pressure/snapshot.h"
#include "memory_pressure/velocity.h"

namespace memory_pressure {

// Aggregate telemetry counters collected by the runtime.
struct RuntimeStats {
    std::uint64_t observations = 0;
    std::uint64_t snapshots = 0;
    std::uint64_t pressure_transitions = 0;
    std::uint64_t provider_failures = 0;
    std::uint64_t stale_observations = 0;
    std::uint64_t admission_accept = 0;
    std::uint64_t admission_caution = 0;
    std::uint64_t admission_defer = 0;
    std::uint64_t admission_reject = 0;
    std::uint64_t backpressure_signals = 0;
    std::uint64_t reclaim_requests = 0;
    std::uint64_t demotion_requests = 0;
    std::uint64_t event_dropped = 0;
    std::uint64_t policy_changes = 0;
    std::uint64_t generation_changes = 0;
    std::uint64_t refresh_latency_us = 0;
    std::uint64_t query_count = 0;
};

// The main runtime.  Coordinates providers, budgets, policy, hysteresis,
// scoring, aggregation, responses, subscriptions, and telemetry.
//
// Thread-safety model:
//   * Current snapshot is an immutable shared_ptr<const Snapshot> and is
//     swapped atomically by refresh(); readers hold their own reference.
//   * Policy is an immutable shared_ptr<const PressurePolicy>.
//   * Budgets and domains are guarded by an internal mutex.
//   * refresh() is serialized by an internal mutex (single writer).
//   * Event callbacks are invoked outside all internal locks.
class PressureRuntime {
public:
    PressureRuntime();
    ~PressureRuntime();

    PressureRuntime(const PressureRuntime&) = delete;
    PressureRuntime& operator=(const PressureRuntime&) = delete;

    // --- Policy ---
    // Applies a validated policy atomically.  Returns the problem description
    // on invalid policy (and leaves the previous policy in place), else nullopt.
    std::optional<std::string> set_policy(PressurePolicy p);
    std::shared_ptr<const PressurePolicy> current_policy() const;

    // --- Providers ---
    void register_provider(std::shared_ptr<Provider> p);

    // --- Budgets ---
    // Attach a governed budget to a domain.  Returns invalid-budget error or
    // nullopt.
    std::optional<std::string> set_budget(const PressureDomainId& id, const Budget& b);
    void clear_budget(const PressureDomainId& id);

    // --- Subscriptions ---
    std::shared_ptr<Subscription> subscribe(SubscriptionFilter filter,
                                            std::function<void(const PressureEvent&)> cb = {});
    void close_subscription(const std::shared_ptr<Subscription>& s);

    // --- Refresh ---
    // Poll all providers and produce a new immutable snapshot.  Generation only
    // increments on material change.  Returns the new snapshot.
    std::shared_ptr<const Snapshot> refresh(std::uint64_t now_ms);

    // --- Queries (against the current snapshot) ---
    std::shared_ptr<const Snapshot> current_snapshot() const;
    // Look up a previously produced snapshot by id.  History is bounded.
    std::shared_ptr<const Snapshot> snapshot_by_id(std::uint64_t id) const;

    AdmissionHint admit(const PressureDomainId& id, std::uint64_t bytes) const;
    Backpressure backpressure(const PressureDomainId& id) const;
    std::string explain(const PressureDomainId& id) const;
    // Recommended responses / relief for a domain in the current snapshot.
    DomainResponse response_for(const PressureDomainId& id) const;

    // --- Telemetry ---
    const RuntimeStats& stats() const noexcept { return stats_; }

    // --- Bounds ---
    void set_history_limit(std::size_t n) noexcept { history_limit_ = n; }

private:
    struct DomainRuntimeState {
        DomainType type = DomainType::Unknown;
        std::string provider;
        std::string native_resource_id;
        Budget budget;
        bool governed = false;
        std::uint64_t entered_level_at_ms = 0;
        std::uint64_t peak_level_rank = 0;
        HysteresisState hysteresis;
        TrendEstimator trend;
        DomainRuntimeState(const HysteresisConfig& cfg, std::size_t win)
            : hysteresis(cfg), trend(win) {}
    };

    void append_trace(const Snapshot& snap);

    mutable std::mutex refresh_mutex_;
    mutable std::mutex current_mutex_;       // guards current_snapshot_
    std::shared_ptr<const PressurePolicy> policy_;
    std::shared_ptr<const Snapshot> current_snapshot_;

    // Budget configuration (written by set_budget, read by refresh).
    mutable std::mutex config_mutex_;
    std::unordered_map<PressureDomainId, Budget, PressureDomainIdHash> budgets_;

    std::unordered_map<PressureDomainId, std::shared_ptr<Provider>, PressureDomainIdHash> providers_;
    std::unordered_map<PressureDomainId, DomainRuntimeState, PressureDomainIdHash> domain_states_;
    std::map<std::string, ProviderStatus> provider_health_;

    mutable std::mutex registry_mutex_;       // protects providers_, subscriptions_
    std::vector<std::shared_ptr<Subscription>> subscriptions_;

    std::deque<std::shared_ptr<const Snapshot>> history_;
    std::size_t history_limit_ = 64;

    std::uint64_t generation_ = 0;
    std::uint64_t snapshot_id_ = 0;
    std::uint64_t event_id_ = 0;
    mutable std::mutex stats_mutex_;
    mutable RuntimeStats stats_;
    std::vector<PressureEvent> trace_events_;
};

} // namespace memory_pressure
