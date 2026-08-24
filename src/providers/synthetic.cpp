#include "memory_pressure/providers/synthetic.h"

#include <algorithm>
#include <cmath>

namespace memory_pressure {

namespace {

double clamp01(double v) noexcept {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

bool is_hostlike(const std::string& g) noexcept {
    return g == "host" || g == "commit" || g == "pinned" || g == "shared";
}

// A deterministic utilization curve for the "primary" pressured group.
double primary_curve(SyntheticScenario sc, double phase, std::uint64_t tick) {
    const double p = std::fmod(std::abs(phase), 1.0);
    switch (sc) {
        case SyntheticScenario::GradualGrowth: {
            const double prog = std::min(1.0, static_cast<double>(tick) / 50.0);
            return clamp01(0.15 + 0.85 * prog * (1.0 - 0.3 * p));
        }
        case SyntheticScenario::RapidSpike:
            return tick >= 6 ? clamp01(0.95 + 0.04 * p) : clamp01(0.2 + 0.25 * p);
        case SyntheticScenario::Sawtooth: {
            const double T = 24.0;
            const double tt = std::fmod(static_cast<double>(tick) + p * T, T);
            const double x = tt / T;
            const double lo = 0.25, hi = 0.98;
            return x < 0.5 ? lo + (hi - lo) * (2.0 * x) : hi - (hi - lo) * (2.0 * x - 1.0);
        }
        case SyntheticScenario::SlowRecovery:
            if (tick < 20) return clamp01(0.15 + 0.82 * std::min(1.0, static_cast<double>(tick) / 20.0));
            return clamp01(0.97 - 0.82 * std::min(1.0, static_cast<double>(tick - 20) / 100.0));
        case SyntheticScenario::NoRecovery:
            return tick >= 18 ? 0.98 : clamp01(0.15 + 0.83 * std::min(1.0, static_cast<double>(tick) / 18.0));
        default:
            return clamp01(0.15 + 0.85 * std::min(1.0, static_cast<double>(tick) / 50.0) * (1.0 - 0.3 * p));
    }
}

double util_for(const SyntheticDomainSpec& d, SyntheticScenario sc, std::uint64_t tick) {
    const std::string& g = d.group;
    const double base = d.base;
    switch (sc) {
        case SyntheticScenario::GpuOnly:
            return g == "gpu" ? primary_curve(SyntheticScenario::GradualGrowth, d.phase, tick) : base;
        case SyntheticScenario::HostOnly:
            return is_hostlike(g) ? primary_curve(SyntheticScenario::GradualGrowth, d.phase, tick) : base;
        case SyntheticScenario::Simultaneous:
            if (g == "gpu") return primary_curve(SyntheticScenario::GradualGrowth, d.phase, tick);
            if (is_hostlike(g)) return primary_curve(SyntheticScenario::GradualGrowth, d.phase, tick + 10);
            return base;
        case SyntheticScenario::PersistentTier:
            return (g == "persistent") ? primary_curve(SyntheticScenario::GradualGrowth, d.phase, tick) : base;
        case SyntheticScenario::PinnedExhaustion:
            return (g == "pinned") ? clamp01(0.05 + 0.95 * std::min(1.0, static_cast<double>(tick) / 30.0)) : base;
        case SyntheticScenario::MultiGpu:
            if (g == "gpu") {
                const double ramp = std::min(1.0, static_cast<double>(tick) / (40.0 + 15.0 * d.phase));
                return clamp01(d.base + (0.97 - d.base) * ramp);
            }
            return base;
        case SyntheticScenario::OneRecovering: {
            // A single "accelerator" that worsens while a "gpu2" recovers.
            if (g == "gpu0") return tick >= 15 ? 0.97 : clamp01(0.2 + 0.77 * std::min(1.0, static_cast<double>(tick) / 15.0));
            if (g == "gpu1") return tick >= 20 ? clamp01(0.97 - 0.72 * std::min(1.0, static_cast<double>(tick - 20) / 60.0)) : 0.97;
            return base;
        }
        default:
            // GradualGrowth, RapidSpike, Sawtooth, SlowRecovery, NoRecovery,
            // ProviderFailure, StaleProvider, Custom: pressurize the primary
            // group (gpu if present, else host-like), others stay at base.
            if (!g.empty() && (g == "gpu" || is_hostlike(g))) {
                if (g == "gpu") return primary_curve(sc, d.phase, tick);
                if (sc == SyntheticScenario::HostOnly || sc == SyntheticScenario::Simultaneous) return primary_curve(sc, d.phase, tick);
                return is_hostlike(g) && sc != SyntheticScenario::GpuOnly ? primary_curve(sc, d.phase, tick) : base;
            }
            return base;
    }
}

} // anonymous namespace

SyntheticProvider::SyntheticProvider(std::vector<SyntheticDomainSpec> domains,
                                     SyntheticScenario scenario,
                                     std::uint64_t step_ms,
                                     std::uint64_t failure_after_steps)
    : domains_(std::move(domains)), scenario_(scenario), step_ms_(step_ms),
      failure_after_steps_(failure_after_steps) {}

ProviderSample SyntheticProvider::sample(std::uint64_t now_ms) {
    // First sample is tick 0; increment at the end so replay is well-defined.
    const std::uint64_t tick = tick_;

    ProviderSample out;
    out.sampled_at_ms = now_ms;

    // Provider-failure: after N steps the provider reports a hard failure.
    if (scenario_ == SyntheticScenario::ProviderFailure && tick >= failure_after_steps_) {
        out.status = ProviderStatus::Failed;
        out.error = "synthetic provider failure (scenario)";
        ++tick_;
        return out;
    }

    // Stale-provider: emit observations stamped in the distant past.
    const bool stale = (scenario_ == SyntheticScenario::StaleProvider && tick >= 3);
    out.status = stale ? ProviderStatus::Stale : ProviderStatus::Healthy;
    if (stale) out.error = "synthetic provider stale (scenario)";
    const std::uint64_t observed_at = stale ? (now_ms > 10 * 60 * 1000ULL ? now_ms - 10 * 60 * 1000ULL : now_ms) : now_ms;

    for (const auto& d : domains_) {
        if (!d.present) continue;
        DomainObservation o;
        o.id = d.id;
        o.type = d.type;
        o.provider = d.provider.empty() ? "synthetic" : d.provider;
        o.native_resource_id = d.native_resource_id;
        o.total_capacity = d.total_capacity;
        const double u = util_for(d, scenario_, tick);
        // Purposely allow committed to exceed capacity for exhaustion scenarios
        // so the runtime's governed-budget logic can trip EXHAUSTED.
        o.committed = static_cast<std::uint64_t>(static_cast<double>(d.total_capacity) * u);
        o.resident = o.committed;
        o.available = o.committed < o.total_capacity ? o.total_capacity - o.committed : 0;
        o.confidence = Confidence::High;
        o.provenance = Provenance::SyntheticInput;
        o.validity = stale ? Validity::Stale : Validity::Valid;
        o.observed_at_ms = observed_at;
        o.detail = "synthetic:" + std::string(to_string(scenario_));
        out.observations.push_back(std::move(o));
    }

    ++tick_;
    return out;
}

} // namespace memory_pressure
