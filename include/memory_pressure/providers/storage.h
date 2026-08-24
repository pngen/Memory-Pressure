#pragma once
#include <cstdint>
#include <string>
#include "memory_pressure/providers/provider.h"

namespace memory_pressure {

// Filesystem / persistent capacity provider.  Reports capacity and usage of a
// governed mount point via the OS filesystem API and maps it to a
// FILE_BACKED_MEMORY or PERSISTENT_STORAGE_CAPACITY pressure domain.
class StorageProvider : public Provider {
public:
    StorageProvider(std::string path, DomainType type = DomainType::PersistentStorageCapacity,
                    std::string resource_id = "filesystem");
    ~StorageProvider() override = default;

    std::string name() const noexcept override { return "storage"; }
    bool supported() const noexcept override;
    ProviderSample sample(std::uint64_t now_ms) override;

private:
    std::string path_;
    DomainType type_;
    std::string resource_id_;
};

} // namespace memory_pressure
