#include "memory_pressure/runtime.h"
#include "memory_pressure/score.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace memory_pressure {

namespace {

double clamp01(double v) noexcept { if (v < 0.0) return 0.0; if (v > 1.0) return 1.0; return v; }

double utilization_for(std::uint64_t committed, std::uint64_t usable) noexcept {
    if (usable == 0) return 0.0;
    return static_cast<double>(committed) / static_cast<double>(usable);
}

bool is_accelerator(DomainType t) noexcept { return t == DomainType::AcceleratorMemory; }
bool is_hostlike(DomainType t) noexcept {
    return t == DomainType::HostMemory || t == DomainType::PinnedHostMemory ||
           t == DomainType::SharedHostMemory || t == DomainType::ProcessCommit ||
           t == DomainType::SystemCommit;
}

TierRole role_for(DomainType t) noexcept {
    if (is_accelerator(t)) return TierRole::Accelerator;
    if (t == DomainType::HostMemory || t == DomainType::SystemCommit || t == DomainType::ProcessCommit) return TierRole::Root;
    if (t == DomainType::PinnedHostMemory || t == DomainType::SharedHostMemory) return TierRole::Staging;
    if (t == DomainType::PersistentStorageCapacity || t == DomainType::FileBackedMemory) return TierRole::Persistent;
    return TierRole::Custom;
}

std::uint64_t relief_bytes_for(PressureLevel level, const Budget& b) {
    switch (level) {
        case PressureLevel::High:      return b.reclaim_target ? b.reclaim_target : (b.hard_capacity ? b.hard_capacity / 8 : 0);
        case PressureLevel::Critical:  return b.reclaim_target ? (b.reclaim_target + b.demotion_target) : (b.hard_capacity ? b.hard_capacity / 4 : 0);
        case PressureLevel::Exhausted: return b.demotion_target ? b.demotion_target : (b.hard_capacity ? b.hard_capacity / 2 : 0);
        default: return 0;
    }
}

std::string build_explanation(const DomainState& d) {
    std::string s;
    s += std::string(to_string(d.type)) + " is " + std::string(to_string(d.level));
    s += " because governed capacity=" + std::to_string(d.total_capacity);
    s += " committed=" + std::to_string(d.committed);
    s += " available=" + std::to_string(d.available);
    s += " reserved=" + std::to_string(d.reserved_bytes);
    s += " utilization=" + std::to_string(d.utilization);
    s += " score=" + std::to_string(d.score);
    s += " trend=" + std::string(to_string(d.trend));
    return s;
}

PressureEvent make_pressure_event(std::uint64_t& next_id, PressureEventType type, std::uint64_t generation,
                                  std::uint64_t now_ms, const std::optional<PressureDomainId>& domain,
                                  std::string detail, std::uint32_t sev) {
    PressureEvent e;
    e.id = ++next_id;
    e.type = type;
    e.generation = generation;
    e.timestamp_ms = now_ms;
    e.domain = domain;
    e.detail = std::move(detail);
    e.severity_rank = sev;
    return e;
}

std::vector<ResponseAction> initial_actions(PressureLevel level) {
    switch (level) {
        case PressureLevel::Normal:    return {ResponseAction::None};
        case PressureLevel::Elevated:  return {ResponseAction::Warn};
        case PressureLevel::High:      return {ResponseAction::Warn, ResponseAction::RequestReclaim};
        case PressureLevel::Critical:  return {ResponseAction::RequestDemotion, ResponseAction::RequestReclaim,
                                               ResponseAction::ReduceAdmission, ResponseAction::Throttle};
        case PressureLevel::Exhausted: return {ResponseAction::EmergencyStopGrowth, ResponseAction::RejectNewWork,
                                               ResponseAction::RequestDemotion, ResponseAction::RequestReclaim};
        case PressureLevel::Unknown:   return {ResponseAction::None};
    }
    return {ResponseAction::None};
}

} // anonymous namespace

PressureRuntime::PressureRuntime() {
    policy_ = std::make_shared<const PressurePolicy>(default_policy());
    current_snapshot_ = std::make_shared<const Snapshot>(Snapshot{});
}

