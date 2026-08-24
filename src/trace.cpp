#include "memory_pressure/trace.h"

namespace memory_pressure {

TraceProvider::TraceProvider(std::shared_ptr<const PressureTrace> trace)
    : trace_(std::move(trace)) {}

ProviderSample TraceProvider::sample(std::uint64_t now_ms) {
    ProviderSample out;
    out.sampled_at_ms = now_ms;
    if (!trace_ || index_ >= trace_->frames.size()) {
        out.status = ProviderStatus::Healthy;
        return out;
    }
    const TraceFrame& frame = trace_->frames[index_++];
    out.status = ProviderStatus::Healthy;
    out.observations = frame.observations;
    for (const auto& [name, st] : frame.provider_health) {
        (void)name; (void)st;
        // Provider health within a frame is honored by the runtime via
        // observation.validity; the frame-level health map is informational.
    }
    return out;
}

} // namespace memory_pressure
