# CUDA device provider

`CudaDeviceProvider` (`name() == \"cuda.driver\"`) reports one `ACCELERATOR_MEMORY` domain per NVIDIA accelerator. It is defined in `include/memory_pressure/providers/cuda.h` and implemented in `src/providers/cuda.cpp`. It keeps the core library buildable on CPU-only systems by loading NVIDIA libraries `dynamically`.

## Dynamic loading

The provider loads two libraries at construction time (never at link time):

| Library | Role | Windows | Linux |
|---------|------|---------|-------|
| Runtime API | authoritative memory totals | `cudart64_13.dll`, `cudart64_12.dll`, `cudart64_11.dll` (or a toolkit install) | `libcudart.so.12` then `libcudart.so` |
| Driver API | device identity | `nvcuda.dll` | `libcuda.so.1` |

On Windows the runtime DLL is searched by name first, then by scanning the NVIDIA CUDA toolkit installation directory (`C:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA`) and preferring the highest-version `cudart64_<n>.dll` found. On Linux, `libcudart.so.12` is tried before `libcudart.so`. Symbols are resolved with `GetProcAddress` (Windows) or `dlsym` (Linux).

There is `no compile-time dependency` on CUDA headers or libraries. If neither library loads, the provider reports `Unavailable` (`supported() == false`) and the runtime treats it accordingly.

### Runtime API functions (memory + allocation)

`cudaSetDevice`, `cudaGetDeviceCount`, `cudaMemGetInfo`, `cudaMalloc`, `cudaFree`.

### Driver API functions (identity + utilities)

`cuInit`, `cuDeviceGetCount`, `cuDeviceGetName`, `cuDeviceGetUuid`.

## The cuMemGetInfo primary/child-context caveat

The provider uses the `runtime` API (`cudaMemGetInfo`), `not` the driver API (`cuMemGetInfo`), for per-device free/total memory. The reason is documented in the implementation:

> The runtime reports authoritative per-device totals on WDDM where the driver `cuMemGetInfo` may be capped.

On Windows Display Driver Model (WDDM) systems, a driver-level query like `cuMemGetInfo` can be `capped` to the current virtual (primary/child) context's view rather than the whole device. The runtime's `cudaMemGetInfo` returns the full device total, so the provider prefers it and stamps the observation with the `CudaDriverApi` provenance (the CUDA runtime API is part of the driver stack on NVIDIA).

In `sample()`, for each device the provider calls `cudaSetDevice(i)` then `cudaMemGetInfo(&freeMem, &totalMem)`. On success it records: total = `totalMem`, committed = `totalMem - freeMem`, resident = committed, available = `freeMem`, confidence = `High`, provenance = `CudaDriverApi`, validity = `Valid`. On failure it keeps the identity but sets confidence = `Unknown`, validity = `Failed`, and marks the provider `Partial`.

## Device identity and UUID

Domain identity is built from the `device UUID` when available, never from the transient ordinal alone:

```cpp
PressureDomainId DeviceDomainId(const unsigned char* uuid, bool have_uuid,
                                const char* name, int ordinal) {
    if (have_uuid) {
        // first 8 bytes -> high, next 8 bytes -> low
        return PressureDomainId{hi, lo};
    }
    // fallback: stable hash of name + ordinal
    return PressureDomainId{0x63756461ULL, fnv1a(name + "#" + ordinal)};
}
```

The native resource id is `uuid_hex + \"|\" + deviceName`, so the display string includes both the UUID (where available) and the device name. When a UUID is unavailable the fallback prefix `0x63756461` (the bytes of `\"cuda\"`) keeps the identity distinct from other providers.

## Allocation probes

`probe_device(int ordinal, std::uint64_t bytes)` performs a small bounded allocation on the given device and immediately frees it, returning true on success. It is a probe of whether an allocation of that size succeeds; it does not leak memory.

Probes are `disabled by default`. To enable them, construct the provider with `enable_probes = true`:

```cpp
CudaDeviceProvider cuda(/*enable_probes=*/true);
bool ok = cuda.probe_device(ordinal, 256ULL * 1024 * 1024);
```

The probe path is not used by `refresh()`; it is an explicit, opt-in diagnostic. Probe results are not wired into the automatic pressure interpretation in this tree.

## supported() and device_count()

`supported()` returns true only if a CUDA library loaded and at least one device was enumerated by either the runtime or driver API. `device_count()` returns the number of devices observed at the last init (0 when unsupported).

## Compute capability note

The provider targets NVIDIA CUDA 12.x runtime binaries (it loads `cudart64_12`/`cudart64_13` and `libcudart.so.12`). The validated hardware is a single NVIDIA device with compute capability 12.0 (Blackwell, the RTX 5090); broader multi-GPU or non-12.0-capability CUDA configurations are not validated in this tree (see LIMITATIONS.md).

See also: PROVIDERS.md (provider interface), DOMAINS.md (domain model), WINDOWS.md (host provider), LIMITATIONS.md (validation scope).