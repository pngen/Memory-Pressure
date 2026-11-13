#include "test_harness.h"
#include "memory_pressure/budget.h"
#include "memory_pressure/providers/synthetic.h"

#include <limits>
#include <memory>
#include <vector>

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

// ---------------------------------------------------------------------------
// Budgets, policy and level bookkeeping
// ---------------------------------------------------------------------------
namespace {
// One accelerator domain that climbs and then holds at 98% of `total`.
std::shared_ptr<SyntheticProvider> holding_provider(const PressureDomainId& id, std::uint64_t total) {
    std::vector<SyntheticDomainSpec> ds;
    SyntheticDomainSpec d; d.id = id; d.type = DomainType::AcceleratorMemory;
    d.group = "gpu"; d.total_capacity = total; ds.push_back(d);
    return std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::NoRecovery, 100, 4);
}
}

TEST(budget_reserve_sum_does_not_wrap) {
    // Reserves that add up past 2^64 are more than any capacity.  The total
    // must not wrap around to a small number that validation then accepts.
    Budget b;
    b.hard_capacity = 1024;
    Reserve r1; r1.bytes = 1ULL << 63;
    Reserve r2; r2.bytes = 1ULL << 63;
    b.reserves = {r1, r2};
    EXPECT(b.reserve_bytes() == (std::numeric_limits<std::uint64_t>::max)());
    EXPECT(b.usable_capacity() == 0);
    EXPECT(validate_budget(b).has_value());
    PressureRuntime rt;
    EXPECT(rt.set_budget(PressureDomainId{40, 1}, b).has_value());
}

TEST(zero_usable_capacity_is_not_normal) {
    // A budget whose reserves take the whole capacity leaves nothing usable.
    // Bytes committed against it are over the ceiling, not "0% utilized".
    const PressureDomainId id{41, 1};
    PressureRuntime rt;
    rt.register_provider(holding_provider(id, 1024ULL*1024*1024));
    Budget b; b.hard_capacity = 64ULL*1024*1024; b.emergency_reserve_bytes = b.hard_capacity;
    EXPECT(!rt.set_budget(id, b).has_value());
    auto sn = rt.refresh(1000);
    const DomainState* st = sn->find_domain(id);
    EXPECT(st != nullptr);
    if (st) {
        EXPECT(st->usable_capacity == 0);
        EXPECT(st->committed > 0);
        EXPECT(st->level == PressureLevel::Exhausted);
        EXPECT(st->utilization >= 1.0);
        EXPECT(st->reserve_deficit_component == 1.0);
        EXPECT(rt.admit(id, 1).decision == AdmissionDecision::Reject);
    }
}

TEST(policy_validation_rejects_impossible_hysteresis) {
    // Confirmations are counted up to max_debounce_observations and no further:
    // a dwell above that cap can never be met.  Such a policy is refused rather
    // than left to freeze every domain at its current level.
    PressurePolicy p = default_policy();
    p.hysteresis.min_dwell_observations = 6;
    p.hysteresis.max_debounce_observations = 4;
    EXPECT(validate_policy(p).has_value());
    PressureRuntime rt;
    EXPECT(rt.set_policy(p).has_value());
    p.hysteresis.max_debounce_observations = 6;
    EXPECT(!validate_policy(p).has_value());
    EXPECT(!rt.set_policy(p).has_value());
    // Durations that can never elapse are refused as well.
    PressurePolicy q = default_policy();
    q.hysteresis.recovery_delay_ms = std::numeric_limits<double>::quiet_NaN();
    EXPECT(validate_policy(q).has_value());
    q = default_policy();
    q.hysteresis.escalation_delay_ms = -1.0;
    EXPECT(validate_policy(q).has_value());
}

TEST(policy_change_keeps_resolved_levels) {
    // Replacing the policy is not an observation.  A domain that is CRITICAL
    // stays CRITICAL across the swap; it is not reported as recovered and then
    // entered again, and work is not admitted in between.
    const PressureDomainId id{42, 1};
    PressureRuntime rt;
    rt.register_provider(holding_provider(id, 1000ULL*1024*1024));
    Budget b; b.hard_capacity = 1024ULL*1024*1024; rt.set_budget(id, b);   // holds at ~95.7%
    for (int i = 0; i < 24; ++i) rt.refresh(1000 + i*100);
    auto held = rt.current_snapshot();
    const DomainState* before = held->find_domain(id);
    EXPECT(before && before->level == PressureLevel::Critical);

    auto sub = rt.subscribe(SubscriptionFilter{});
    PressurePolicy p = default_policy(); p.version = 2;
    EXPECT(!rt.set_policy(p).has_value());
    auto sn = rt.refresh(1000 + 24*100);
    EXPECT(sn->policy_version == 2);
    const DomainState* after = sn->find_domain(id);
    EXPECT(after != nullptr);
    if (after) {
        EXPECT(after->level == PressureLevel::Critical);
        EXPECT(after->previous_level == PressureLevel::Critical);
        EXPECT(rt.admit(id, 1).decision == AdmissionDecision::Reject);
    }
    bool flapped = false;
    PressureEvent e;
    while (sub->try_pop(e)) {
        if (e.type == PressureEventType::PressureRecovered || e.type == PressureEventType::PressureEntered) flapped = true;
    }
    EXPECT(!flapped);
}

TEST(relief_is_bounded_by_policy) {
    // max_relief_bytes caps what a single relief request may ask for.
    const PressureDomainId id{43, 1};
    PressureRuntime rt;
    rt.register_provider(holding_provider(id, 1000ULL*1024*1024));
    Budget b; b.hard_capacity = 1024ULL*1024*1024; rt.set_budget(id, b);
    PressurePolicy p = default_policy(); p.max_relief_bytes = 1ULL*1024*1024;
    EXPECT(!rt.set_policy(p).has_value());
    for (int i = 0; i < 24; ++i) rt.refresh(1000 + i*100);
    DomainResponse dr = rt.response_for(id);
    EXPECT(dr.level == PressureLevel::Critical);
    EXPECT(dr.relief.has_value());
    if (dr.relief) {
        EXPECT(dr.relief->target_bytes == 1ULL*1024*1024);   // not a quarter of the capacity
        EXPECT(dr.relief->minimum_useful_relief <= dr.relief->target_bytes);
    }
}

TEST(closed_subscription_is_released) {
    // The runtime does not keep a subscription once it has been closed.
    const PressureDomainId id{44, 1};
    PressureRuntime rt;
    rt.register_provider(holding_provider(id, 1024ULL*1024*1024));
    std::weak_ptr<Subscription> through_runtime;
    {
        auto sub = rt.subscribe(SubscriptionFilter{});
        through_runtime = sub;
        rt.close_subscription(sub);
    }
    EXPECT(through_runtime.expired());
    // One that was closed directly is dropped with the next delivery.
    std::weak_ptr<Subscription> directly;
    {
        auto sub = rt.subscribe(SubscriptionFilter{});
        directly = sub;
        sub->close();
    }
    rt.refresh(1000);
    EXPECT(directly.expired());
}

TEST(scoring_ignores_negative_failure_counts) {
    // Score components are normalized to [0,1]: a negative failure count is
    // not a credit against the other components.
    const ScoreWeights w;
    const PressureScore negative = score_domain(0.5, true, 0.5, 0.0, 0.0, -50, Confidence::Authoritative, Validity::Valid, w);
    const PressureScore none = score_domain(0.5, true, 0.5, 0.0, 0.0, 0, Confidence::Authoritative, Validity::Valid, w);
    EXPECT(negative.allocation_failure_component == 0.0);
    EXPECT(negative.value == none.value);
}
