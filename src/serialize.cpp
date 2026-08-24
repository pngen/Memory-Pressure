#include "memory_pressure/serialize.h"

#include <cmath>
#include <limits>

namespace memory_pressure {

namespace {

Json str(const char* s) { return Json(s ? s : ""); }

std::optional<double> num_or_nullopt(const Json& j, const char* key) {
    const Json* v = j.find(key);
    if (!v) return std::nullopt;
    return v->as_number();
}

std::optional<std::string> string_or_nullopt(const Json& j, const char* key) {
    const Json* v = j.find(key);
    if (!v) return std::nullopt;
    return v->as_string();
}

bool write_u64(const Json& j, const char* key, std::uint64_t& out) {
    const Json* v = j.find(key);
    if (!v) return false;
    auto uv = v->as_uint64();
    if (!uv) return false;
    out = *uv;
    return true;
}

bool write_double(const Json& j, const char* key, double& out) {
    const Json* v = j.find(key);
    if (!v) return false;
    auto dv = v->as_number();
    if (!dv || !std::isfinite(*dv)) return false;
    out = *dv;
    return true;
}

bool write_string(const Json& j, const char* key, std::string& out) {
    const Json* v = j.find(key);
    if (!v) return false;
    auto sv = v->as_string();
    if (!sv) return false;
    out = *sv;
    return true;
}

} // anonymous namespace

// ------------------------------------------------------------- policy -------
Json policy_to_json(const PressurePolicy& p) {
    Json r = Json::object();
    r["schema"] = Json(kPolicySchemaVersion);
    r["version"] = Json(static_cast<std::int64_t>(p.version));
    r["name"] = Json(p.name);

    Json th = Json::object();
    th["normal_exit"] = Json(p.thresholds.normal_exit);
    th["elevated_enter"] = Json(p.thresholds.elevated_enter);
    th["elevated_exit"] = Json(p.thresholds.elevated_exit);
    th["high_enter"] = Json(p.thresholds.high_enter);
    th["high_exit"] = Json(p.thresholds.high_exit);
    th["critical_enter"] = Json(p.thresholds.critical_enter);
    th["critical_exit"] = Json(p.thresholds.critical_exit);
    th["exhausted_enter"] = Json(p.thresholds.exhausted_enter);
    th["exhausted_exit"] = Json(p.thresholds.exhausted_exit);
    r["thresholds"] = th;

    Json hs = Json::object();
    hs["min_dwell_observations"] = Json(static_cast<std::int64_t>(p.hysteresis.min_dwell_observations));
    hs["min_dwell_duration_ms"] = Json(p.hysteresis.min_dwell_duration_ms);
    hs["escalation_delay_ms"] = Json(p.hysteresis.escalation_delay_ms);
    hs["recovery_delay_ms"] = Json(p.hysteresis.recovery_delay_ms);
    hs["immediate_emergency_escalation"] = Json(p.hysteresis.immediate_emergency_escalation);
    hs["max_debounce_observations"] = Json(static_cast<std::int64_t>(p.hysteresis.max_debounce_observations));
    r["hysteresis"] = hs;

    Json w = Json::object();
    w["utilization"] = Json(p.weights.utilization);
    w["free_deficit"] = Json(p.weights.free_deficit);
    w["reserve_deficit"] = Json(p.weights.reserve_deficit);
    w["growth_rate"] = Json(p.weights.growth_rate);
    w["allocation_failure"] = Json(p.weights.allocation_failure);
    w["fragmentation"] = Json(p.weights.fragmentation);
    w["reclaimable_deficit"] = Json(p.weights.reclaimable_deficit);
    w["confidence_penalty"] = Json(p.weights.confidence_penalty);
    w["stale_penalty"] = Json(p.weights.stale_penalty);
    r["weights"] = w;

    Json agg = Json::object();
    agg["consider_hierarchy"] = Json(p.aggregate.consider_hierarchy);
    Json rw = Json::object();
    for (const auto& [role, val] : p.aggregate.role_weight) rw[to_string(role)] = Json(val);
    agg["role_weight"] = rw;
    agg["bottleneck_penalty"] = Json(p.aggregate.bottleneck_penalty);
    agg["reserve_exhaustion_penalty"] = Json(p.aggregate.reserve_exhaustion_penalty);
    agg["response_feasibility_penalty"] = Json(p.aggregate.response_feasibility_penalty);
    r["aggregate"] = agg;

    Json fresh = Json::object();
    fresh["default_max_age_us"] = Json(static_cast<std::int64_t>(p.freshness.default_max_age_us));
    Json per = Json::object();
    for (const auto& [k, v] : p.freshness.per_provider_max_age_us) per[k] = Json(static_cast<std::int64_t>(v));
    fresh["per_provider_max_age_us"] = per;
    r["freshness"] = fresh;

    Json rules = Json::array();
    for (const auto& rule : p.response_rules) {
        Json rr = Json::object();
        rr["level"] = str(to_string(rule.level));
        Json acts = Json::array();
        for (const auto a : rule.actions) acts.push_back(str(to_string(a)));
        rr["actions"] = acts;
        rules.push_back(rr);
    }
    r["response_rules"] = rules;
    r["max_queued_events"] = Json(static_cast<std::int64_t>(p.max_queued_events));
    r["max_relief_bytes"] = Json(static_cast<std::int64_t>(p.max_relief_bytes));
    return r;
}

std::optional<PressurePolicy> policy_from_json(const Json& j) {
    if (!j.is_object()) return std::nullopt;
    auto schema = j.find("schema");
    if (!schema || !schema->is_number()) return std::nullopt;
    if (schema->as_int64().value_or(-1) != kPolicySchemaVersion) return std::nullopt;

    PressurePolicy p;
    if (!write_u64(j, "version", p.version)) return std::nullopt;
    if (!write_string(j, "name", p.name)) p.name = "default";

    const Json* th = j.find("thresholds");
    if (!th) return std::nullopt;
    if (!write_double(*th, "normal_exit", p.thresholds.normal_exit)) return std::nullopt;
    if (!write_double(*th, "elevated_enter", p.thresholds.elevated_enter)) return std::nullopt;
    if (!write_double(*th, "elevated_exit", p.thresholds.elevated_exit)) return std::nullopt;
    if (!write_double(*th, "high_enter", p.thresholds.high_enter)) return std::nullopt;
    if (!write_double(*th, "high_exit", p.thresholds.high_exit)) return std::nullopt;
    if (!write_double(*th, "critical_enter", p.thresholds.critical_enter)) return std::nullopt;
    if (!write_double(*th, "critical_exit", p.thresholds.critical_exit)) return std::nullopt;
    if (!write_double(*th, "exhausted_enter", p.thresholds.exhausted_enter)) return std::nullopt;
    if (!write_double(*th, "exhausted_exit", p.thresholds.exhausted_exit)) return std::nullopt;

    if (const Json* hs = j.find("hysteresis")) {
        if (const Json* v = hs->find("min_dwell_observations")) p.hysteresis.min_dwell_observations = static_cast<std::uint32_t>(v->as_int64().value_or(2));
        if (const Json* v = hs->find("min_dwell_duration_ms")) p.hysteresis.min_dwell_duration_ms = v->as_number().value_or(0.0);
        if (const Json* v = hs->find("escalation_delay_ms")) p.hysteresis.escalation_delay_ms = v->as_number().value_or(0.0);
        if (const Json* v = hs->find("recovery_delay_ms")) p.hysteresis.recovery_delay_ms = v->as_number().value_or(0.0);
        if (const Json* v = hs->find("immediate_emergency_escalation")) p.hysteresis.immediate_emergency_escalation = v->as_bool().value_or(true);
        if (const Json* v = hs->find("max_debounce_observations")) p.hysteresis.max_debounce_observations = static_cast<std::uint32_t>(v->as_int64().value_or(4));
    }

    if (const Json* w = j.find("weights")) {
        auto rd = [&](const char* k, double& out) { if (const Json* v = w->find(k)) out = v->as_number().value_or(-1.0); };
        rd("utilization", p.weights.utilization);
        rd("free_deficit", p.weights.free_deficit);
        rd("reserve_deficit", p.weights.reserve_deficit);
        rd("growth_rate", p.weights.growth_rate);
        rd("allocation_failure", p.weights.allocation_failure);
        rd("fragmentation", p.weights.fragmentation);
        rd("reclaimable_deficit", p.weights.reclaimable_deficit);
        rd("confidence_penalty", p.weights.confidence_penalty);
        rd("stale_penalty", p.weights.stale_penalty);
    }

    if (const Json* agg = j.find("aggregate")) {
        if (const Json* v = agg->find("consider_hierarchy")) p.aggregate.consider_hierarchy = v->as_bool().value_or(true);
        if (const Json* v = agg->find("role_weight"); v && v->is_object()) {
            for (const auto& [k, val] : v->object_items()) {
                auto role = tier_role_from_string(k);
                if (role && val.is_number()) p.aggregate.role_weight[*role] = val.as_number().value_or(1.0);
            }
        }
    }

    if (const Json* rules = j.find("response_rules"); rules && rules->is_array()) {
        for (const auto& rr : rules->array_items()) {
            ResponseRule rule;
            auto lv = rr.find("level");
            if (lv) { auto parsed = pressure_level_from_string(lv->as_string().value_or("")); if (parsed) rule.level = *parsed; }
            if (const Json* acts = rr.find("actions")) {
                for (const auto& a : acts->array_items()) {
                    auto parsed = response_action_from_string(a.as_string().value_or(""));
                    if (parsed) rule.actions.push_back(*parsed);
                }
            }
            p.response_rules.push_back(rule);
        }
    }

    if (const Json* v = j.find("max_queued_events")) p.max_queued_events = v->as_uint64().value_or(4096);
    if (const Json* v = j.find("max_relief_bytes")) p.max_relief_bytes = v->as_uint64().value_or((std::numeric_limits<std::uint64_t>::max)());

    if (validate_policy(p)) return std::nullopt;
    return p;
}

// ------------------------------------------------------------- budget -------
Json budget_to_json(const Budget& b) {
    Json r = Json::object();
    r["version"] = Json(static_cast<std::int64_t>(b.version));
    r["hard_capacity"] = Json(static_cast<std::int64_t>(b.hard_capacity));
    if (b.soft_capacity) r["soft_capacity"] = Json(static_cast<std::int64_t>(*b.soft_capacity));
    r["emergency_reserve_bytes"] = Json(static_cast<std::int64_t>(b.emergency_reserve_bytes));
    r["minimum_free_reserve"] = Json(static_cast<std::int64_t>(b.minimum_free_reserve));
    r["reclaim_target"] = Json(static_cast<std::int64_t>(b.reclaim_target));
    r["demotion_target"] = Json(static_cast<std::int64_t>(b.demotion_target));
    r["admission_headroom"] = Json(static_cast<std::int64_t>(b.admission_headroom));
    r["owner"] = Json(b.owner);
    Json res = Json::array();
    for (const auto& rv : b.reserves) {
        Json rr = Json::object();
        rr["kind"] = str(to_string(rv.kind));
        rr["name"] = Json(rv.name);
        rr["bytes"] = Json(static_cast<std::int64_t>(rv.bytes));
        res.push_back(rr);
    }
    r["reserves"] = res;
    return r;
}

std::optional<Budget> budget_from_json(const Json& j) {
    if (!j.is_object()) return std::nullopt;
    Budget b;
    if (!write_u64(j, "hard_capacity", b.hard_capacity)) return std::nullopt;
    if (const Json* v = j.find("soft_capacity")) b.soft_capacity = v->as_uint64();
    if (const Json* v = j.find("emergency_reserve_bytes")) b.emergency_reserve_bytes = v->as_uint64().value_or(0);
    if (const Json* v = j.find("minimum_free_reserve")) b.minimum_free_reserve = v->as_uint64().value_or(0);
    if (const Json* v = j.find("reclaim_target")) b.reclaim_target = v->as_uint64().value_or(0);
    if (const Json* v = j.find("demotion_target")) b.demotion_target = v->as_uint64().value_or(0);
    if (const Json* v = j.find("admission_headroom")) b.admission_headroom = v->as_uint64().value_or(0);
    if (!write_string(j, "owner", b.owner)) b.owner.clear();
    if (const Json* v = j.find("version")) b.version = v->as_uint64().value_or(0);
    if (const Json* res = j.find("reserves"); res && res->is_array()) {
        for (const auto& rv : res->array_items()) {
            Reserve r;
            if (const Json* k = rv.find("kind")) { auto pk = reserve_kind_from_string(k->as_string().value_or("")); if (pk) r.kind = *pk; }
            if (const Json* n = rv.find("name")) r.name = n->as_string().value_or("");
            if (const Json* by = rv.find("bytes")) r.bytes = by->as_uint64().value_or(0);
            b.reserves.push_back(r);
        }
    }
    if (validate_budget(b)) return std::nullopt;
    return b;
}
// --------------------------------------------------------------- observation --
Json observation_to_json(const DomainObservation& o) {
    Json r = Json::object();
    r["id"] = Json(o.id.to_hex());
    r["type"] = str(to_string(o.type));
    r["provider"] = Json(o.provider);
    r["native_resource_id"] = Json(o.native_resource_id);
    r["total_capacity"] = Json(static_cast<std::int64_t>(o.total_capacity));
    r["committed"] = Json(static_cast<std::int64_t>(o.committed));
    r["resident"] = Json(static_cast<std::int64_t>(o.resident));
    if (o.reclaimable) r["reclaimable"] = Json(static_cast<std::int64_t>(*o.reclaimable));
    r["unavailable"] = Json(static_cast<std::int64_t>(o.unavailable));
    r["available"] = Json(static_cast<std::int64_t>(o.available));
    if (o.usable_capacity) r["usable_capacity"] = Json(static_cast<std::int64_t>(*o.usable_capacity));
    if (o.reserved) r["reserved"] = Json(static_cast<std::int64_t>(*o.reserved));
    r["confidence"] = str(to_string(o.confidence));
    r["provenance"] = str(to_string(o.provenance));
    r["validity"] = str(to_string(o.validity));
    r["observed_at_ms"] = Json(static_cast<std::int64_t>(o.observed_at_ms));
    r["allocation_failures"] = Json(static_cast<std::int64_t>(o.allocation_failures));
    r["growth_bytes_per_s"] = Json(o.growth_bytes_per_s);
    r["detail"] = Json(o.detail);
    return r;
}

std::optional<DomainObservation> observation_from_json(const Json& j) {
    if (!j.is_object()) return std::nullopt;
    DomainObservation o;
    if (const Json* v = j.find("id"); v && v->is_string()) {
        auto id = PressureDomainId::from_hex(v->as_string().value_or(""));
        if (!id) return std::nullopt;
        o.id = *id;
    } else { return std::nullopt; }
    if (const Json* v = j.find("type")) o.type = domain_type_from_string(v->as_string().value_or("")).value_or(DomainType::Unknown);
    if (const Json* v = j.find("provider")) o.provider = v->as_string().value_or("");
    if (const Json* v = j.find("native_resource_id")) o.native_resource_id = v->as_string().value_or("");
    auto req = [&](const char* key, std::uint64_t& out) -> bool {
        const Json* v = j.find(key);
        if (!v) return true;
        auto u = v->as_uint64();
        if (!u) return false;
        out = *u;
        return true;
    };
    if (!req("total_capacity", o.total_capacity)) return std::nullopt;
    if (!req("committed", o.committed)) return std::nullopt;
    if (!req("resident", o.resident)) return std::nullopt;
    if (!req("unavailable", o.unavailable)) return std::nullopt;
    if (!req("available", o.available)) return std::nullopt;
    if (!req("observed_at_ms", o.observed_at_ms)) return std::nullopt;
    if (const Json* v = j.find("reclaimable")) { auto u = v->as_uint64(); if (!u) return std::nullopt; o.reclaimable = *u; }
    if (const Json* v = j.find("usable_capacity")) { auto u = v->as_uint64(); if (!u) return std::nullopt; o.usable_capacity = *u; }
    if (const Json* v = j.find("reserved")) { auto u = v->as_uint64(); if (!u) return std::nullopt; o.reserved = *u; }
    if (const Json* v = j.find("confidence")) o.confidence = confidence_from_string(v->as_string().value_or("")).value_or(Confidence::Unknown);
    if (const Json* v = j.find("provenance")) o.provenance = provenance_from_string(v->as_string().value_or("")).value_or(Provenance::Unknown);
    if (const Json* v = j.find("validity")) o.validity = validity_from_string(v->as_string().value_or("")).value_or(Validity::Valid);
    if (const Json* v = j.find("allocation_failures")) o.allocation_failures = static_cast<int>(v->as_int64().value_or(0));
    if (const Json* v = j.find("growth_bytes_per_s")) o.growth_bytes_per_s = v->as_number().value_or(0.0);
    if (const Json* v = j.find("detail")) o.detail = v->as_string().value_or("");
    return o;
}

Json domain_state_to_json(const DomainState& d) {
    Json r = Json::object();
    r["id"] = Json(d.id.to_hex());
    r["type"] = str(to_string(d.type));
    r["provider"] = Json(d.provider);
    r["native_resource_id"] = Json(d.native_resource_id);
    r["budget"] = budget_to_json(d.budget);
    r["governed"] = Json(d.governed);
    r["total_capacity"] = Json(static_cast<std::int64_t>(d.total_capacity));
    r["usable_capacity"] = Json(static_cast<std::int64_t>(d.usable_capacity));
    r["reserved_bytes"] = Json(static_cast<std::int64_t>(d.reserved_bytes));
    r["committed"] = Json(static_cast<std::int64_t>(d.committed));
    r["resident"] = Json(static_cast<std::int64_t>(d.resident));
    r["available"] = Json(static_cast<std::int64_t>(d.available));
    r["reclaimable"] = Json(static_cast<std::int64_t>(d.reclaimable));
    r["utilization"] = Json(d.utilization);
    r["level"] = str(to_string(d.level));
    r["previous_level"] = str(to_string(d.previous_level));
    r["in_hysteresis_hold"] = Json(d.in_hysteresis_hold);
    r["level_hold_observations"] = Json(static_cast<std::int64_t>(d.level_hold_observations));
    r["score"] = Json(d.score);
    r["utilization_component"] = Json(d.utilization_component);
    r["free_deficit_component"] = Json(d.free_deficit_component);
    r["reserve_deficit_component"] = Json(d.reserve_deficit_component);
    r["growth_component"] = Json(d.growth_component);
    r["allocation_failure_component"] = Json(d.allocation_failure_component);
    r["stigma"] = Json(d.stigma);
    r["trend"] = str(to_string(d.trend));
    r["trend_rate"] = Json(d.trend_rate);
    r["trend_confidence"] = Json(d.trend_confidence);
    r["trend_window_samples"] = Json(static_cast<std::int64_t>(d.trend_window_samples));
    r["confidence"] = str(to_string(d.confidence));
    r["provenance"] = str(to_string(d.provenance));
    r["validity"] = str(to_string(d.validity));
    r["timestamp_ms"] = Json(static_cast<std::int64_t>(d.timestamp_ms));
    r["explanation"] = Json(d.explanation);
    r["entered_level_at_ms"] = Json(static_cast<std::int64_t>(d.entered_level_at_ms));
    r["peak_level"] = Json(static_cast<std::int64_t>(d.peak_level));
    Json resp = Json::array();
    for (const auto& a : d.responses) resp.push_back(str(to_string(a)));
    r["responses"] = resp;
    return r;
}

std::optional<DomainState> domain_state_from_json(const Json& j) {
    if (!j.is_object()) return std::nullopt;
    DomainState d;
    if (const Json* v = j.find("id"); v && v->is_string()) {
        auto id = PressureDomainId::from_hex(v->as_string().value_or(""));
        if (!id) return std::nullopt;
        d.id = *id;
    } else { return std::nullopt; }
    if (const Json* v = j.find("type")) d.type = domain_type_from_string(v->as_string().value_or("")).value_or(DomainType::Unknown);
    if (const Json* v = j.find("provider")) d.provider = v->as_string().value_or("");
    if (const Json* v = j.find("native_resource_id")) d.native_resource_id = v->as_string().value_or("");
    if (const Json* v = j.find("budget")) d.budget = budget_from_json(*v).value_or(Budget{});
    if (const Json* v = j.find("governed")) d.governed = v->as_bool().value_or(false);
    if (const Json* v = j.find("total_capacity")) d.total_capacity = v->as_uint64().value_or(0);
    if (const Json* v = j.find("usable_capacity")) d.usable_capacity = v->as_uint64().value_or(0);
    if (const Json* v = j.find("reserved_bytes")) d.reserved_bytes = v->as_uint64().value_or(0);
    if (const Json* v = j.find("committed")) d.committed = v->as_uint64().value_or(0);
    if (const Json* v = j.find("resident")) d.resident = v->as_uint64().value_or(0);
    if (const Json* v = j.find("available")) d.available = v->as_uint64().value_or(0);
    if (const Json* v = j.find("reclaimable")) d.reclaimable = v->as_uint64().value_or(0);
    if (const Json* v = j.find("utilization")) d.utilization = v->as_number().value_or(0.0);
    if (const Json* v = j.find("level")) d.level = pressure_level_from_string(v->as_string().value_or("")).value_or(PressureLevel::Unknown);
    if (const Json* v = j.find("previous_level")) d.previous_level = pressure_level_from_string(v->as_string().value_or("")).value_or(PressureLevel::Unknown);
    if (const Json* v = j.find("in_hysteresis_hold")) d.in_hysteresis_hold = v->as_bool().value_or(false);
    if (const Json* v = j.find("level_hold_observations")) d.level_hold_observations = v->as_uint64().value_or(0);
    if (const Json* v = j.find("score")) d.score = v->as_number().value_or(0.0);
    if (const Json* v = j.find("utilization_component")) d.utilization_component = v->as_number().value_or(0.0);
    if (const Json* v = j.find("free_deficit_component")) d.free_deficit_component = v->as_number().value_or(0.0);
    if (const Json* v = j.find("reserve_deficit_component")) d.reserve_deficit_component = v->as_number().value_or(0.0);
    if (const Json* v = j.find("growth_component")) d.growth_component = v->as_number().value_or(0.0);
    if (const Json* v = j.find("allocation_failure_component")) d.allocation_failure_component = v->as_number().value_or(0.0);
    if (const Json* v = j.find("stigma")) d.stigma = v->as_number().value_or(0.0);
    if (const Json* v = j.find("trend")) d.trend = trend_from_string(v->as_string().value_or("")).value_or(TrendDirection::Unknown);
    if (const Json* v = j.find("trend_rate")) d.trend_rate = v->as_number().value_or(0.0);
    if (const Json* v = j.find("trend_confidence")) d.trend_confidence = v->as_number().value_or(0.0);
    if (const Json* v = j.find("trend_window_samples")) d.trend_window_samples = v->as_uint64().value_or(0);
    if (const Json* v = j.find("confidence")) d.confidence = confidence_from_string(v->as_string().value_or("")).value_or(Confidence::Unknown);
    if (const Json* v = j.find("provenance")) d.provenance = provenance_from_string(v->as_string().value_or("")).value_or(Provenance::Unknown);
    if (const Json* v = j.find("validity")) d.validity = validity_from_string(v->as_string().value_or("")).value_or(Validity::Valid);
    if (const Json* v = j.find("timestamp_ms")) d.timestamp_ms = v->as_uint64().value_or(0);
    if (const Json* v = j.find("explanation")) d.explanation = v->as_string().value_or("");
    if (const Json* v = j.find("entered_level_at_ms")) d.entered_level_at_ms = v->as_uint64().value_or(0);
    if (const Json* v = j.find("peak_level")) d.peak_level = v->as_uint64().value_or(0);
    if (const Json* v = j.find("responses"); v && v->is_array()) {
        for (const auto& a : v->array_items()) {
            auto parsed = response_action_from_string(a.as_string().value_or(""));
            if (parsed) d.responses.push_back(*parsed);
        }
    }
    return d;
}

Json snapshot_to_json(const Snapshot& s) {
    Json r = Json::object();
    r["schema"] = Json(kSnapshotSchemaVersion);
    r["id"] = Json(static_cast<std::int64_t>(s.id));
    r["generation"] = Json(static_cast<std::int64_t>(s.generation));
    r["timestamp_ms"] = Json(static_cast<std::int64_t>(s.timestamp_ms));
    r["policy_version"] = Json(static_cast<std::int64_t>(s.policy_version));
    r["policy_name"] = Json(s.policy_name);
    r["aggregate_level"] = str(to_string(s.aggregate_level));
    r["aggregate_score"] = Json(s.aggregate_score);
    r["aggregate_explanation"] = Json(s.aggregate_explanation);
    r["stale_data"] = Json(s.stale_data);
    r["partial_data"] = Json(s.partial_data);
    r["confidence"] = str(to_string(s.confidence));
    r["provenance"] = str(to_string(s.provenance));
    Json warnings = Json::array();
    for (const auto& w : s.warnings) warnings.push_back(Json(w));
    r["warnings"] = warnings;
    Json health = Json::object();
    for (const auto& [k, v] : s.provider_health) health[k] = str(to_string(v));
    r["provider_health"] = health;
    Json domains = Json::array();
    for (const auto& d : s.domains) domains.push_back(domain_state_to_json(d));
    r["domains"] = domains;
    Json ev = Json::object();
    ev["count"] = Json(static_cast<std::int64_t>(s.events.count));
    ev["dropped"] = Json(static_cast<std::int64_t>(s.events.dropped));
    r["events"] = ev;
    return r;
}

std::optional<Snapshot> snapshot_from_json(const Json& j) {
    if (!j.is_object()) return std::nullopt;
    auto schema = j.find("schema");
    if (!schema || schema->as_int64().value_or(-1) != kSnapshotSchemaVersion) return std::nullopt;
    Snapshot s;
    if (const Json* v = j.find("id")) s.id = v->as_uint64().value_or(0);
    if (const Json* v = j.find("generation")) s.generation = v->as_uint64().value_or(0);
    if (const Json* v = j.find("timestamp_ms")) s.timestamp_ms = v->as_uint64().value_or(0);
    if (const Json* v = j.find("policy_version")) s.policy_version = v->as_uint64().value_or(0);
    if (const Json* v = j.find("policy_name")) s.policy_name = v->as_string().value_or("");
    if (const Json* v = j.find("aggregate_level")) s.aggregate_level = pressure_level_from_string(v->as_string().value_or("")).value_or(PressureLevel::Unknown);
    if (const Json* v = j.find("aggregate_score")) s.aggregate_score = v->as_number().value_or(0.0);
    if (const Json* v = j.find("aggregate_explanation")) s.aggregate_explanation = v->as_string().value_or("");
    if (const Json* v = j.find("stale_data")) s.stale_data = v->as_bool().value_or(false);
    if (const Json* v = j.find("partial_data")) s.partial_data = v->as_bool().value_or(false);
    if (const Json* v = j.find("confidence")) s.confidence = confidence_from_string(v->as_string().value_or("")).value_or(Confidence::Unknown);
    if (const Json* v = j.find("provenance")) s.provenance = provenance_from_string(v->as_string().value_or("")).value_or(Provenance::Unknown);
    if (const Json* v = j.find("warnings"); v && v->is_array()) { for (const auto& w : v->array_items()) s.warnings.push_back(w.as_string().value_or("")); }
    if (const Json* v = j.find("provider_health"); v && v->is_object()) { for (const auto& [k, hv] : v->object_items()) s.provider_health[k] = provider_status_from_string(hv.as_string().value_or("")).value_or(ProviderStatus::Unavailable); }
    if (const Json* v = j.find("domains"); v && v->is_array()) {
        std::vector<PressureDomainId> seen;
        for (const auto& dv : v->array_items()) {
            auto d = domain_state_from_json(dv);
            if (!d) return std::nullopt;
            for (const auto& sid : seen) if (sid == d->id) return std::nullopt;
            seen.push_back(d->id);
            s.domains.push_back(std::move(*d));
        }
    }
    if (const Json* v = j.find("events"); v && v->is_object()) {
        if (const Json* c = v->find("count")) s.events.count = static_cast<std::uint32_t>(c->as_uint64().value_or(0));
        if (const Json* c = v->find("dropped")) s.events.dropped = static_cast<std::uint32_t>(c->as_uint64().value_or(0));
    }
    return s;
}

Json event_to_json(const PressureEvent& e) {
    Json r = Json::object();
    r["schema"] = Json(kEventSchemaVersion);
    r["id"] = Json(static_cast<std::int64_t>(e.id));
    r["type"] = str(to_string(e.type));
    r["generation"] = Json(static_cast<std::int64_t>(e.generation));
    r["timestamp_ms"] = Json(static_cast<std::int64_t>(e.timestamp_ms));
    if (e.domain) r["domain"] = Json(e.domain->to_hex());
    r["detail"] = Json(e.detail);
    r["severity_rank"] = Json(static_cast<std::int64_t>(e.severity_rank));
    return r;
}

Json stats_to_json(const RuntimeStats& s) {
    Json r = Json::object();
    r["observations"] = Json(static_cast<std::int64_t>(s.observations));
    r["snapshots"] = Json(static_cast<std::int64_t>(s.snapshots));
    r["pressure_transitions"] = Json(static_cast<std::int64_t>(s.pressure_transitions));
    r["provider_failures"] = Json(static_cast<std::int64_t>(s.provider_failures));
    r["stale_observations"] = Json(static_cast<std::int64_t>(s.stale_observations));
    r["admission_accept"] = Json(static_cast<std::int64_t>(s.admission_accept));
    r["admission_caution"] = Json(static_cast<std::int64_t>(s.admission_caution));
    r["admission_defer"] = Json(static_cast<std::int64_t>(s.admission_defer));
    r["admission_reject"] = Json(static_cast<std::int64_t>(s.admission_reject));
    r["backpressure_signals"] = Json(static_cast<std::int64_t>(s.backpressure_signals));
    r["reclaim_requests"] = Json(static_cast<std::int64_t>(s.reclaim_requests));
    r["demotion_requests"] = Json(static_cast<std::int64_t>(s.demotion_requests));
    r["event_dropped"] = Json(static_cast<std::int64_t>(s.event_dropped));
    r["policy_changes"] = Json(static_cast<std::int64_t>(s.policy_changes));
    r["generation_changes"] = Json(static_cast<std::int64_t>(s.generation_changes));
    r["refresh_latency_us"] = Json(static_cast<std::int64_t>(s.refresh_latency_us));
    r["query_count"] = Json(static_cast<std::int64_t>(s.query_count));
    return r;
}

Json trace_to_json(const PressureTrace& t) {
    Json r = Json::object();
    r["schema"] = Json(kTraceSchemaVersion);
    r["policy_version"] = Json(static_cast<std::int64_t>(t.policy_version));
    Json frames = Json::array();
    for (const auto& f : t.frames) {
        Json ff = Json::object();
        ff["timestamp_ms"] = Json(static_cast<std::int64_t>(f.timestamp_ms));
        Json obs = Json::array();
        for (const auto& o : f.observations) obs.push_back(observation_to_json(o));
        ff["observations"] = obs;
        Json ph = Json::object();
        for (const auto& [k, v] : f.provider_health) ph[k] = str(to_string(v));
        ff["provider_health"] = ph;
        frames.push_back(ff);
    }
    r["frames"] = frames;
    return r;
}

std::optional<PressureTrace> trace_from_json(const Json& j) {
    if (!j.is_object()) return std::nullopt;
    auto schema = j.find("schema");
    if (!schema || schema->as_int64().value_or(-1) != kTraceSchemaVersion) return std::nullopt;
    PressureTrace t;
    if (const Json* v = j.find("policy_version")) t.policy_version = v->as_uint64().value_or(1);
    const Json* frames = j.find("frames");
    if (!frames || !frames->is_array()) return std::nullopt;
    for (const auto& f : frames->array_items()) {
        TraceFrame fr;
        if (const Json* v = f.find("timestamp_ms")) fr.timestamp_ms = v->as_uint64().value_or(0);
        if (const Json* v = f.find("observations"); v && v->is_array()) {
            for (const auto& ov : v->array_items()) {
                auto o = observation_from_json(ov);
                if (!o) return std::nullopt;
                fr.observations.push_back(std::move(*o));
            }
        }
        if (const Json* v = f.find("provider_health"); v && v->is_object()) {
            for (const auto& [k, hv] : v->object_items()) fr.provider_health[k] = provider_status_from_string(hv.as_string().value_or("")).value_or(ProviderStatus::Unavailable);
        }
        t.frames.push_back(std::move(fr));
    }
    return t;
}

} // namespace memory_pressure

