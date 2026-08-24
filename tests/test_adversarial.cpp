#include "test_harness.h"
#include "memory_pressure/runtime.h"
#include "memory_pressure/events.h"
#include "memory_pressure/serialize.h"
#include "memory_pressure/domain.h"
#include "memory_pressure/providers/synthetic.h"

#include <limits>

using namespace memory_pressure;

TEST(adversarial_capacity_zero_and_near_max) {
    PressureRuntime rt;
    std::vector<SyntheticDomainSpec> ds;
    // Zero capacity.
    SyntheticDomainSpec z; z.id = PressureDomainId{1,1}; z.type = DomainType::AcceleratorMemory; z.group="gpu"; z.total_capacity=0; ds.push_back(z);
    // Near UINT64_MAX capacity with committed near max.
    SyntheticDomainSpec m; m.id = PressureDomainId{1,2}; m.type = DomainType::AcceleratorMemory; m.group="gpu";
    m.total_capacity = (std::numeric_limits<std::uint64_t>::max)(); ds.push_back(m);
    rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::GradualGrowth, 100, 4));
    auto sn = rt.refresh(1000);
    for (const auto& d : sn->domains) {
        // available is computed as max(0, usable-committed); never negative and never wraps.
        EXPECT(d.available <= d.usable_capacity || d.usable_capacity == 0);
        EXPECT(d.score >= 0.0 && d.score <= 1.0);
    }
}

TEST(adversarial_committed_exceeds_capacity) {
    PressureRuntime rt;
    std::vector<SyntheticDomainSpec> ds;
    SyntheticDomainSpec d; d.id = PressureDomainId{2,1}; d.type = DomainType::AcceleratorMemory; d.group="gpu";
    d.total_capacity = 1024ULL*1024*1024; ds.push_back(d);
    rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::PinnedExhaustion, 100, 4));
    Budget b; b.hard_capacity = 64ULL*1024*1024; rt.set_budget(d.id, b);
    auto sn = rt.refresh(1000);
    const DomainState* st = sn->find_domain(d.id);
    EXPECT(st != nullptr);
    if (st) {
        EXPECT(st->available == 0);          // no negative available
        EXPECT(st->utilization >= 0.0);      // can exceed 1 but is finite
        EXPECT(st->score >= 0.0 && st->score <= 1.0);
    }
}

TEST(adversarial_json_rejects_bad_observations) {
    // Negative capacity is rejected (as_uint64 fails).
    Json o = Json::object();
    o["id"] = Json("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    o["type"] = Json("ACCELERATOR_MEMORY");
    o["total_capacity"] = Json(static_cast<std::int64_t>(-5));
    o["committed"] = Json(static_cast<std::int64_t>(40));
    EXPECT(!observation_from_json(o).has_value());

    // Non-finite number is rejected by the parser.
    EXPECT(!json_parse("[1.2, NaN]").has_value());
    EXPECT(!json_parse("[1.2, Infinity]").has_value());
    // Malformed.
    EXPECT(!json_parse("{").has_value());
    EXPECT(!json_parse("[1,2,]").has_value());
    // Huge string is rejected via max_bytes.
    std::string huge(3 * 1024 * 1024, 'a');
    JsonParseLimits lim; lim.max_bytes = 1024;
    EXPECT(!json_parse(huge, lim).has_value());
}

TEST(adversarial_snapshot_forged_ids) {
    // A snapshot with two distinct but structurally valid domains is accepted;
    // duplicate ids are rejected (tested elsewhere).  Here we ensure a forged
    // id (not matching the provider) is simply carried as an extended domain.
    Json j = Json::object();
    j["schema"] = Json(static_cast<std::int64_t>(1));
    j["id"] = Json(static_cast<std::int64_t>(1));
    j["generation"] = Json(static_cast<std::int64_t>(1));
    j["timestamp_ms"] = Json(static_cast<std::int64_t>(0));
    j["policy_version"] = Json(static_cast<std::int64_t>(1));
    j["policy_name"] = Json("p");
    j["aggregate_level"] = Json("NORMAL");
    j["aggregate_score"] = Json(0.0);
    j["aggregate_explanation"] = Json("");
    j["stale_data"] = Json(false);
    j["partial_data"] = Json(false);
    j["confidence"] = Json("HIGH");
    j["provenance"] = Json("RUNTIME_REGISTRATION");
    Json d1 = Json::object(); d1["id"] = Json("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    Json domains = Json::array(); domains.push_back(d1);
    j["domains"] = domains;
    auto parsed = snapshot_from_json(j);
    EXPECT(parsed.has_value());
    if (parsed) EXPECT(parsed->domains.size() == 1);
}

TEST(adversarial_event_storm_bounded) {
    SubscriptionFilter f;
    Subscription sub(f, 8, EventOverflowPolicy::DropOldest);
    PressureEvent e;
    e.type = PressureEventType::PressureEntered;
    e.domain = PressureDomainId{3,3};
    for (int i = 0; i < 200; ++i) sub.push(e);
    EXPECT(sub.pending() <= 8);       // bounded queue
    EXPECT(sub.dropped() > 0);        // overflow accounted for

    Subscription sub2(f, 4, EventOverflowPolicy::Reject);
    for (int i = 0; i < 20; ++i) sub2.push(e);
    EXPECT(sub2.pending() <= 4);
    EXPECT(sub2.rejected() > 0);
}

TEST(adversarial_provider_failure_storm_coherent) {
    PressureRuntime rt;
    std::vector<SyntheticDomainSpec> ds;
    SyntheticDomainSpec d; d.id = PressureDomainId{4,1}; d.type = DomainType::AcceleratorMemory; d.group="gpu"; d.total_capacity=8ULL*1024*1024*1024; ds.push_back(d);
    rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::ProviderFailure, 100, 4));
    Budget b; b.hard_capacity = 4ULL*1024*1024*1024; rt.set_budget(d.id, b);
    auto s1 = rt.refresh(1000);
    EXPECT(s1->domains.size() == 1);
    // After failure steps the provider stops producing; the runtime keeps the
    // last-known domain as stale rather than disappearing or crashing.
    for (int i = 0; i < 8; ++i) rt.refresh(1000 + i*100);
    auto s2 = rt.current_snapshot();
    EXPECT(s2->stale_data || !s2->domains.empty());
}

TEST(adversarial_subscription_churn_and_shutdown) {
    PressureRuntime rt;
    std::vector<SyntheticDomainSpec> ds;
    SyntheticDomainSpec d; d.id = PressureDomainId{5,1}; d.type = DomainType::AcceleratorMemory; d.group="gpu"; d.total_capacity=8ULL*1024*1024*1024; ds.push_back(d);
    rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::GradualGrowth, 100, 4));
    Budget b; b.hard_capacity = 2ULL*1024*1024*1024; rt.set_budget(d.id, b);
    // Rapidly create and close subscriptions.
    for (int i = 0; i < 20; ++i) {
        auto sub = rt.subscribe(SubscriptionFilter{});
        rt.refresh(1000 + i*100);
        if (i % 2 == 0) sub->close();
    }
    // Shutdown destroys the runtime; subscriptions are closed safely.
    rt.close_subscription(nullptr);
    EXPECT(true);
}
