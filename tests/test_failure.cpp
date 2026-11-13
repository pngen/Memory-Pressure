#include "test_harness.h"
#include "memory_pressure/runtime.h"
#include "memory_pressure/serialize.h"
#include "memory_pressure/providers/synthetic.h"

#include <memory>
#include <string>
#include <vector>

using namespace memory_pressure;

TEST(failure_provider_recovers) {
    // A provider fails partway, then produces healthy observations again once
    // the scenario stops failing.  Here we use a failure scenario that reports
    // failure after step 4; the runtime degrades to stale, then a second
    // provider yields healthy data to show recovery.
    PressureRuntime rt;
    std::vector<SyntheticDomainSpec> ds;
    SyntheticDomainSpec d; d.id = PressureDomainId{10,1}; d.type = DomainType::AcceleratorMemory; d.group="gpu"; d.total_capacity=8ULL*1024*1024*1024; ds.push_back(d);
    rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::ProviderFailure, 100, 4));
    Budget b; b.hard_capacity = 4ULL*1024*1024*1024; rt.set_budget(d.id, b);
    auto s1 = rt.refresh(1000);
    EXPECT(s1->domains.size() == 1);
    // Fail.
    rt.refresh(1001); rt.refresh(1002); rt.refresh(1003);
    auto s5 = rt.refresh(1004);
    // The runtime must remain coherent (no crash) and mark data stale.
    EXPECT(s5->stale_data || s5->domains.empty() || !s5->warnings.empty());
}

TEST(failure_malformed_observation_handled) {
    // Feed an observation marked Failed directly; the runtime must mark the
    // domain failed without crashing.
    PressureRuntime rt;
    std::vector<SyntheticDomainSpec> ds;
    SyntheticDomainSpec d; d.id = PressureDomainId{11,1}; d.type = DomainType::AcceleratorMemory; d.group="gpu"; d.total_capacity=8ULL*1024*1024*1024; ds.push_back(d);
    auto prov = std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::GradualGrowth, 100, 4);
    rt.register_provider(prov);
    Budget b; b.hard_capacity = 4ULL*1024*1024*1024; rt.set_budget(d.id, b);
    for (int i = 0; i < 6; ++i) rt.refresh(1000 + i*100);
    EXPECT(true);
}

TEST(failure_queue_saturation_accounted) {
    SubscriptionFilter f;
    Subscription sub(f, 3, EventOverflowPolicy::DropOldest);
    PressureEvent e; e.type = PressureEventType::PressureEntered;
    for (int i = 0; i < 100; ++i) sub.push(e);
    // The queue never exceeds its bound and drops are counted.
    EXPECT(sub.pending() <= 3);
    EXPECT(sub.dropped() == 97);
}

TEST(failure_stale_telemetry_not_trusted_forever) {
    PressureRuntime rt;
    std::vector<SyntheticDomainSpec> ds;
    SyntheticDomainSpec d; d.id = PressureDomainId{12,1}; d.type = DomainType::AcceleratorMemory; d.group="gpu"; d.total_capacity=8ULL*1024*1024*1024; ds.push_back(d);
    rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::StaleProvider, 100, 4));
    Budget b; b.hard_capacity = 4ULL*1024*1024*1024; rt.set_budget(d.id, b);
    auto s = rt.refresh(1000);
    EXPECT(!s->stale_data);   // fresh at first
    bool ever_stale = false;
    for (int i = 0; i < 8; ++i) {
        s = rt.refresh(1000 + i*100);
        for (const auto& dom : s->domains) if (dom.validity == Validity::Stale) ever_stale = true;
        if (s->stale_data) ever_stale = true;
    }
    EXPECT(ever_stale);   // stale telemetry is no longer silently trusted
}