PressureRuntime::~PressureRuntime() {
    std::lock_guard<std::mutex> lk(registry_mutex_);
    for (auto& s : subscriptions_) if (s) s->close();
}

std::shared_ptr<const PressurePolicy> PressureRuntime::current_policy() const { return policy_; }

std::optional<std::string> PressureRuntime::set_policy(PressurePolicy p) {
    auto err = validate_policy(p);
    if (err) return err;
    {
        std::lock_guard<std::mutex> rl(refresh_mutex_);
        for (auto& kv : domain_states_) kv.second.hysteresis = HysteresisState(p.hysteresis);
    }
    policy_ = std::make_shared<const PressurePolicy>(std::move(p));
    {
        std::lock_guard<std::mutex> sl(stats_mutex_);
        ++stats_.policy_changes;
    }
    return std::nullopt;
}

void PressureRuntime::register_provider(std::shared_ptr<Provider> p) {
    if (!p) return;
    std::lock_guard<std::mutex> lk(registry_mutex_);
    providers_[PressureDomainId{0x70726F76ULL, PressureDomainId::fnv1a("provider::" + p->name())}] = std::move(p);
}

std::optional<std::string> PressureRuntime::set_budget(const PressureDomainId& id, const Budget& b) {
    auto err = validate_budget(b);
    if (err) return err;
    std::lock_guard<std::mutex> lk(config_mutex_);
    budgets_[id] = b;
    return std::nullopt;
}

void PressureRuntime::clear_budget(const PressureDomainId& id) {
    std::lock_guard<std::mutex> lk(config_mutex_);
    budgets_.erase(id);
}

std::shared_ptr<Subscription> PressureRuntime::subscribe(
    SubscriptionFilter filter, std::function<void(const PressureEvent&)> cb) {
    auto s = std::make_shared<Subscription>(std::move(filter), 256, EventOverflowPolicy::DropOldest, std::move(cb));
    std::lock_guard<std::mutex> lk(registry_mutex_);
    subscriptions_.push_back(s);
    return s;
}

void PressureRuntime::close_subscription(const std::shared_ptr<Subscription>& s) {
    if (s) s->close();
}

std::shared_ptr<const Snapshot> PressureRuntime::current_snapshot() const {
    std::lock_guard<std::mutex> lk(current_mutex_);
    return current_snapshot_;
}

std::shared_ptr<const Snapshot> PressureRuntime::snapshot_by_id(std::uint64_t id) const {
    std::lock_guard<std::mutex> lk(current_mutex_);
    for (const auto& s : history_) if (s && s->id == id) return s;
    return nullptr;
}

