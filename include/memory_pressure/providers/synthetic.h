#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "memory_pressure/providers/provider.h"

namespace memory_pressure {

enum class SyntheticScenario : std::uint8_t {
    GradualGrowth = 0, RapidSpike = 1, Sawtooth = 2, SlowRecovery = 3, NoRecovery = 4,
    ProviderFailure = 5, StaleProvider = 6, GpuOnly = 7, HostOnly = 8, Simultaneous = 9,
    PersistentTier = 10, PinnedExhaustion = 11, MultiGpu = 12, OneRecovering = 13, Custom = 14
};

inline const char* to_string(SyntheticScenario v) noexcept {
    switch (v) {
        case SyntheticScenario::GradualGrowth: return "gradual-growth";
        case SyntheticScenario::RapidSpike: return "rapid-spike";
        case SyntheticScenario::Sawtooth: return "sawtooth";
        case SyntheticScenario::SlowRecovery: return "slow-recovery";
        case SyntheticScenario::NoRecovery: return "no-recovery";
        case SyntheticScenario::ProviderFailure: return "provider-failure";
        case SyntheticScenario::StaleProvider: return "stale-provider";
        case SyntheticScenario::GpuOnly: return "gpu-only";
        case SyntheticScenario::HostOnly: return "host-only";
        case SyntheticScenario::Simultaneous: return "simultaneous";
        case SyntheticScenario::PersistentTier: return "persistent-tier";
        case SyntheticScenario::PinnedExhaustion: return "pinned-exhaustion";
        case SyntheticScenario::MultiGpu: return "multi-gpu";
        case SyntheticScenario::OneRecovering: return "one-recovering";
        case SyntheticScenario::Custom: return "custom";
    }
    return "custom";
}

struct SyntheticDomainSpec {
    PressureDomainId id;
    DomainType type = DomainType::Unknown;
    std::string group;
    std::string provider;
    std::string native_resource_id;
    std::uint64_t total_capacity = 0;
    double phase = 0.0;
    double base = 0.15;
    bool present = true;
};

class SyntheticProvider : public Provider {
public:
    SyntheticProvider(std::vector<SyntheticDomainSpec> domains, SyntheticScenario scenario,
                      std::uint64_t step_ms = 100, std::uint64_t failure_after_steps = 4);
    ~SyntheticProvider() override = default;

    std::string name() const noexcept override { return "synthetic"; }
    bool supported() const noexcept override { return true; }
    ProviderSample sample(std::uint64_t now_ms) override;

    void reset() noexcept { tick_ = 0; }
    std::uint64_t tick() const noexcept { return tick_; }
    SyntheticScenario scenario() const noexcept { return scenario_; }

private:
    std::vector<SyntheticDomainSpec> domains_;
    SyntheticScenario scenario_;
    std::uint64_t step_ms_;
    std::uint64_t failure_after_steps_;
    std::uint64_t tick_ = 0;
};

} // namespace memory_pressure
