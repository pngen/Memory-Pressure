#pragma once
// Bounded pressure event model and subscriptions.
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "memory_pressure/types.h"
namespace memory_pressure {
enum class PressureEventType : std::uint8_t {
    DomainAdded       = 0, DomainRemoved = 1, PressureEntered = 2,
    PressureEscalated = 3, PressureRelieved = 4, PressureRecovered = 5,
    BudgetChanged = 6, ReserveChanged = 7, ProviderStale = 8,
    ProviderFailed = 9, ProviderRecovered = 10, BackpressureIssued = 11,
    ReclaimRequested = 12, DemotionRequested = 13, PolicyChanged = 14,
    GenerationChanged = 15, Custom = 16
};
inline const char* to_string(PressureEventType v) noexcept {
    switch (v) {
        case PressureEventType::DomainAdded: return "DOMAIN_ADDED";
        case PressureEventType::DomainRemoved: return "DOMAIN_REMOVED";
        case PressureEventType::PressureEntered: return "PRESSURE_ENTERED";
        case PressureEventType::PressureEscalated: return "PRESSURE_ESCALATED";
        case PressureEventType::PressureRelieved: return "PRESSURE_RELIEVED";
        case PressureEventType::PressureRecovered: return "PRESSURE_RECOVERED";
        case PressureEventType::BudgetChanged: return "BUDGET_CHANGED";
        case PressureEventType::ReserveChanged: return "RESERVE_CHANGED";
        case PressureEventType::ProviderStale: return "PROVIDER_STALE";
        case PressureEventType::ProviderFailed: return "PROVIDER_FAILED";
        case PressureEventType::ProviderRecovered: return "PROVIDER_RECOVERED";
        case PressureEventType::BackpressureIssued: return "BACKPRESSURE_ISSUED";
        case PressureEventType::ReclaimRequested: return "RECLAIM_REQUESTED";
        case PressureEventType::DemotionRequested: return "DEMOTION_REQUESTED";
        case PressureEventType::PolicyChanged: return "POLICY_CHANGED";
        case PressureEventType::GenerationChanged: return "GENERATION_CHANGED";
        case PressureEventType::Custom: return "CUSTOM";
    }
    return "CUSTOM";
}
struct PressureEvent {
    std::uint64_t id = 0;
    PressureEventType type = PressureEventType::Custom;
    std::uint64_t generation = 0;
    std::uint64_t timestamp_ms = 0;
    std::optional<PressureDomainId> domain;
    std::string detail;
    std::uint32_t severity_rank = 0;
};
struct SubscriptionFilter {
    std::vector<PressureDomainId> domains;
    std::vector<DomainType> domain_types;
    std::vector<PressureLevel> severities;
    std::vector<PressureEventType> event_types;
};
enum class EventOverflowPolicy : std::uint8_t { DropNewest = 0, DropOldest = 1, Reject = 2 };
class Subscription {
public:
    Subscription(SubscriptionFilter filter, std::size_t max_queue = 256,
                 EventOverflowPolicy overflow = EventOverflowPolicy::DropOldest,
                 std::function<void(const PressureEvent&)> callback = {});
    ~Subscription();
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    bool push(const PressureEvent& e);
    bool try_pop(PressureEvent& out);
    std::size_t pending() const;
    void set_callback(std::function<void(const PressureEvent&)> cb);
    void close();
    bool closed() const;
    void clear();
    const SubscriptionFilter& filter() const noexcept { return filter_; }
    bool matches(const PressureEvent& e) const;
    std::uint64_t dropped() const noexcept { return dropped_; }
    std::uint64_t rejected() const noexcept { return rejected_; }
    std::uint64_t delivered() const noexcept { return delivered_; }
private:
    SubscriptionFilter filter_;
    std::size_t max_queue_;
    EventOverflowPolicy overflow_policy_;
    mutable std::mutex mutex_;
    std::deque<PressureEvent> queue_;
    std::function<void(const PressureEvent&)> callback_;
    bool closed_ = false;
    std::uint64_t delivered_ = 0;
    std::uint64_t dropped_ = 0;
    std::uint64_t rejected_ = 0;
};
struct SubscriptionStats {
    std::size_t active = 0;
    std::uint64_t delivered = 0;
    std::uint64_t dropped = 0;
    std::uint64_t rejected = 0;
};
} // namespace memory_pressure