// ---------------------------------------------------------------- refresh ---
std::shared_ptr<const Snapshot> PressureRuntime::refresh(std::uint64_t now_ms) {
    std::lock_guard<std::mutex> rl(refresh_mutex_);
    auto pol = policy_;

    const auto start_clock = std::chrono::steady_clock::now();

    std::unordered_map<PressureDomainId, Budget, PressureDomainIdHash> bm;
    {
        std::lock_guard<std::mutex> cl(config_mutex_);
        bm = budgets_;
    }

    auto new_snap = std::make_shared<Snapshot>();
    new_snap->id = ++snapshot_id_;
    new_snap->timestamp_ms = now_ms;
    new_snap->policy_version = pol->version;
    new_snap->policy_name = pol->name;

    std::map<std::string, ProviderStatus> health = provider_health_;

    struct ProvObs { std::string name; std::vector<DomainObservation> obs; };
    std::vector<ProvObs> all;
    std::vector<PressureEvent> pending_events;

    std::vector<std::shared_ptr<Provider>> provs;
    {
        std::lock_guard<std::mutex> lk(registry_mutex_);
        for (auto& kv : providers_) provs.push_back(kv.second);
    }

    for (auto& prov : provs) {
        try {
            ProviderSample sample = prov->sample(now_ms);
            const ProviderStatus ps = sample.status;
            if (ps == ProviderStatus::Failed) {
                { std::lock_guard<std::mutex> sl(stats_mutex_); ++stats_.provider_failures; }
                pending_events.push_back(make_pressure_event(event_id_, PressureEventType::ProviderFailed,
                    0, now_ms, std::nullopt, prov->name() + " failed: " + sample.error, 0));
                new_snap->warnings.push_back(prov->name() + " failed: " + sample.error);
                new_snap->partial_data = true;
            } else if (ps == ProviderStatus::Stale) {
                pending_events.push_back(make_pressure_event(event_id_, PressureEventType::ProviderStale,
                    0, now_ms, std::nullopt, prov->name() + " is stale", 0));
                new_snap->stale_data = true;
                new_snap->warnings.push_back(prov->name() + " stale");
            } else if (ps == ProviderStatus::Unavailable) {
                pending_events.push_back(make_pressure_event(event_id_, PressureEventType::ProviderFailed,
                    0, now_ms, std::nullopt, prov->name() + " unavailable", 0));
                new_snap->partial_data = true;
            } else if (ps == ProviderStatus::Partial) {
                new_snap->partial_data = true;
            }
            health[prov->name()] = ps;
            all.push_back(ProvObs{prov->name(), std::move(sample.observations)});
        } catch (const std::exception& e) {
            health[prov->name()] = ProviderStatus::Failed;
            { std::lock_guard<std::mutex> sl(stats_mutex_); ++stats_.provider_failures; }
            new_snap->warnings.push_back(prov->name() + " threw: " + e.what());
            new_snap->partial_data = true;
        }
    }

    std::vector<DomainState> domains;
    std::unordered_set<PressureDomainId, PressureDomainIdHash> observed_ids;
    std::uint64_t obs_count = 0;

    for (auto& po : all) {
        for (auto& obs : po.obs) {
            observed_ids.insert(obs.id);
            auto it = domain_states_.find(obs.id);
            if (it == domain_states_.end()) {
                it = domain_states_.emplace(obs.id, DomainRuntimeState(pol->hysteresis, 16)).first;
            }
            DomainRuntimeState& st = it->second;
            if (st.type == DomainType::Unknown) st.type = obs.type;
            if (st.provider.empty()) st.provider = obs.provider;
            if (st.native_resource_id.empty()) st.native_resource_id = obs.native_resource_id;

            const auto bit = bm.find(obs.id);
            const bool governed = bit != bm.end() && bit->second.hard_capacity != 0;
            Budget bud = governed ? bit->second : default_budget();
            st.budget = bud;
            st.governed = governed;

            const std::uint64_t usable = governed ? bud.usable_capacity()
                                                  : (obs.usable_capacity.value_or(obs.total_capacity));
            const double util = utilization_for(obs.committed, usable);

            st.trend.add_sample(obs.committed, now_ms);
            const TrendEstimate te = st.trend.estimate();

            const PressureLevel prev = st.hysteresis.current();
            const PressureLevel raw = evaluate_level(util, prev, pol->thresholds);
            const PressureLevel resolved = st.hysteresis.update(raw, now_ms);
            if (resolved != prev) {
                st.entered_level_at_ms = now_ms;
                { std::lock_guard<std::mutex> sl(stats_mutex_); ++stats_.pressure_transitions; }
            }
            st.peak_level_rank = std::max<uint64_t>(st.peak_level_rank,
                static_cast<std::uint64_t>(std::max(0, severity_rank(resolved))));

            const std::uint64_t available = usable > obs.committed ? usable - obs.committed : 0;
            const double free_fraction = usable > 0 ? static_cast<double>(available) / static_cast<double>(usable) : 0.0;
            const double reserve_deficit = (usable > 0 && obs.committed > usable)
                ? clamp01(static_cast<double>(obs.committed - usable) / static_cast<double>(std::max<std::uint64_t>(usable, 1)))
                : 0.0;
            double growth_factor = 0.0;
            if (te.direction == TrendDirection::Rising) growth_factor = te.confidence;
            else if (te.direction == TrendDirection::Falling) growth_factor = 0.2 * te.confidence;

            const PressureScore sc = score_domain(util, governed, free_fraction, reserve_deficit,
                growth_factor, obs.allocation_failures, obs.confidence, obs.validity, pol->weights);

            DomainState ds;
            ds.id = obs.id;
            ds.type = obs.type;
            ds.provider = po.name;
            ds.native_resource_id = obs.native_resource_id;
            ds.budget = bud;
            ds.governed = governed;
            ds.total_capacity = governed ? bud.hard_capacity : obs.total_capacity;
            ds.usable_capacity = usable;
            ds.reserved_bytes = governed ? bud.reserve_bytes() : obs.reserved.value_or(0);
            ds.committed = obs.committed;
            ds.resident = obs.resident;
            ds.available = available;
            ds.reclaimable = obs.reclaimable.value_or(0);
            ds.utilization = util;
            ds.level = resolved;
            ds.previous_level = prev;
            ds.in_hysteresis_hold = st.hysteresis.in_hold();
            ds.level_hold_observations = st.hysteresis.pending_observations();
            ds.score = sc.value;
            ds.utilization_component = sc.utilization_component;
            ds.free_deficit_component = sc.free_deficit_component;
            ds.reserve_deficit_component = sc.reserve_deficit_component;
            ds.growth_component = sc.growth_component;
            ds.allocation_failure_component = sc.allocation_failure_component;
            ds.stigma = sc.confidence_penalty + sc.stale_penalty;
            ds.trend = te.direction;
            ds.trend_rate = te.rate;
            ds.trend_confidence = te.confidence;
            ds.trend_window_samples = te.samples;
            ds.confidence = obs.confidence;
            ds.provenance = obs.provenance;
            ds.validity = obs.validity;
            ds.timestamp_ms = now_ms;
            ds.entered_level_at_ms = st.entered_level_at_ms;
            ds.peak_level = st.peak_level_rank;

            domains.push_back(std::move(ds));
            ++obs_count;
        }
    }

    auto prev_snap = current_snapshot();
    if (prev_snap) {
        for (const auto& pd : prev_snap->domains) {
            if (observed_ids.find(pd.id) != observed_ids.end()) continue;
            const auto hh = health.find(pd.provider);
            const ProviderStatus ph = (hh == health.end()) ? ProviderStatus::Unavailable : hh->second;
            if (ph == ProviderStatus::Failed || ph == ProviderStatus::Stale || ph == ProviderStatus::Unavailable) {
                DomainState keeper = pd;
                keeper.validity = Validity::Stale;
                keeper.confidence = Confidence::Low;
                keeper.timestamp_ms = now_ms;
                keeper.explanation += " [retained; provider " + pd.provider + " not fresh]";
                new_snap->stale_data = true;
                pending_events.push_back(make_pressure_event(event_id_, PressureEventType::ProviderStale,
                    0, now_ms, pd.id, "retained stale: " + pd.provider, 0));
                domains.push_back(std::move(keeper));
            } else {
                pending_events.push_back(make_pressure_event(event_id_, PressureEventType::DomainRemoved,
                    0, now_ms, pd.id, "domain removed", 0));
                domain_states_.erase(pd.id);
            }
        }

        for (const auto& d : domains) {
            if (!prev_snap->find_domain(d.id)) {
                pending_events.push_back(make_pressure_event(event_id_, PressureEventType::DomainAdded,
                    0, now_ms, d.id, "domain " + std::string(to_string(d.type)) + " added", 0));
            }
        }
    }

    // Cross-tier response policy.
    std::uint64_t accel_sev = 0, host_sev = 0;
    bool host_present = false;
    for (const auto& d : domains) {
        const std::uint64_t r = static_cast<std::uint64_t>(std::max(0, severity_rank(d.level)));
        if (is_accelerator(d.type)) accel_sev = std::max(accel_sev, r);
        if (is_hostlike(d.type)) { host_present = true; host_sev = std::max(host_sev, r); }
    }
    (void)accel_sev;
    for (auto& d : domains) {
        d.responses = initial_actions(d.level);
        // Demotion is only safe when the host tier can actually accept the
        // displaced bytes.  If the host is at or above CRITICAL, drop the
        // demotion recommendation for any elevated accelerator (High, Critical,
        // or Exhausted) so we never suggest moving pressure into an equally-or
        // more-constrained tier.
        if (is_accelerator(d.type) && severity_rank(d.level) >= severity_rank(PressureLevel::High)) {
            if (!host_present || host_sev >= static_cast<std::uint64_t>(severity_rank(PressureLevel::Critical))) {
                d.responses.erase(std::remove(d.responses.begin(), d.responses.end(), ResponseAction::RequestDemotion), d.responses.end());
            }
        }
        d.explanation = build_explanation(d);
    }

    std::uint64_t agg_sev = 0;
    double agg_score = 0.0, wsum = 0.0;
    for (const auto& d : domains) {
        const std::uint64_t r = static_cast<std::uint64_t>(std::max(0, severity_rank(d.level)));
        agg_sev = std::max(agg_sev, r);
        double rw = 1.0;
        const auto rwi = pol->aggregate.role_weight.find(role_for(d.type));
        if (rwi != pol->aggregate.role_weight.end()) rw = rwi->second;
        agg_score += d.score * rw; wsum += rw;
    }
    if (domains.empty()) {
        new_snap->aggregate_level = PressureLevel::Unknown;
    } else if (agg_sev >= static_cast<std::uint64_t>(severity_rank(PressureLevel::Exhausted))) {
        new_snap->aggregate_level = PressureLevel::Exhausted;
    } else if (agg_sev >= static_cast<std::uint64_t>(severity_rank(PressureLevel::Critical))) {
        new_snap->aggregate_level = PressureLevel::Critical;
    } else if (agg_sev >= static_cast<std::uint64_t>(severity_rank(PressureLevel::High))) {
        new_snap->aggregate_level = PressureLevel::High;
    } else if (agg_sev >= static_cast<std::uint64_t>(severity_rank(PressureLevel::Elevated))) {
        new_snap->aggregate_level = PressureLevel::Elevated;
    } else {
        new_snap->aggregate_level = PressureLevel::Normal;
    }
    new_snap->aggregate_score = wsum > 0.0 ? clamp01(agg_score / wsum) : 0.0;
    new_snap->aggregate_explanation = "aggregate driven by max-severity domain";

    new_snap->domains = std::move(domains);
    new_snap->provider_health = std::move(health);
    new_snap->confidence = new_snap->partial_data ? Confidence::Medium : Confidence::High;
    new_snap->provenance = Provenance::RuntimeRegistration;

    const SnapshotDiff dd = prev_snap ? diff_snapshots(*prev_snap, *new_snap) : SnapshotDiff{};
    const bool changed = dd.any();
    if (changed) {
        ++generation_;
        { std::lock_guard<std::mutex> sl(stats_mutex_); ++stats_.generation_changes; }
        pending_events.push_back(make_pressure_event(event_id_, PressureEventType::GenerationChanged,
            generation_, now_ms, std::nullopt, "generation " + std::to_string(generation_), 0));
    }
    new_snap->generation = generation_;

    if (prev_snap) {
        for (const auto& ds : new_snap->domains) {
            const DomainState* pds = prev_snap->find_domain(ds.id);
            if (!pds || ds.level == pds->level) continue;
            const bool up = severity_rank(ds.level) > severity_rank(pds->level);
            if (up) {
                if (severity_rank(pds->level) <= severity_rank(PressureLevel::Normal)) {
                    pending_events.push_back(make_pressure_event(event_id_, PressureEventType::PressureEntered,
                        generation_, now_ms, ds.id, "entered " + std::string(to_string(ds.level)),
                        static_cast<std::uint32_t>(std::max(0, severity_rank(ds.level)))));
                } else {
                    pending_events.push_back(make_pressure_event(event_id_, PressureEventType::PressureEscalated,
                        generation_, now_ms, ds.id, "escalated to " + std::string(to_string(ds.level)),
                        static_cast<std::uint32_t>(std::max(0, severity_rank(ds.level)))));
                }
            } else if (ds.level == PressureLevel::Normal) {
                pending_events.push_back(make_pressure_event(event_id_, PressureEventType::PressureRecovered,
                    generation_, now_ms, ds.id, "recovered to NORMAL", 0));
            } else {
                pending_events.push_back(make_pressure_event(event_id_, PressureEventType::PressureRelieved,
                    generation_, now_ms, ds.id, "relieved to " + std::string(to_string(ds.level)),
                    static_cast<std::uint32_t>(std::max(0, severity_rank(ds.level)))));
            }
        }
    }

    for (const auto& ds : new_snap->domains) {
        for (const ResponseAction a : ds.responses) {
            if (a == ResponseAction::RequestReclaim) {
                { std::lock_guard<std::mutex> sl(stats_mutex_); ++stats_.reclaim_requests; }
                pending_events.push_back(make_pressure_event(event_id_, PressureEventType::ReclaimRequested,
                    generation_, now_ms, ds.id, "reclaim requested", static_cast<std::uint32_t>(std::max(0, severity_rank(ds.level)))));
            } else if (a == ResponseAction::RequestDemotion) {
                { std::lock_guard<std::mutex> sl(stats_mutex_); ++stats_.demotion_requests; }
                pending_events.push_back(make_pressure_event(event_id_, PressureEventType::DemotionRequested,
                    generation_, now_ms, ds.id, "demotion requested", static_cast<std::uint32_t>(std::max(0, severity_rank(ds.level)))));
            } else if (a == ResponseAction::ReduceAdmission || a == ResponseAction::RejectNewWork) {
                { std::lock_guard<std::mutex> sl(stats_mutex_); ++stats_.backpressure_signals; }
                pending_events.push_back(make_pressure_event(event_id_, PressureEventType::BackpressureIssued,
                    generation_, now_ms, ds.id, "backpressure:" + std::string(to_string(a)),
                    static_cast<std::uint32_t>(std::max(0, severity_rank(ds.level)))));
            }
        }
    }

    new_snap->events.count = static_cast<std::uint32_t>(pending_events.size());
    new_snap->events.dropped = 0;

    {
        std::lock_guard<std::mutex> cl(current_mutex_);
        current_snapshot_ = new_snap;
        history_.push_back(new_snap);
        while (history_.size() > history_limit_) history_.pop_front();
    }

    const auto end_clock = std::chrono::steady_clock::now();
    { std::lock_guard<std::mutex> sl(stats_mutex_);
      stats_.refresh_latency_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end_clock - start_clock).count());
      ++stats_.snapshots;
      stats_.observations += obs_count; }

    for (const auto& e : pending_events) {
        std::vector<std::shared_ptr<Subscription>> subs;
        { std::lock_guard<std::mutex> lk(registry_mutex_); subs = subscriptions_; }
        for (auto& s : subs) { if (s && !s->closed()) s->push(e); }
    }

    return new_snap;
}

