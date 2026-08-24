#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "memory_pressure/domain.h"
#include "memory_pressure/types.h"

namespace memory_pressure {

struct ProviderSample {
    ProviderStatus status = ProviderStatus::Healthy;
    std::vector<DomainObservation> observations;
    std::string error;
    std::uint64_t sampled_at_ms = 0;
};

class Provider {
public:
    virtual ~Provider() = default;
    virtual std::string name() const noexcept = 0;
    virtual bool supported() const noexcept = 0;
    virtual ProviderSample sample(std::uint64_t now_ms) = 0;
};

} // namespace memory_pressure
