#include "memory_pressure/providers/windows.h"

#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

namespace memory_pressure {

namespace {

// Build a stable domain identity for a Windows-reported resource.
PressureDomainId make_id(const char* tag) noexcept {
    const std::uint64_t h = PressureDomainId::fnv1a("windows::" + std::string(tag));
    return PressureDomainId{0x77696E64ULL, h};   // stable per-resource identity
}

} // anonymous namespace

WindowsHostProvider::WindowsHostProvider() = default;

ProviderSample WindowsHostProvider::sample(std::uint64_t now_ms) {
    ProviderSample out;
    out.sampled_at_ms = now_ms;
    out.status = ProviderStatus::Healthy;

#ifdef _WIN32
    MEMORYSTATUSEX msx;
    msx.dwLength = sizeof(msx);
    if (!GlobalMemoryStatusEx(&msx)) {
        out.status = ProviderStatus::Failed;
        out.error = "GlobalMemoryStatusEx failed";
        return out;
    }

    // HOST_MEMORY: total physical RAM and in-use physical (resident) bytes.
    {
        DomainObservation o;
        o.id = make_id("host-memory");
        o.type = DomainType::HostMemory;
        o.provider = name();
        o.native_resource_id = "physical-ram";
        o.total_capacity = static_cast<std::uint64_t>(msx.ullTotalPhys);
        o.committed = static_cast<std::uint64_t>(msx.ullTotalPhys - msx.ullAvailPhys);
        o.resident = o.committed;
        o.available = static_cast<std::uint64_t>(msx.ullAvailPhys);
        o.confidence = Confidence::Authoritative;
        o.provenance = Provenance::WindowsGlobalMemoryStatusEx;
        o.validity = Validity::Valid;
        o.observed_at_ms = now_ms;
        o.detail = "GlobalMemoryStatusEx dwMemoryLoad=" +
                   std::to_string(static_cast<unsigned long long>(msx.dwMemoryLoad)) + "%";
        out.observations.push_back(std::move(o));
    }

    PERFORMANCE_INFORMATION pi;
    if (GetPerformanceInfo(&pi, sizeof(pi))) {
        const std::uint64_t pageSize = static_cast<std::uint64_t>(pi.PageSize);
        const std::uint64_t commitLimit = static_cast<std::uint64_t>(pi.CommitLimit) * pageSize;
        const std::uint64_t commitTotal = static_cast<std::uint64_t>(pi.CommitTotal) * pageSize;

        // SYSTEM_COMMIT domain.
        {
            DomainObservation o;
            o.id = make_id("system-commit");
            o.type = DomainType::SystemCommit;
            o.provider = name();
            o.native_resource_id = "system-commit";
            o.total_capacity = commitLimit;
            o.committed = commitTotal;
            o.resident = commitTotal;
            o.available = commitLimit >= commitTotal ? commitLimit - commitTotal : 0;
            o.confidence = Confidence::Authoritative;
            o.provenance = Provenance::WindowsPerformanceInfo;
            o.validity = Validity::Valid;
            o.observed_at_ms = now_ms;
            o.detail = "GetPerformanceInfo";
            out.observations.push_back(std::move(o));
        }

        // PROCESS_COMMIT domain for the current process.
        PROCESS_MEMORY_COUNTERS_EX pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
            const std::uint64_t priv = static_cast<std::uint64_t>(pmc.PrivateUsage);
            DomainObservation o;
            o.id = make_id("process-commit");
            o.type = DomainType::ProcessCommit;
            o.provider = name();
            o.native_resource_id = "current-process-commit";
            o.total_capacity = commitLimit;
            o.committed = priv;
            o.resident = static_cast<std::uint64_t>(pmc.WorkingSetSize);
            o.available = commitLimit >= commitTotal ? commitLimit - commitTotal : 0;
            o.confidence = Confidence::High;
            o.provenance = Provenance::WindowsProcessMemoryInfo;
            o.validity = Validity::Valid;
            o.observed_at_ms = now_ms;
            o.detail = "PrivateUsage=" + std::to_string(priv) + " WorkingSet=" +
                       std::to_string(static_cast<unsigned long long>(pmc.WorkingSetSize));
            out.observations.push_back(std::move(o));
        }
    } else {
        out.status = ProviderStatus::Partial;
        out.error = "GetPerformanceInfo failed; only physical RAM reported";
    }
#else
    out.status = ProviderStatus::Unavailable;
    out.error = "Windows host provider unavailable on non-Windows platform";
#endif
    return out;
}

} // namespace memory_pressure