// ---------------------------------------------------------------- queries ---
AdmissionHint PressureRuntime::admit(const PressureDomainId& id, std::uint64_t bytes) const {
    AdmissionHint h;
    h.requested_bytes = bytes;
    auto snap = current_snapshot();
    if (!snap) { h.decision = AdmissionDecision::Reject; h.explanation = "no snapshot available"; return h; }
    const DomainState* ds = snap->find_domain(id);
    if (!ds) { h.decision = AdmissionDecision::Defer; h.explanation = "domain unknown"; return h; }
    h.domain_level = ds->level;
    h.safe_bytes = ds->available;
    h.reserve_available = ds->available >= ds->budget.admission_headroom;

    const std::uint64_t avail = ds->available;
    switch (ds->level) {
        case PressureLevel::Normal:
            h.decision = (bytes <= avail) ? AdmissionDecision::Accept : AdmissionDecision::Defer;
            h.explanation = "domain NORMAL; request " + std::to_string(bytes) + " vs available " + std::to_string(avail);
            break;
        case PressureLevel::Elevated:
            h.decision = (bytes <= avail / 2) ? AdmissionDecision::AcceptWithCaution : AdmissionDecision::Defer;
            h.explanation = "domain ELEVATED; only cautious admission";
            break;
        case PressureLevel::High:
            h.decision = (bytes <= avail / 4) ? AdmissionDecision::AcceptWithCaution : AdmissionDecision::Defer;
            h.explanation = "domain HIGH; defer unless the request is small and headroom exists";
            break;
        case PressureLevel::Critical:
            h.decision = AdmissionDecision::Reject;
            h.explanation = "domain CRITICAL; reject new work";
            break;
        case PressureLevel::Exhausted:
            h.decision = AdmissionDecision::Reject;
            h.explanation = "domain EXHAUSTED; reject new work";
            break;
        case PressureLevel::Unknown:
            h.decision = AdmissionDecision::Defer;
            h.explanation = "domain UNKNOWN; defer";
            break;
    }

    std::lock_guard<std::mutex> sl(stats_mutex_);
    if (h.decision == AdmissionDecision::Accept) ++stats_.admission_accept;
    else if (h.decision == AdmissionDecision::AcceptWithCaution) ++stats_.admission_caution;
    else if (h.decision == AdmissionDecision::Defer) ++stats_.admission_defer;
    else ++stats_.admission_reject;
    return h;
}

