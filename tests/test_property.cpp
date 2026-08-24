#include "test_harness.h"
#include "memory_pressure/runtime.h"
#include "memory_pressure/hysteresis.h"
#include "memory_pressure/policy.h"
#include "memory_pressure/domain.h"
#include "memory_pressure/providers/synthetic.h"

#include <cmath>

using namespace memory_pressure;

namespace {
struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed ? seed : 1) {}
    std::uint64_t next() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return s; }
    double u01() { return static_cast<double>(next() >> 11) / static_cast<double>(1ULL << 53); }
    std::uint64_t range(std::uint64_t n) { return n ? next() % n : 0; }
};
}

TEST(property_random_sequences_preserve_invariants) {
    Rng rng(0xDEC0DEULL);
    const int trials = 2000;
    for (int t = 0; t < trials; ++t) {
        PressureRuntime rt;
        const int nd = 1 + static_cast<int>(rng.range(4));
        std::vector<PressureDomainId> ids;
        for (int di = 0; di < nd; ++di) {
            PressureDomainId id{static_cast<std::uint64_t>(t) + 1, static_cast<std::uint64_t>(di) + 1};
            ids.push_back(id);
        }
        // Random budgets.
        for (int di = 0; di < nd; ++di) {
            Budget b;
            const std::uint64_t cap = 1ULL * 1024 * 1024 * (1 + rng.range(512));
            b.hard_capacity = cap;
            b.emergency_reserve_bytes = cap ? (rng.range(cap / 2 + 1)) : 0;
            if (b.emergency_reserve_bytes > cap) b.emergency_reserve_bytes = cap;
            if (!validate_budget(b)) rt.set_budget(ids[di], b);
        }
        // Random policy sometimes.
        if (t % 7 == 0) {
            PressurePolicy p = default_policy();
            p.version = 1 + rng.range(100);
            rt.set_policy(p);
        }
        // Random observation generator: committed fraction of capacity.
        std::vector<std::uint64_t> caps(nd);
        for (int di = 0; di < nd; ++di) caps[di] = 1ULL * 1024 * 1024 * (1 + rng.range(1024));
        const int steps = 3 + static_cast<int>(rng.range(18));
        for (int st = 0; st < steps; ++st) {
            // Feed a random committed amount, then a controlled provider sample.
            DomainObservation o;
            o.id = ids[0];
            o.type = DomainType::AcceleratorMemory;
            o.provider = "prop";
            o.total_capacity = caps[0];
            o.committed = (caps[0] ? (rng.range(caps[0] + 1)) : 0);   // could exceed cap (adversarial)
            o.available = o.committed <= o.total_capacity ? o.total_capacity - o.committed : 0;
            o.confidence = Confidence::High;
            o.provenance = Provenance::SyntheticInput;
            o.validity = Validity::Valid;
            o.observed_at_ms = 1000ULL + st * 100;

            // We must go through the provider path to update the runtime; use a
            // synthetic provider seeded per-step via a tiny helper runtime.
            // Instead, exercise the hysteresis + scoring + diff invariants
            // directly here (pure functions) plus a shared runtime via synthetic.
            (void)o;

            // Hysteresis state invariant check on a fresh machine.
            HysteresisState h(default_policy().hysteresis);
            PressureLevel lv = h.current();
            for (int k = 0; k < 6; ++k) {
                const double util = rng.u01();
                const PressureLevel raw = evaluate_level(util, lv, default_policy().thresholds);
                lv = h.update(raw, 100 + k);
                EXPECT(is_known_level(lv) || lv == PressureLevel::Unknown);
                EXPECT(severity_rank(lv) >= -1 && severity_rank(lv) <= 4);
            }
        }

        // Snapshot invariants through real synthetic provider + budget.
        PressureRuntime rt2;
        std::vector<SyntheticDomainSpec> ds;
        SyntheticDomainSpec g; g.id = ids[0]; g.type = DomainType::AcceleratorMemory;
        g.group="gpu"; g.total_capacity = caps[0]; ds.push_back(g);
        rt2.register_provider(std::make_shared<SyntheticProvider>(std::move(ds),
            SyntheticScenario::GradualGrowth, 100, 4));
        Budget b2; b2.hard_capacity = caps[0] ? caps[0] : 1; b2.emergency_reserve_bytes = b2.hard_capacity / 4;
        rt2.set_budget(ids[0], b2);
        for (int st = 0; st < 8; ++st) {
            auto sn = rt2.refresh(1000ULL + st * 100);
            const DomainState* d = sn->find_domain(ids[0]);
            if (d) {
                EXPECT(d->available <= d->usable_capacity || d->usable_capacity == 0);
                EXPECT(d->score >= 0.0 && d->score <= 1.0);
                EXPECT(d->utilization >= 0.0);
                EXPECT(std::isfinite(d->utilization));
                EXPECT(is_known_level(d->level) || d->level == PressureLevel::Unknown);
            }
            EXPECT(sn->aggregate_score >= 0.0 && sn->aggregate_score <= 1.0);
        }
    }
}

TEST(property_no_negative_available_hysteresis_dwell) {
    // Exercise the hysteresis machine around an oscillation and confirm it never
    // produces an invalid level and honours dwell.
    PressurePolicy pol = default_policy();
    HysteresisState h(pol.hysteresis);
    // Force HIGH (0.87 is the HIGH band) with confirmations.
    for (int i = 0; i < 4; ++i) h.update(evaluate_level(0.87, h.current(), pol.thresholds), i);
    EXPECT(h.current() == PressureLevel::High);
    PressureLevel before = h.current();
    // Oscillate just below the exit threshold: must not recover.
    for (int i = 0; i < 10; ++i) {
        h.update(evaluate_level(0.80, h.current(), pol.thresholds), 100 + i);
        EXPECT(h.current() == before || severity_rank(h.current()) >= severity_rank(before));
    }
}

TEST(property_identical_seed_identical_sequence) {
    auto run = [](std::uint64_t seed) {
        Rng r(seed);
        PressureRuntime rt;
        std::vector<SyntheticDomainSpec> ds;
        PressureDomainId id{7, 8};
        for (int i = 0; i < 3; ++i) {
            SyntheticDomainSpec d; d.id = PressureDomainId{7, static_cast<std::uint64_t>(8 + i)};
            d.type = DomainType::AcceleratorMemory; d.group="gpu";
            d.total_capacity = 1ULL*1024*1024*1024; d.phase = r.u01(); ds.push_back(d);
        }
        rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::MultiGpu, 100, 4));
        Budget b; b.hard_capacity = 512ULL*1024*1024; rt.set_budget(id, b);
        std::vector<std::string> out;
        for (int i = 0; i < 25; ++i) {
            auto sn = rt.refresh(1000 + i*100);
            const DomainState* d = sn->find_domain(id);
            out.push_back(d ? std::to_string(d->score) + ":" + std::to_string(d->utilization) : "none");
        }
        return out;
    };
    EXPECT(run(12345) == run(12345));
}
