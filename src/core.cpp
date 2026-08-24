#include "memory_pressure/budget.h"
#include "memory_pressure/domain.h"
#include "memory_pressure/events.h"
#include "memory_pressure/hysteresis.h"
#include "memory_pressure/policy.h"
#include "memory_pressure/score.h"
#include "memory_pressure/snapshot.h"
#include "memory_pressure/velocity.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace memory_pressure {

namespace {

double clamp01(double x) noexcept {
    if (x < 0.0) return 0.0;
    if (x > 1.0) return 1.0;
    return x;
}

double confidence_penalty(Confidence c) noexcept {
    switch (c) {
        case Confidence::Authoritative: return 0.0;
        case Confidence::High:          return 0.15;
        case Confidence::Medium:        return 0.40;
        case Confidence::Low:           return 0.65;
        case Confidence::Unknown:       return 1.0;
    }
    return 1.0;
}

double validity_penalty(Validity v) noexcept {
    switch (v) {
        case Validity::Valid:       return 0.0;
        case Validity::Stale:       return 0.85;
        case Validity::Partial:     return 0.35;
        case Validity::Failed:      return 1.0;
        case Validity::Unavailable: return 1.0;
    }
    return 1.0;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Budget
// ---------------------------------------------------------------------------
std::optional<std::string> validate_budget(const Budget& b) noexcept {
    if (b.hard_capacity == 0) {
        // A zero hard capacity means "ungoverned"; still validate the rest.
        if (b.soft_capacity.has_value() && *b.soft_capacity != 0) {
            return "soft capacity set but no hard capacity";
        }
        return std::nullopt;
    }
    if (b.soft_capacity.has_value() && *b.soft_capacity > b.hard_capacity) {
        return "soft capacity exceeds hard capacity";
    }
    if (b.reserve_bytes() > b.hard_capacity) {
        return "reserves exceed hard capacity";
    }
    if (b.emergency_reserve_bytes > b.hard_capacity) {
        return "emergency reserve exceeds hard capacity";
    }
    if (b.minimum_free_reserve > b.hard_capacity) {
        return "minimum free reserve exceeds hard capacity";
    }
    if (b.reclaim_target > b.hard_capacity) {
        return "reclaim target exceeds hard capacity";
    }
    if (b.demotion_target > b.hard_capacity) {
        return "demotion target exceeds hard capacity";
    }
    if (b.admission_headroom > b.hard_capacity) {
        return "admission headroom exceeds hard capacity";
    }
    return std::nullopt;
}

Budget default_budget() {
    Budget b;
    b.hard_capacity = 0;   // ungoverned: derive from provider capacity
    return b;
}

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------
std::optional<std::string> validate_policy(const PressurePolicy& p) noexcept {
    const auto& t = p.thresholds;

    auto in01 = [](double v) { return v >= 0.0 && v <= 1.0 && std::isfinite(v); };
    // Ascending ordering of the threshold windows prevents descending severity
    // thresholds and guarantees hysteresis bands never invert.
    auto ascending = [](double a, double b) { return a <= b; };

    if (!in01(t.normal_exit) || !in01(t.elevated_exit) || !in01(t.elevated_enter) ||
        !in01(t.high_exit) || !in01(t.high_enter) || !in01(t.critical_exit) ||
        !in01(t.critical_enter) || !in01(t.exhausted_exit) || !in01(t.exhausted_enter)) {
        return "thresholds must all be finite and within [0,1]";
    }
    if (!(ascending(t.normal_exit, t.elevated_exit) &&
          ascending(t.elevated_exit, t.elevated_enter) &&
          ascending(t.elevated_enter, t.high_exit) &&
          ascending(t.high_exit, t.high_enter) &&
          ascending(t.high_enter, t.critical_exit) &&
          ascending(t.critical_exit, t.critical_enter) &&
          ascending(t.critical_enter, t.exhausted_exit) &&
          ascending(t.exhausted_exit, t.exhausted_enter))) {
        return "thresholds are not in ascending order (descending severity or inverted band)";
    }

    const auto& w = p.weights;
    auto wok = [](double v) { return v >= 0.0 && std::isfinite(v); };
    if (!wok(w.utilization) || !wok(w.free_deficit) || !wok(w.reserve_deficit) ||
        !wok(w.growth_rate) || !wok(w.allocation_failure) || !wok(w.fragmentation) ||
        !wok(w.reclaimable_deficit) || !wok(w.confidence_penalty) || !wok(w.stale_penalty)) {
        return "scoring weights must be finite and non-negative";
    }

    if (p.max_queued_events == 0) {
        return "max_queued_events must be positive";
    }

    // Hysteresis dwell must be at least 1 confirmation to be meaningful.
    if (p.hysteresis.min_dwell_observations == 0) {
        return "min_dwell_observations must be positive";
    }
    return std::nullopt;
}

PressurePolicy default_policy() {
    PressurePolicy p;
    p.version = 1;
    p.name = "default";
    p.max_relief_bytes = std::numeric_limits<std::uint64_t>::max();

    ResponseRule r1; r1.level = PressureLevel::High;
    r1.actions = {ResponseAction::Warn, ResponseAction::RequestReclaim};
    p.response_rules.push_back(r1);

    ResponseRule r2; r2.level = PressureLevel::Critical;
    r2.actions = {ResponseAction::RequestDemotion, ResponseAction::RequestReclaim,
                  ResponseAction::ReduceAdmission, ResponseAction::Throttle};
    p.response_rules.push_back(r2);

    ResponseRule r3; r3.level = PressureLevel::Exhausted;
    r3.actions = {ResponseAction::EmergencyStopGrowth, ResponseAction::RejectNewWork,
                  ResponseAction::RequestDemotion, ResponseAction::RequestReclaim};
    p.response_rules.push_back(r3);

    return p;
}

// ---------------------------------------------------------------------------
// Scoring
// ---------------------------------------------------------------------------
PressureScore score_domain(double utilization, bool governed, double free_fraction,
                           double reserve_deficit_fraction, double growth_factor,
                           int allocation_failures, Confidence confidence,
                           Validity validity, const ScoreWeights& w) {
    PressureScore s;
    s.score_version = 1;

    s.utilization_component = clamp01(utilization);
    s.free_deficit_component = clamp01(1.0 - free_fraction);
    s.reserve_deficit_component = clamp01(reserve_deficit_fraction);
    s.growth_component = clamp01(growth_factor);
    const double af = std::min(1.0, static_cast<double>(allocation_failures) / 5.0);
    s.allocation_failure_component = af;
    s.confidence_penalty = confidence_penalty(confidence);
    s.stale_penalty = validity_penalty(validity);

    if (!governed) {
        // Ungoverned: report utilization but do not weight free/reserve deficit
        // against a nonexistent budget; treat them conservatively at zero.
        s.free_deficit_component = clamp01(1.0 - free_fraction);
    }

    const double comps[] = {
        s.utilization_component, s.free_deficit_component, s.reserve_deficit_component,
        s.growth_component, s.allocation_failure_component,
        s.confidence_penalty, s.stale_penalty
    };
    const double weights[] = {
        w.utilization, w.free_deficit, w.reserve_deficit, w.growth_rate,
        w.allocation_failure, w.confidence_penalty, w.stale_penalty
    };
    double num = 0.0, den = 0.0;
    for (int i = 0; i < 7; ++i) {
        num += weights[i] * comps[i];
        den += weights[i];
    }
    s.value = den > 0.0 ? clamp01(num / den) : 0.0;
    return s;
}

// ---------------------------------------------------------------------------
// Trend estimation
// ---------------------------------------------------------------------------
TrendEstimator::TrendEstimator(std::size_t window_samples, std::uint64_t max_window_age_ms)
    : window_samples_(window_samples), max_window_age_ms_(max_window_age_ms) {}

void TrendEstimator::add_sample(std::uint64_t committed, std::uint64_t timestamp_ms) {
    std::uint64_t cutoff = max_window_age_ms_ > timestamp_ms ? 0 : timestamp_ms - max_window_age_ms_;
    while (!samples_.empty() && samples_.front().t_ms < cutoff) samples_.pop_front();
    samples_.push_back(Sample{timestamp_ms, committed});
    while (samples_.size() > window_samples_) samples_.pop_front();
}

void TrendEstimator::reset() { samples_.clear(); }

TrendEstimate TrendEstimator::estimate() const noexcept {
    TrendEstimate out;
    out.samples = samples_.size();
    if (samples_.size() < 2) return out;

    const double t0 = static_cast<double>(samples_.front().t_ms);
    std::size_t n = samples_.size();
    double mt = 0.0, mc = 0.0;
    for (const auto& s : samples_) { mt += static_cast<double>(s.t_ms) - t0; mc += static_cast<double>(s.committed); }
    mt /= static_cast<double>(n); mc /= static_cast<double>(n);
    double cov = 0.0, var = 0.0, varc = 0.0;
    for (const auto& s : samples_) {
        const double dt = (static_cast<double>(s.t_ms) - t0) - mt;
        const double dc = static_cast<double>(s.committed) - mc;
        cov += dt * dc; var += dt * dt; varc += dc * dc;
    }
    if (var <= 0.0) { out.direction = TrendDirection::Flat; out.confidence = 0.0; return out; }
    const double slope = cov / var;        // bytes per ms
    out.rate = slope * 1000.0;             // bytes per second
    const double eps = 1.0;
    if (out.rate > eps) out.direction = TrendDirection::Rising;
    else if (out.rate < -eps) out.direction = TrendDirection::Falling;
    else out.direction = TrendDirection::Flat;
    double r2 = varc > 0.0 ? (cov * cov) / (var * varc) : 0.0;
    if (r2 < 0.0) r2 = 0.0; if (r2 > 1.0) r2 = 1.0;
    double span_frac = static_cast<double>(n) / static_cast<double>(std::max<std::size_t>(window_samples_, 1));
    out.confidence = clamp01(std::sqrt(r2) * std::min(1.0, span_frac));
    return out;
}

// ---------------------------------------------------------------------------
// Threshold evaluation + hysteresis
// ---------------------------------------------------------------------------
PressureLevel evaluate_level(double utilization, PressureLevel current, const Thresholds& t) noexcept {
    // Fresh or reset state: classify by climbing enter thresholds.
    if (current == PressureLevel::Unknown || current == PressureLevel::Normal) {
        if (utilization >= t.exhausted_enter) return PressureLevel::Exhausted;
        if (utilization >= t.critical_enter) return PressureLevel::Critical;
        if (utilization >= t.high_enter) return PressureLevel::High;
        if (utilization >= t.elevated_enter) return PressureLevel::Elevated;
        return PressureLevel::Normal;
    }
    // For a known elevated level: climb using enter thresholds, drop using the
    // current level's exit threshold, and stay within the hysteresis band.
    switch (current) {
        case PressureLevel::Elevated:
            if (utilization >= t.exhausted_enter) return PressureLevel::Exhausted;
            if (utilization >= t.critical_enter) return PressureLevel::Critical;
            if (utilization >= t.high_enter) return PressureLevel::High;
            if (utilization >= t.elevated_enter) return PressureLevel::Elevated;
            if (utilization <= t.normal_exit) return PressureLevel::Normal;
            return PressureLevel::Elevated;
        case PressureLevel::High:
            if (utilization >= t.exhausted_enter) return PressureLevel::Exhausted;
            if (utilization >= t.critical_enter) return PressureLevel::Critical;
            if (utilization >= t.high_enter) return PressureLevel::High;
            if (utilization <= t.high_exit) return PressureLevel::Elevated;
            return PressureLevel::High;
        case PressureLevel::Critical:
            if (utilization >= t.exhausted_enter) return PressureLevel::Exhausted;
            if (utilization >= t.critical_enter) return PressureLevel::Critical;
            if (utilization <= t.critical_exit) return PressureLevel::High;
            return PressureLevel::Critical;
        case PressureLevel::Exhausted:
            if (utilization >= t.exhausted_enter) return PressureLevel::Exhausted;
            if (utilization <= t.exhausted_exit) return PressureLevel::Critical;
            return PressureLevel::Exhausted;
        case PressureLevel::Normal:
        case PressureLevel::Unknown:
            return PressureLevel::Normal;
    }
    return PressureLevel::Unknown;
}

HysteresisState::HysteresisState(const HysteresisConfig& cfg) : cfg_(cfg) {}

PressureLevel HysteresisState::update(PressureLevel raw, std::uint64_t now_ms) {
    if (raw == level_) {
        pending_ = PressureLevel::Unknown;
        pending_obs_ = 0;
        return level_;
    }
    const bool escalating = severity_rank(raw) > severity_rank(level_);
    if (raw == PressureLevel::Exhausted && cfg_.immediate_emergency_escalation && escalating) {
        level_ = raw; pending_ = PressureLevel::Unknown; pending_obs_ = 0;
        return level_;
    }
    if (pending_ != raw) {
        pending_ = raw; pending_obs_ = 1; pending_since_ms_ = now_ms;
    } else {
        ++pending_obs_;
    }
    if (pending_obs_ > cfg_.max_debounce_observations) pending_obs_ = cfg_.max_debounce_observations;

    const bool obs_ok = pending_obs_ >= cfg_.min_dwell_observations;
    const bool time_ok = (now_ms - pending_since_ms_) >= cfg_.min_dwell_duration_ms;
    const double wait = escalating ? cfg_.escalation_delay_ms : cfg_.recovery_delay_ms;
    const bool delay_ok = static_cast<double>(now_ms - pending_since_ms_) >= wait;

    if (obs_ok && time_ok && delay_ok) {
        level_ = raw; pending_ = PressureLevel::Unknown; pending_obs_ = 0;
        return level_;
    }
    return level_;
}

void HysteresisState::reset() noexcept {
    level_ = PressureLevel::Normal;
    pending_ = PressureLevel::Unknown;
    pending_obs_ = 0;
}

// ---------------------------------------------------------------------------
// Domain helper
// ---------------------------------------------------------------------------
std::uint64_t compute_domain_available(const Budget& b, const DomainObservation& o) noexcept {
    if (b.hard_capacity == 0) return o.available;
    const std::uint64_t usable = b.usable_capacity();
    return o.committed < usable ? usable - o.committed : 0;
}

// ---------------------------------------------------------------------------
// Subscription
// ---------------------------------------------------------------------------
Subscription::Subscription(SubscriptionFilter filter, std::size_t max_queue,
                           EventOverflowPolicy overflow, std::function<void(const PressureEvent&)> callback)
    : filter_(std::move(filter)), max_queue_(max_queue), overflow_policy_(overflow),
      callback_(std::move(callback)) {}

Subscription::~Subscription() = default;

bool Subscription::matches(const PressureEvent& e) const {
    if (!filter_.domains.empty()) {
        if (!e.domain) return false;
        if (std::find(filter_.domains.begin(), filter_.domains.end(), *e.domain) == filter_.domains.end()) return false;
    }
    if (!filter_.event_types.empty() &&
        std::find(filter_.event_types.begin(), filter_.event_types.end(), e.type) == filter_.event_types.end()) {
        return false;
    }
    if (!filter_.severities.empty()) {
        bool ok = false;
        for (const auto lv : filter_.severities) {
            if (severity_rank(lv) == static_cast<int>(e.severity_rank)) { ok = true; break; }
        }
        if (!ok) return false;
    }
    return true;
}

bool Subscription::push(const PressureEvent& e) {
    if (!matches(e)) return false;
    std::unique_lock<std::mutex> lk(mutex_);
    if (closed_) return false;
    if (queue_.size() >= max_queue_) {
        if (overflow_policy_ == EventOverflowPolicy::Reject) { ++rejected_; return false; }
        if (overflow_policy_ == EventOverflowPolicy::DropNewest) { ++dropped_; return false; }
        // DropOldest
        queue_.pop_front(); ++dropped_;
    }
    queue_.push_back(e);
    ++delivered_;
    auto cb = callback_;
    lk.unlock();
    if (cb) cb(e);
    return true;
}

bool Subscription::try_pop(PressureEvent& out) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

std::size_t Subscription::pending() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return queue_.size();
}

void Subscription::set_callback(std::function<void(const PressureEvent&)> cb) {
    std::lock_guard<std::mutex> lk(mutex_);
    callback_ = std::move(cb);
}

void Subscription::close() {
    std::lock_guard<std::mutex> lk(mutex_);
    closed_ = true;
    callback_ = nullptr;
}

bool Subscription::closed() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return closed_;
}

