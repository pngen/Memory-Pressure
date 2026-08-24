#pragma once
#include <cstdint>
#include <memory>
#include "memory_pressure/providers/provider.h"

namespace memory_pressure {

// CUDA device-memory provider backed by the NVIDIA *driver* API loaded
// dynamically from nvcuda.dll (or libcuda.so).  This keeps the core library
// buildable on CPU-only systems while still exercising real device telemetry on
// systems with a compatible NVIDIA driver.
//
// One pressure domain is produced per accelerator.  Domain identity is built
// from the device UUID where available (not the transient ordinal).
class CudaDeviceProvider : public Provider {
public:
    explicit CudaDeviceProvider(bool enable_probes = false);
    ~CudaDeviceProvider() override;

    CudaDeviceProvider(const CudaDeviceProvider&) = delete;
    CudaDeviceProvider& operator=(const CudaDeviceProvider&) = delete;

    std::string name() const noexcept override { return "cuda.driver"; }
    bool supported() const noexcept override;
    ProviderSample sample(std::uint64_t now_ms) override;

    // Perform a small bounded allocation probe on the given device ordinal,
    // immediately freeing it.  Returns true on success.  Probes are disabled
    // by default; callers that enable them must pass enable_probes = true.
    bool probe_device(int ordinal, std::uint64_t bytes);

    // Number of devices observed at the last init (0 when unsupported).
    int device_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace memory_pressure
