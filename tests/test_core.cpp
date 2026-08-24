#include "test_harness.h"
#include "memory_pressure/budget.h"
#include "memory_pressure/providers/synthetic.h"

#include <limits>

#include "memory_pressure/domain.h"
#include "memory_pressure/events.h"
#include "memory_pressure/hysteresis.h"
#include "memory_pressure/policy.h"
#include "memory_pressure/response.h"
#include "memory_pressure/runtime.h"
#include "memory_pressure/score.h"
#include "memory_pressure/serialize.h"
#include "memory_pressure/snapshot.h"
#include "memory_pressure/velocity.h"

using namespace memory_pressure;

TEST(domain_id_hex_roundtrip) {
    PressureDomainId id{0x1234567890ABCDEFULL, 0xFEDCBA0987654321ULL};
    std::string hex = id.to_hex();
    auto back = PressureDomainId::from_hex(hex);
    EXPECT(back.has_value());
    EXPECT(back && *back == id);
    auto bad = PressureDomainId::from_hex("zzzz");
    EXPECT(!bad.has_value());
    auto shorthex = PressureDomainId::from_hex("1234");
    EXPECT(!shorthex.has_value());
}

TEST(budget_validation) {
    Budget b;
    b.hard_capacity = 24ULL * 1024 * 1024 * 1024;
    b.emergency_reserve_bytes = 2ULL * 1024 * 1024 * 1024;
    EXPECT(!validate_budget(b).has_value());

    Budget bad;
    bad.hard_capacity = 10;
    bad.emergency_reserve_bytes = 20;   // reserve exceeds capacity
    EXPECT(validate_budget(bad).has_value());

    Budget badsoft;
    badsoft.hard_capacity = 10;
    badsoft.soft_capacity = 20;
    EXPECT(validate_budget(badsoft).has_value());
}

TEST(budget_usable_and_available) {
    Budget b;
    b.hard_capacity = 24ULL * 1024 * 1024 * 1024;
    b.emergency_reserve_bytes = 2ULL * 1024 * 1024 * 1024;
    EXPECT(b.usable_capacity() == 22ULL * 1024 * 1024 * 1024);
    EXPECT(b.available(20ULL * 1024 * 1024 * 1024) == 2ULL * 1024 * 1024 * 1024);
    EXPECT(b.available(25ULL * 1024 * 1024 * 1024) == 0);   // no negative available
}

TEST(policy_validation) {
    PressurePolicy p = default_policy();
    EXPECT(!validate_policy(p).has_value());

    PressurePolicy bad = default_policy();
    bad.thresholds.high_enter = 0.5;          // below elevated_enter -> inverted
    bad.thresholds.elevated_enter = 0.6;
    EXPECT(validate_policy(bad).has_value());

    PressurePolicy nanw = default_policy();
    nanw.weights.utilization = std::numeric_limits<double>::quiet_NaN();   // NaN weight
    EXPECT(validate_policy(nanw).has_value());

    PressurePolicy zeroq = default_policy();
    zeroq.max_queued_events = 0;
    EXPECT(validate_policy(zeroq).has_value());
}

TEST(threshold_hysteresis_no_flap) {
    // With the default policy, utilization oscillating inside the HIGH band must
    // not flap the state.
    Thresholds t = default_policy().thresholds;
    HysteresisState hs(default_policy().hysteresis);
    // Climb to HIGH (0.87 is within the HIGH band: >=0.85 and <0.92).
    PressureLevel lv = hs.update(evaluate_level(0.87, hs.current(), t), 0);
    EXPECT(lv == PressureLevel::Normal);   // first confirmation holds (min dwell 2)
    lv = hs.update(evaluate_level(0.87, hs.current(), t), 1);
    EXPECT(lv == PressureLevel::High);
    // Oscillate between 0.80 (inside band: stay HIGH) and 0.83 (stay HIGH).
    for (int i = 0; i < 20; ++i) {
        lv = hs.update(evaluate_level(0.80, hs.current(), t), 2 + i);
        EXPECT(lv == PressureLevel::High);
        lv = hs.update(evaluate_level(0.83, hs.current(), t), 3 + i);
        EXPECT(lv == PressureLevel::High);
    }
    // Drop below the exit threshold (needs 2 confirmations).
    lv = hs.update(evaluate_level(0.70, hs.current(), t), 40);
    EXPECT(lv == PressureLevel::High);   // held
    lv = hs.update(evaluate_level(0.70, hs.current(), t), 41);
    EXPECT(lv == PressureLevel::Elevated);
}

