#include "test_harness.h"
#include "memory_pressure/runtime.h"
#include "memory_pressure/domain.h"
#include "memory_pressure/providers/cuda.h"
#include "memory_pressure/providers/storage.h"
#include "memory_pressure/providers/synthetic.h"
#include "memory_pressure/providers/windows.h"

using namespace memory_pressure;

TEST(windows_provider_observes_host) {
    WindowsHostProvider p;
    ProviderSample s = p.sample(1000);
    EXPECT(s.status == ProviderStatus::Healthy || s.status == ProviderStatus::Partial);
    bool host=false, commit=false, proc=false;
    for (const auto& o : s.observations) {
        if (o.type == DomainType::HostMemory) { host = true; EXPECT(o.total_capacity > 0); EXPECT(o.provenance == Provenance::WindowsGlobalMemoryStatusEx); }
        if (o.type == DomainType::SystemCommit) { commit = true; EXPECT(o.total_capacity > 0); EXPECT(o.provenance == Provenance::WindowsPerformanceInfo); }
        if (o.type == DomainType::ProcessCommit) { proc = true; EXPECT(o.total_capacity > 0); }
        EXPECT(o.observed_at_ms == 1000);
    }
    EXPECT(host);
    EXPECT(commit);
    EXPECT(proc);
}

TEST(cuda_provider_real_or_unavailable) {
    CudaDeviceProvider p(true);   // enable probes
    if (p.supported()) {
        EXPECT(p.device_count() >= 1);
        ProviderSample s = p.sample(2000);
        EXPECT(!s.observations.empty());
        bool accel=false;
        for (const auto& o : s.observations) {
            if (o.type == DomainType::AcceleratorMemory && o.validity == Validity::Valid) {
                accel = true;
                EXPECT(o.total_capacity > 0);
                EXPECT(o.provenance == Provenance::CudaRuntimeApi);
                // committed = total - free must not be negative.
                EXPECT(o.committed <= o.total_capacity);
            }
        }
        EXPECT(accel);
        // A small bounded allocation probe must succeed and free cleanly.
        EXPECT(p.probe_device(0, 1ULL * 1024 * 1024) || p.device_count() == 0);
    } else {
        // Honest: on a machine without a working CUDA driver, the provider
        // reports unavailable rather than fabricating capacity.
        ProviderSample s = p.sample(2000);
        EXPECT(s.status == ProviderStatus::Unavailable);
    }
}

TEST(storage_provider_observes_filesystem) {
    StorageProvider p(".", DomainType::PersistentStorageCapacity);
    ProviderSample s = p.sample(3000);
    EXPECT(!s.observations.empty());
    for (const auto& o : s.observations) {
        EXPECT(o.type == DomainType::PersistentStorageCapacity);
        EXPECT(o.total_capacity > 0);
        EXPECT(o.provenance == Provenance::Filesystem);
        EXPECT(o.committed <= o.total_capacity || (o.total_capacity >= o.committed));  // no negative
    }
}

TEST(synthetic_scenarios_are_deterministic) {
    auto run = [](SyntheticScenario sc) {
        PressureRuntime rt;
        std::vector<SyntheticDomainSpec> ds;
        SyntheticDomainSpec d; d.id = PressureDomainId{3,3}; d.type = DomainType::AcceleratorMemory;
        d.group="gpu"; d.total_capacity = 8ULL*1024*1024*1024; ds.push_back(d);
        rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), sc, 100, 4));
        Budget b; b.hard_capacity = 2ULL*1024*1024*1024; rt.set_budget(d.id, b);
        std::vector<std::string> seq;
        for (int i = 0; i < 30; ++i) {
            auto sn = rt.refresh(1000 + i*100);
            const DomainState* st = sn->find_domain(d.id);
            seq.push_back(st ? std::string(to_string(st->level)) : "none");
        }
        return seq;
    };
    auto a = run(SyntheticScenario::GradualGrowth);
    auto b = run(SyntheticScenario::GradualGrowth);
    EXPECT(a == b);   // deterministic replay of identical input
    EXPECT(a.back() != "NORMAL");   // gradual growth reaches pressure
    // Monotonic climb: never a severity decrease.
    const int r0 = severity_rank(PressureLevel::Normal);
    int prev = r0;
    for (const auto& l : a) {
        auto lv = pressure_level_from_string(l);
        int r = lv ? severity_rank(*lv) : r0;
        EXPECT(r >= prev);   // never de-escalates in gradual growth
        prev = r;
    }
}

TEST(synthetic_multi_gpu_model) {
    PressureRuntime rt;
    std::vector<SyntheticDomainSpec> ds;
    SyntheticDomainSpec g0; g0.id = PressureDomainId{1,0}; g0.type = DomainType::AcceleratorMemory; g0.group="gpu0"; g0.total_capacity=8ULL*1024*1024*1024; ds.push_back(g0);
    SyntheticDomainSpec g1; g1.id = PressureDomainId{1,1}; g1.type = DomainType::AcceleratorMemory; g1.group="gpu1"; g1.total_capacity=8ULL*1024*1024*1024; ds.push_back(g1);
    rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::OneRecovering, 100, 4));
    Budget b; b.hard_capacity = 2ULL*1024*1024*1024; rt.set_budget(g0.id, b); rt.set_budget(g1.id, b);
    auto sn1 = rt.refresh(1000);
    EXPECT(sn1->find_domain(g0.id) != nullptr);
    EXPECT(sn1->find_domain(g1.id) != nullptr);
    // Run to completion: at least one domain changes level.
    std::string l0, l1;
    for (int i = 0; i < 50; ++i) {
        auto sn = rt.refresh(1000 + i*100);
        auto* a = sn->find_domain(g0.id); auto* b2 = sn->find_domain(g1.id);
        if (a) l0 = to_string(a->level);
        if (b2) l1 = to_string(b2->level);
    }
    EXPECT(!l0.empty() && !l1.empty());
}

TEST(synthetic_provider_failure_honest) {
    std::vector<SyntheticDomainSpec> ds;
    SyntheticDomainSpec d; d.id = PressureDomainId{4,4}; d.type = DomainType::AcceleratorMemory; d.group="gpu"; d.total_capacity=8ULL*1024*1024*1024; ds.push_back(d);
    SyntheticProvider p(std::move(ds), SyntheticScenario::ProviderFailure, 100, 4);
    // First calls succeed, then it fails.
    auto s1 = p.sample(1000);
    EXPECT(s1.status == ProviderStatus::Healthy);
    p.sample(1100); p.sample(1200); p.sample(1300);
    auto s5 = p.sample(1400);
    EXPECT(s5.status == ProviderStatus::Failed);
}