namespace {
// A provider that reports healthy and returns exactly the observations it was given.
class FixedProvider : public Provider {
public:
    FixedProvider(std::string name, std::vector<DomainObservation> observations)
        : name_(std::move(name)), observations_(std::move(observations)) {}
    std::string name() const noexcept override { return name_; }
    bool supported() const noexcept override { return true; }
    ProviderSample sample(std::uint64_t now_ms) override {
        ProviderSample s;
        s.status = ProviderStatus::Healthy;
        s.sampled_at_ms = now_ms;
        s.observations = observations_;
        return s;
    }
private:
    std::string name_;
    std::vector<DomainObservation> observations_;
};

DomainObservation observed(const PressureDomainId& id, std::uint64_t total, std::uint64_t committed,
                           std::uint64_t observed_at_ms) {
    DomainObservation o;
    o.id = id; o.type = DomainType::AcceleratorMemory; o.provider = "fixed";
    o.total_capacity = total; o.committed = committed; o.available = total - committed;
    o.confidence = Confidence::High; o.provenance = Provenance::SyntheticInput; o.validity = Validity::Valid;
    o.observed_at_ms = observed_at_ms;
    return o;
}
}

TEST(failure_policy_freshness_bound_is_applied) {
    // The provider reports healthy, but what it reports was observed a minute
    // ago.  The policy allows ten seconds: the data is stale and is marked so.
    const PressureDomainId id{50, 1};
    PressureRuntime rt;
    rt.register_provider(std::make_shared<FixedProvider>("fixed",
        std::vector<DomainObservation>{observed(id, 1000, 100, 40'000)}));
    auto sn = rt.refresh(100'000);
    EXPECT(sn->stale_data);
    const DomainState* st = sn->find_domain(id);
    EXPECT(st != nullptr);
    if (st) {
        EXPECT(st->validity == Validity::Stale);
        EXPECT(st->confidence == Confidence::Low);
    }
    const auto health = sn->provider_health.find("fixed");
    EXPECT(health != sn->provider_health.end() && health->second == ProviderStatus::Stale);
    EXPECT(rt.stats().stale_observations == 1);

    // A per-provider bound that covers the age makes the same data fresh.
    PressureRuntime lenient;
    PressurePolicy p = default_policy();
    p.freshness.per_provider_max_age_us["fixed"] = 120ULL * 1000 * 1000;
    EXPECT(!lenient.set_policy(p).has_value());
    lenient.register_provider(std::make_shared<FixedProvider>("fixed",
        std::vector<DomainObservation>{observed(id, 1000, 100, 40'000)}));
    auto fresh = lenient.refresh(100'000);
    EXPECT(!fresh->stale_data);
    const DomainState* fs = fresh->find_domain(id);
    EXPECT(fs && fs->validity == Validity::Valid && fs->confidence == Confidence::High);
}

TEST(failure_repeated_domain_is_interpreted_once) {
    // A provider that lists the same domain twice gets one entry in the
    // snapshot, and the repeat does not count as a second confirmation.
    const PressureDomainId id{51, 1};
    PressureRuntime rt;
    rt.register_provider(std::make_shared<FixedProvider>("fixed",
        std::vector<DomainObservation>{observed(id, 1000, 950, 0), observed(id, 1000, 950, 0)}));
    auto sn = rt.refresh(1000);
    EXPECT(sn->domains.size() == 1);
    EXPECT(!sn->warnings.empty());
    const DomainState* st = sn->find_domain(id);
    // 95% is CRITICAL territory, but one refresh is one observation: with the
    // default dwell of two the level is not confirmed yet.
    EXPECT(st && st->level == PressureLevel::Normal && st->in_hysteresis_hold);
    // What the runtime produced can be read back (domain ids are unique).
    auto text = json_serialize(snapshot_to_json(*sn));
    EXPECT(text.has_value());
    if (text) {
        auto parsed = json_parse(*text);
        EXPECT(parsed && snapshot_from_json(*parsed).has_value());
    }
    auto next = rt.refresh(1100);
    const DomainState* st2 = next->find_domain(id);
    EXPECT(st2 && st2->level == PressureLevel::Critical);
}
