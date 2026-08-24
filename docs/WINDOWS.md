# Windows host provider

`WindowsHostProvider` (`name() == "windows"`) observes host memory from native Windows APIs. It is defined in `include/memory_pressure/providers/windows.h` and implemented in `src/providers/windows.cpp`. It produces three pressure domains and stamps each observation with provenance and confidence.

| Domain | `DomainType` | Native API | `Provenance` | `Confidence` |
|--------|--------------|------------|--------------|--------------|
| `HOST_MEMORY` | `HostMemory` | `GlobalMemoryStatusEx` | `WindowsGlobalMemoryStatusEx` | `Authoritative` |
| `SYSTEM_COMMIT` | `SystemCommit` | `GetPerformanceInfo` | `WindowsPerformanceInfo` | `Authoritative` |
| `PROCESS_COMMIT` | `ProcessCommit` | `GetProcessMemoryInfo` | `WindowsProcessMemoryInfo` | `High` |

## Domain identity

Each domain gets a stable identity derived from a fixed tag, combining a stable provider prefix with an FNV-1a hash:

```cpp
PressureDomainId make_id(const char* tag) {
    const std::uint64_t h = PressureDomainId::fnv1a("windows::" + std::string(tag));
    return PressureDomainId{0x77696E64ULL, h};
}
// tags: "host-memory", "system-commit", "process-commit"
```

The `0x77696E64` prefix (the bytes of `"wind"`) keeps Windows identities distinct from those of other providers. Using a tag, not a transient handle, means the identity survives across refreshes and processes.

## Native APIs

### `GlobalMemoryStatusEx` (HOST_MEMORY)

`MEMORYSTATUSEX` reports physical memory. The provider records:

- `total_capacity = msx.ullTotalPhys`
- `committed = msx.ullTotalPhys - msx.ullAvailPhys` (in-use physical)
- `resident = committed`
- `available = msx.ullAvailPhys`
- `detail` includes `dwMemoryLoad` (percentage load).

If `GlobalMemoryStatusEx` fails, the provider returns `ProviderStatus::Failed` with error `GlobalMemoryStatusEx failed`.

### `GetPerformanceInfo` (SYSTEM_COMMIT)

`PERFORMANCE_INFORMATION` reports system commit. The provider multiplies page counts by `pi.PageSize` to recover bytes:

- `commitLimit = pi.CommitLimit * pageSize`
- `commitTotal = pi.CommitTotal * pageSize`
- `total_capacity = commitLimit`, `committed = commitTotal`, `resident = commitTotal`, `available = commitLimit - commitTotal` (0 if over).

The observation is stamped `Authoritative` with `provenance = WindowsPerformanceInfo` and `detail = GetPerformanceInfo`.

### `GetProcessMemoryInfo` (PROCESS_COMMIT)

`PROCESS_MEMORY_COUNTERS_EX` reports the current process private commit. The provider records:

- `total_capacity = commitLimit` (the same system commit limit)
- `committed = pmc.PrivateUsage`
- `resident = pmc.WorkingSetSize`
- `available = commitLimit - commitTotal`

This observation is stamped `High` (not `Authoritative`) with `provenance = WindowsProcessMemoryInfo`, because private usage is an approximation of this process commit rather than a system-wide truth.

## Failure and degradation

- If `GetPerformanceInfo` fails, the provider still returns the `HOST_MEMORY` domain but sets `ProviderStatus::Partial` with error `GetPerformanceInfo failed; only physical RAM reported`.
- On a non-Windows build, the whole provider returns `ProviderStatus::Unavailable`.

## What a consumer sees

Three domains are typically present on Windows: physical RAM (`HOST_MEMORY`), the system commit limit (`SYSTEM_COMMIT`), and the current process commit (`PROCESS_COMMIT`). Each is subject to the same interpretation stage (budget, threshold, hysteresis, scoring), so a budget can be attached to any of them. The commit domains are the ones most relevant for admission control, since they track the commit limit rather than physical pages.

See also: PROVIDERS.md (provider interface), DOMAINS.md (domain model), ARCHITECTURE.md (pipeline).