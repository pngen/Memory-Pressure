# Providers

A **provider** is the only component in Memory Pressure that talks to a foreign API. It observes a native resource and emits `DomainObservation` records; it never interprets pressure. Providers live in `include/memory_pressure/providers/` and are registered on the runtime with `PressureRuntime::register_provider`.

## The Provider interface

```cpp
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
```

- `name()` is the stable provider identity (e.g. `"windows"`, `"cuda.driver"`, `"storage"`, `"synthetic"`, `"trace"`). It is used as the provider health key and, via `"provider::" + name()`, as the provider registration key.
- `supported()` reports whether the provider can function in the current build/runtime (e.g. CUDA present, platform is Windows).
- `sample(now_ms)` performs one poll and returns a `ProviderSample`.

A `ProviderSample` carries a `ProviderStatus`, zero or more `DomainObservation`s, an optional error string, and the `sampled_at_ms` timestamp. `ProviderStatus` is folded into the snapshot as described in ARCHITECTURE.md.

Providers are kept intentionally simple and stateless with respect to pressure: they know nothing about budgets, thresholds, hysteresis, or policy. That is the provider/runtime separation at the heart of the design.

## Shipped providers

| Provider | `name()` | Class | Platform | Observed domains |
|----------|----------|-------|----------|-----------------|
| Windows host | `windows` | `WindowsHostProvider` | Windows | `HOST_MEMORY`, `SYSTEM_COMMIT`, `PROCESS_COMMIT` |
| CUDA device | `cuda.driver` | `CudaDeviceProvider` | Windows/Linux w/ NVIDIA | `ACCELERATOR_MEMORY` (one per device) |
| Filesystem | `storage` | `StorageProvider` | Windows | `FILE_BACKED_MEMORY` or `PERSISTENT_STORAGE_CAPACITY` |
| Synthetic | `synthetic` | `SyntheticProvider` | any | caller-defined |
| Trace replay | `trace` | `TraceProvider` | any | recorded frames |

### Windows host provider

`WindowsHostProvider` reports physical RAM, system commit, and current-process commit from native Windows APIs. It is the only in-tree host-memory provider. See WINDOWS.md.

### CUDA device provider

`CudaDeviceProvider` reports one `ACCELERATOR_MEMORY` domain per NVIDIA device, using the CUDA runtime API for memory totals and the driver API for identity. It loads NVIDIA libraries dynamically, so a CPU-only host builds and runs; the provider simply reports `Unavailable`. See CUDA.md.

### Storage provider

`StorageProvider(path, type, resource_id)` observes a mount point via the OS filesystem API (`GetDiskFreeSpaceExW` on Windows) and maps it to a `FILE_BACKED_MEMORY` or `PERSISTENT_STORAGE_CAPACITY` domain. Domain identity is derived from the path. On non-Windows platforms it reports `Unavailable`.

### Synthetic provider

`SyntheticProvider` produces deterministic utilization curves for a set of `SyntheticDomainSpec`s under a chosen `SyntheticScenario`. It is used by the CLI `simulate`/`benchmark` commands, by the self-test, and by the test suite to drive reproducible pressure transitions. Scenarios include `GradualGrowth`, `RapidSpike`, `Sawtooth`, `SlowRecovery`, `NoRecovery`, `ProviderFailure`, `StaleProvider`, `GpuOnly`, `HostOnly`, `Simultaneous`, `PersistentTier`, `PinnedExhaustion`, `MultiGpu`, `OneRecovering`, and `Custom`.

### Trace provider

`TraceProvider` replays a recorded `PressureTrace` frame-by-frame. Registering it on a fresh runtime and calling `refresh()` once per frame reproduces the original pressure transitions deterministically, which is how `trace_replay.cpp` demonstrates deterministic replay. See TESTING.md.

## Extension seams

`Provider` is an abstract interface, so adding a vendor is a matter of subclassing it and registering an instance. The seams that exist today are:

- **A Linux/other host provider.** The in-tree host provider is Windows-only. A Linux host provider (e.g. reading `/proc/meminfo`, `sysinfo()`, or cgroup pressure files) would subclass `Provider` and emit `HOST_MEMORY`/`SYSTEM_COMMIT`-style observations. It is **not implemented** in this tree.
- **AMD HIP devices.** An AMD HIP provider would detect ROCm/HIP and report accelerator memory. It is **not implemented**; only NVIDIA CUDA is present.
- **Intel/OneAPI Level Zero devices.** A Level Zero provider would detect oneAPI/Level Zero and report accelerator memory. It is **not implemented**.
- **Vendor-neutral accelerator providers.** Any future accelerator backend implements the same three virtuals and is registered the same way.

### What is NOT implemented

The following are **extension seams only** in this tree: they are not implemented as working providers.

- Linux/other-OS host memory provider (Windows host provider is the only host provider).
- AMD HIP accelerator provider.
- Intel Level Zero accelerator provider.
- Any other OS filesystem backend (storage reports `Unavailable` off Windows).

The core library builds and runs without any of these; the platform-specific providers degrade to `Unavailable` on a host that does not expose the underlying API.

## Provider contract summary

- A provider emits observations; it never decides a level, score, or response.
- A provider stamps `confidence`, `provenance`, and `validity` on each observation so the runtime can reason about evidence quality.
- A provider may return a non-`Healthy` status; the runtime reacts (stale/failed/partial) without trusting the provider's data blindly.
- Provider registration is additive; two providers with the same `name()` collide under the same registration key.

See also: ARCHITECTURE.md (provider/runtime separation), DOMAINS.md (observation vs state), WINDOWS.md, CUDA.md.