Backpressure PressureRuntime::backpressure(const PressureDomainId& id) const {
    Backpressure bp;
    bp.target = id;
    auto snap = current_snapshot();
    if (!snap) return bp;
    const DomainState* ds = snap->find_domain(id);
    if (!ds) return bp;
    bp.severity = ds->level;
    bp.confidence = ds->confidence;
    bp.generation = snap->generation;
    bp.issued_at_ms = snap->timestamp_ms;
    bp.expiry_ms = ds->timestamp_ms + 10 * 1000ULL;
    bp.target_role = role_for(ds->type);
    bp.reason = std::string(to_string(ds->type)) + " " + std::string(to_string(ds->level));
    bp.recommended_response = ds->responses.empty() ? ResponseAction::None : ds->responses.front();
    bp.max_new_allocation = ds->governed ? (ds->available > 0 ? ds->available / 4 : 0) : (ds->available / 4);
    bp.admission_reduction = (ds->level == PressureLevel::High) ? 0.5 :
                             (ds->level == PressureLevel::Critical) ? 0.8 :
                             (ds->level == PressureLevel::Exhausted) ? 1.0 : 0.0;
    return bp;
}

DomainResponse PressureRuntime::response_for(const PressureDomainId& id) const {
    DomainResponse dr;
    dr.domain = id;
    auto snap = current_snapshot();
    if (!snap) return dr;
    const DomainState* ds = snap->find_domain(id);
    if (!ds) return dr;
    dr.level = ds->level;
    dr.actions = ds->responses;
    dr.explanation = ds->explanation;
    for (const ResponseAction a : ds->responses) {
        if (a == ResponseAction::RequestReclaim) {
            ReliefRequest r; r.kind = ReliefRequest::Kind::Reclaim;
            r.domain = id; r.urgency = ds->level; r.generation = snap->generation;
            r.target_bytes = relief_bytes_for(ds->level, ds->budget);
            r.minimum_useful_relief = std::min<std::uint64_t>(r.target_bytes ? r.target_bytes / 2 : 0, std::max<std::uint64_t>(1, ds->committed / 2));
            r.reason = "reclaim " + std::to_string(r.target_bytes) + " bytes";
            dr.relief = r;
        } else if (a == ResponseAction::RequestDemotion) {
            ReliefRequest r; r.kind = ReliefRequest::Kind::Demotion;
            r.domain = id; r.urgency = ds->level; r.generation = snap->generation;
            r.target_bytes = relief_bytes_for(ds->level, ds->budget);
            r.reason = "demote " + std::to_string(r.target_bytes) + " bytes";
            r.preferred_tier = DomainType::PinnedHostMemory;
            dr.relief = r;
        }
    }
    return dr;
}

std::string PressureRuntime::explain(const PressureDomainId& id) const {
    auto snap = current_snapshot();
    if (!snap) return "no snapshot";
    const DomainState* ds = snap->find_domain(id);
    if (!ds) return "domain not present";
    return ds->explanation;
}

} // namespace memory_pressure
