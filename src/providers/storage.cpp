#include "memory_pressure/providers/storage.h"

#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace memory_pressure {

StorageProvider::StorageProvider(std::string path, DomainType type, std::string resource_id)
    : path_(std::move(path)), type_(type), resource_id_(std::move(resource_id)) {}

bool StorageProvider::supported() const noexcept { return true; }

ProviderSample StorageProvider::sample(std::uint64_t now_ms) {
    ProviderSample out;
    out.sampled_at_ms = now_ms;
    out.status = ProviderStatus::Healthy;

#ifdef _WIN32
    ULARGE_INTEGER freeAvailable, total, totalFree;
    const std::wstring wpath(path_.begin(), path_.end());
    if (!GetDiskFreeSpaceExW(wpath.c_str(), &freeAvailable, &total, &totalFree)) {
        out.status = ProviderStatus::Unavailable;
        out.error = "GetDiskFreeSpaceExW failed";
        return out;
    }
    const std::uint64_t totalBytes = total.QuadPart;
    const std::uint64_t freeBytes = totalFree.QuadPart;
    DomainObservation o;
    o.id = PressureDomainId{0x73746F72ULL, PressureDomainId::fnv1a("storage::" + path_)};
    o.type = type_;
    o.provider = name();
    o.native_resource_id = resource_id_;
    o.total_capacity = totalBytes;
    o.committed = totalBytes >= freeBytes ? totalBytes - freeBytes : 0;
    o.resident = o.committed;
    o.available = freeBytes;
    o.confidence = Confidence::High;
    o.provenance = Provenance::Filesystem;
    o.validity = Validity::Valid;
    o.observed_at_ms = now_ms;
    o.detail = "path=" + path_;
    out.observations.push_back(std::move(o));
#else
    out.status = ProviderStatus::Unavailable;
    out.error = "storage provider unimplemented on this platform";
#endif
    return out;
}

} // namespace memory_pressure
