#include "test_harness.h"
#include "memory_pressure/runtime.h"
#include "memory_pressure/serialize.h"
#include "memory_pressure/providers/synthetic.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace memory_pressure;

TEST(concurrency_parallel_readers_during_refresh) {
    PressureRuntime rt;
    std::vector<SyntheticDomainSpec> ds;
    for (int i = 0; i < 4; ++i) {
        SyntheticDomainSpec d; d.id = PressureDomainId{1, static_cast<std::uint64_t>(i)};
        d.type = DomainType::AcceleratorMemory; d.group = "gpu";
        d.total_capacity = 4ULL*1024*1024*1024; d.phase = static_cast<double>(i)/4.0; ds.push_back(d);
    }
    rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::GradualGrowth, 100, 4));
    Budget b; b.hard_capacity = 2ULL*1024*1024*1024; rt.set_budget(PressureDomainId{1,0}, b);

    std::atomic<bool> stop{false};
    std::atomic<int> reader_errors{0};

    // Writer thread: refresh repeatedly.
    std::thread writer([&]() {
        for (int i = 0; i < 400; ++i) rt.refresh(1000ULL + i*10);
        stop.store(true);
    });

    // Reader threads: query current snapshot + admission + serialization.
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&]() {
            while (!stop.load() && reader_errors.load() < 50) {
                auto snap = rt.current_snapshot();
                if (snap) {
                    if (snap->aggregate_score < 0.0 || snap->aggregate_score > 1.0) ++reader_errors;
                    auto hint = rt.admit(PressureDomainId{1,0}, 1024);
                    (void)hint;
                    if (json_serialize(snapshot_to_json(*snap)).has_value() == false) ++reader_errors;
                }
            }
        });
    }

    writer.join();
    for (auto& r : readers) r.join();
    EXPECT(reader_errors.load() == 0);
}

TEST(concurrency_policy_replacement_during_refresh) {
    PressureRuntime rt;
    std::vector<SyntheticDomainSpec> ds;
    SyntheticDomainSpec d; d.id = PressureDomainId{2,0}; d.type = DomainType::AcceleratorMemory; d.group="gpu"; d.total_capacity=4ULL*1024*1024*1024; ds.push_back(d);
    rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::GradualGrowth, 100, 4));
    Budget b; b.hard_capacity = 512ULL*1024*1024; rt.set_budget(d.id, b);

    std::atomic<bool> stop{false};
    std::thread writer([&]() {
        for (int i = 0; i < 300; ++i) rt.refresh(1000ULL + i*10);
        stop.store(true);
    });
    std::thread policy([&]() {
        int i = 0;
        while (!stop.load()) {
            PressurePolicy p = default_policy();
            p.version = 1 + (i++ % 3);
            rt.set_policy(p);
        }
    });
    std::thread reader([&]() {
        while (!stop.load()) {
            auto pol = rt.current_policy();
            if (pol) { /* read is safe */ }
        }
    });
    writer.join(); policy.join(); reader.join();
    EXPECT(true);
}

TEST(concurrency_subscription_storm_while_transitioning) {
    PressureRuntime rt;
    std::vector<SyntheticDomainSpec> ds;
    SyntheticDomainSpec d; d.id = PressureDomainId{3,0}; d.type = DomainType::AcceleratorMemory; d.group="gpu"; d.total_capacity=4ULL*1024*1024*1024; ds.push_back(d);
    rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::Sawtooth, 100, 4));
    Budget b; b.hard_capacity = 512ULL*1024*1024; rt.set_budget(d.id, b);

    std::atomic<bool> stop{false};
    std::thread writer([&]() {
        for (int i = 0; i < 300; ++i) rt.refresh(1000ULL + i*10);
        stop.store(true);
    });
    std::vector<std::thread> subs;
    for (int i = 0; i < 4; ++i) {
        subs.emplace_back([&]() {
            auto s = rt.subscribe(SubscriptionFilter{});
            PressureEvent e;
            while (!stop.load()) { if (s->try_pop(e)) { /* consumed */ } }
        });
    }
    writer.join();
    for (auto& t : subs) t.join();
    EXPECT(true);
}
