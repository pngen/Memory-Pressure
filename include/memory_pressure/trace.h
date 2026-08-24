#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <vector>
#include "memory_pressure/domain.h"
#include "memory_pressure/providers/provider.h"
#include "memory_pressure/types.h"

namespace memory_pressure {

// A single recorded observation batch (one refresh "frame").
struct TraceFrame {
    std::uint64_t timestamp_ms = 0;
    std::vector<DomainObservation> observations;
    std::map<std::string, ProviderStatus> provider_health;
};

// A deterministic, replayable pressure trace.
struct PressureTrace {
    std::uint64_t policy_version = 1;
    std::vector<TraceFrame> frames;
};

// A provider that replays a recorded trace frame-by-frame.  Registering a
// TraceProvider on a fresh runtime and calling refresh() once per frame
// reproduces the original pressure transitions deterministically.
class TraceProvider : public Provider {
public:
    explicit TraceProvider(std::shared_ptr<const PressureTrace> trace);
    ~TraceProvider() override = default;

    std::string name() const noexcept override { return "trace"; }
    bool supported() const noexcept override { return true; }
    ProviderSample sample(std::uint64_t now_ms) override;

    std::size_t index() const noexcept { return index_; }
    bool exhausted() const noexcept { return index_ >= (trace_ ? trace_->frames.size() : 0); }

private:
    std::shared_ptr<const PressureTrace> trace_;
    std::size_t index_ = 0;
};

} // namespace memory_pressure