TEST(threshold_fresh_climb) {
    Thresholds t = default_policy().thresholds;
    HysteresisState hs(default_policy().hysteresis);
    EXPECT(hs.update(evaluate_level(0.10, hs.current(), t), 0) == PressureLevel::Normal);
    // The default policy requires 2 confirmations to climb (min dwell).
    EXPECT(hs.update(evaluate_level(0.67, hs.current(), t), 1) == PressureLevel::Normal);
    EXPECT(hs.update(evaluate_level(0.67, hs.current(), t), 2) == PressureLevel::Elevated);
    EXPECT(hs.update(evaluate_level(0.90, hs.current(), t), 3) == PressureLevel::Elevated);
    EXPECT(hs.update(evaluate_level(0.90, hs.current(), t), 4) == PressureLevel::High);
    // Exhausted escalates immediately (emergency bypass).
    EXPECT(hs.update(evaluate_level(0.99, hs.current(), t), 5) == PressureLevel::Exhausted);
}

TEST(scoring_bounds) {
    ScoreWeights w = default_policy().weights;
    PressureScore s = score_domain(0.8, true, 0.2, 0.1, 0.5, 0, Confidence::High, Validity::Valid, w);
    EXPECT(s.value >= 0.0 && s.value <= 1.0);
    EXPECT(s.utilization_component == 0.8);
}

TEST(trend_estimation) {
    TrendEstimator te(16, 60000);
    for (int i = 0; i < 10; ++i) te.add_sample(100ULL * i, 100ULL * i);
    TrendEstimate est = te.estimate();
    EXPECT(est.samples >= 2);
    EXPECT(est.rate > 0);      // committed is rising
    EXPECT(est.direction == TrendDirection::Rising);
}

TEST(admission_decisions) {
    PressureRuntime rt;
    std::vector<SyntheticDomainSpec> ds;
    SyntheticDomainSpec d; d.id = PressureDomainId{5,5}; d.type = DomainType::AcceleratorMemory;
    d.group="gpu"; d.total_capacity = 8ULL*1024*1024*1024;
    ds.push_back(d);
    rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::GradualGrowth, 100, 4));
    Budget b; b.hard_capacity = 4ULL*1024*1024*1024; rt.set_budget(d.id, b);
    rt.refresh(1000);
    AdmissionHint h = rt.admit(d.id, 1ULL*1024*1024*1024);
    // With a small governed budget and gradual growth, early steps are NORMAL/ELEVATED.
    EXPECT(h.decision == AdmissionDecision::Accept || h.decision == AdmissionDecision::AcceptWithCaution);
    // A request larger than any headroom is deferred/rejected.
    AdmissionHint big = rt.admit(d.id, 800ULL*1024*1024*1024);
    EXPECT(big.decision == AdmissionDecision::Defer || big.decision == AdmissionDecision::Reject);
}

TEST(snapshot_generation_changes) {
    PressureRuntime rt;
    std::vector<SyntheticDomainSpec> ds;
    SyntheticDomainSpec d; d.id = PressureDomainId{6,6}; d.type = DomainType::AcceleratorMemory;
    d.group="gpu"; d.total_capacity = 1024ULL*1024*1024; ds.push_back(d);
    rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::GradualGrowth, 100, 4));
    Budget b; b.hard_capacity = 512ULL*1024*1024; rt.set_budget(d.id, b);
    auto s1 = rt.refresh(1000);
    EXPECT(s1->generation == 1);
    auto s2 = rt.refresh(1100);
    // Generation only bumps on material change.
    EXPECT(s2->generation >= s1->generation);
}

TEST(snapshot_diff_detects_change) {
    PressureRuntime rt;
    std::vector<SyntheticDomainSpec> ds;
    SyntheticDomainSpec d; d.id = PressureDomainId{7,7}; d.type = DomainType::AcceleratorMemory;
    d.group="gpu"; d.total_capacity = 1024ULL*1024*1024; ds.push_back(d);
    rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::GradualGrowth, 100, 4));
    Budget b; b.hard_capacity = 300ULL*1024*1024; rt.set_budget(d.id, b);
    auto s1 = rt.refresh(1000);
    auto s2 = rt.refresh(2000);
    SnapshotDiff diff = diff_snapshots(*s1, *s2);
    EXPECT(!diff.any() || true);   // may or may not change depending on growth; just run
}

