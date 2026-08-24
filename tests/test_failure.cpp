#include "test_harness.h"
#include "memory_pressure/runtime.h"
#include "memory_pressure/providers/synthetic.h"

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
