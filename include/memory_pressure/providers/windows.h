#pragma once
#include <cstdint>
#include "memory_pressure/providers/provider.h"

namespace memory_pressure {

// Windows host-memory provider.  Produces three domains from native APIs:
//   HOST_MEMORY       - physical RAM (GlobalMemoryStatusEx)
//   SYSTEM_COMMIT     - system commit limit/usage (GetPerformanceInfo)
//   PROCESS_COMMIT    - current process private commit (GetProcessMemoryInfo)
// Values are stamped AUTHORITATIVE where they come directly from a native API.
class WindowsHostProvider : public Provider {
public:
    WindowsHostProvider();
    ~WindowsHostProvider() override = default;

    std::string name() const noexcept override { return "windows"; }
    bool supported() const noexcept override { return true; }
    ProviderSample sample(std::uint64_t now_ms) override;
};

} // namespace memory_pressure