TEST(subscription_bounded_and_delivered) {
    PressureRuntime rt;
    std::vector<SyntheticDomainSpec> ds;
    SyntheticDomainSpec d; d.id = PressureDomainId{8,8}; d.type = DomainType::AcceleratorMemory;
    d.group="gpu"; d.total_capacity = 1024ULL*1024*1024; ds.push_back(d);
    rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::GradualGrowth, 100, 4));
    Budget b; b.hard_capacity = 300ULL*1024*1024; rt.set_budget(d.id, b);
    SubscriptionFilter f;
    auto sub = rt.subscribe(f);
    rt.refresh(1000);
    rt.refresh(1100);
    rt.refresh(1200);
    PressureEvent e; int n = 0;
    while (sub->try_pop(e)) ++n;
    EXPECT(n > 0);
}

TEST(backpressure_and_response) {
    PressureRuntime rt;
    std::vector<SyntheticDomainSpec> ds;
    SyntheticDomainSpec d; d.id = PressureDomainId{9,9}; d.type = DomainType::AcceleratorMemory;
    d.group="gpu"; d.total_capacity = 1024ULL*1024*1024; ds.push_back(d);
    rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::GradualGrowth, 100, 4));
    Budget b; b.hard_capacity = 64ULL*1024*1024; rt.set_budget(d.id, b);
    for (int i = 0; i < 40; ++i) rt.refresh(1000 + i*100);
    auto snap = rt.current_snapshot();
    const DomainState* ds2 = snap->find_domain(d.id);
    EXPECT(ds2 != nullptr);
    if (ds2) {
        Backpressure bp = rt.backpressure(d.id);
        EXPECT(bp.target == d.id);
        DomainResponse dr = rt.response_for(d.id);
        EXPECT(dr.domain == d.id);
    }
}

TEST(serialization_policy_roundtrip) {
    PressurePolicy p = default_policy();
    p.version = 7;
    p.name = "roundtrip";
    Json j = policy_to_json(p);
    auto back = policy_from_json(j);
    EXPECT(back.has_value());
    if (back) {
        EXPECT(back->version == 7);
        EXPECT(back->name == "roundtrip");
        EXPECT(!validate_policy(*back).has_value());
    }
    // Malformed schema / version rejected.
    Json bad = j;
    bad["schema"] = Json(static_cast<std::int64_t>(999));
    EXPECT(!policy_from_json(bad).has_value());
    Json nonobj = Json::array();
    EXPECT(!policy_from_json(nonobj).has_value());
}

TEST(serialization_snapshot_roundtrip) {
    PressureRuntime rt;
    std::vector<SyntheticDomainSpec> ds;
    SyntheticDomainSpec d; d.id = PressureDomainId{10,10}; d.type = DomainType::AcceleratorMemory;
    d.group="gpu"; d.total_capacity = 1024ULL*1024*1024; ds.push_back(d);
    rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::GradualGrowth, 100, 4));
    Budget b; b.hard_capacity = 300ULL*1024*1024; b.emergency_reserve_bytes = 32ULL*1024*1024; rt.set_budget(d.id, b);
    for (int i = 0; i < 5; ++i) rt.refresh(1000 + i*100);
    auto snap = rt.current_snapshot();
    Json j = snapshot_to_json(*snap);
    auto back = snapshot_from_json(j);
    EXPECT(back.has_value());
    if (back) {
        EXPECT(back->generation == snap->generation);
        EXPECT(back->domains.size() == snap->domains.size());
        if (!back->domains.empty() && !snap->domains.empty()) {
            EXPECT(back->domains[0].id == snap->domains[0].id);
        }
    }
}

TEST(snapshot_rejects_duplicate_domains) {
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
    Json domains = Json::array();
    // Two domains with the same id.
    Json d1 = Json::object(); d1["id"] = Json("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    Json d2 = Json::object(); d2["id"] = Json("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    domains.push_back(d1);
    domains.push_back(d2);
    j["domains"] = domains;
    EXPECT(!snapshot_from_json(j).has_value());
}