void Subscription::clear() {
    std::lock_guard<std::mutex> lk(mutex_);
    queue_.clear();
}

// ---------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------
const DomainState* Snapshot::find_domain(const PressureDomainId& domain_id) const noexcept {
    for (const auto& d : domains) if (d.id == domain_id) return &d;
    return nullptr;
}
DomainState* Snapshot::find_domain(const PressureDomainId& domain_id) noexcept {
    for (auto& d : domains) if (d.id == domain_id) return &d;
    return nullptr;
}

SnapshotDiff diff_snapshots(const Snapshot& a, const Snapshot& b) {
    SnapshotDiff d;
    auto find_state = [](const Snapshot& s, const PressureDomainId& id) -> const DomainState* {
        for (const auto& x : s.domains) if (x.id == id) return &x;
        return nullptr;
    };
    for (const auto& s : a.domains) if (!find_state(b, s.id)) d.removed_domains.push_back(s.id);
    for (const auto& s : b.domains) if (!find_state(a, s.id)) d.added_domains.push_back(s.id);
    for (const auto& s : b.domains) {
        const DomainState* pa = find_state(a, s.id);
        if (pa) {
            if (pa->level != s.level) d.level_changed.push_back(s.id);
            if ((pa->budget.hard_capacity != s.budget.hard_capacity) || (pa->budget.reserve_bytes() != s.budget.reserve_bytes())) {
                d.budget_changed.push_back(s.id.to_hex());
            }
            if (pa->budget.reserve_bytes() != s.budget.reserve_bytes()) d.reserve_changed.push_back(s.id);
        }
    }
    d.aggregate_changed = (a.aggregate_level != b.aggregate_level) || (a.aggregate_score != b.aggregate_score);
    d.policy_changed = a.policy_version != b.policy_version;
    d.provider_health_changed = (a.provider_health != b.provider_health);
    return d;
}

} // namespace memory_pressure
