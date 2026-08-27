# Memory Pressure

> **When memory becomes scarce, how should the system know and respond *before* scarcity becomes failure?**

Memory Pressure is a vendor-neutral C++20 systems runtime for detecting, modeling, propagating, and **governing** memory pressure across heterogeneous AI infrastructure. It answers the question above in two halves:

1. **Knowing.** It observes memory (host RAM, commit, accelerator/device memory, storage) through pluggable providers and interprets raw numbers into an explicit, ordered pressure model: a `PressureLevel` (`Normal` -> `Elevated` -> `High` -> `Critical` -> `Exhausted`, plus a distinct `Unknown`), a bounded `PressureScore`, a `TrendEstimate`, and a per-domain `DomainState` -- all stamped with `Confidence` and `Provenance`.
2. **Responding.** It emits **recommendations**, never commands: a `Backpressure` signal, an `AdmissionHint`, a `DomainResponse`, and a `ReliefRequest` (reclaim or demotion) with a target and an urgency. Memory Pressure never moves a byte, never selects an object to reclaim, and never places work.

This separation is what makes it embeddable: it tells the rest of the system how much relief is needed and how urgent it is, and leaves actuation to the runtimes that own the data.

## Highlights

- **Explicit pressure, explicit evidence.** Every conclusion carries a `Provenance` (where the value came from) and a `Confidence` (how reliable it is).
- **Hysteresis + thresholds.** A deterministic, sticky, debounced state machine prevents threshold flapping.
- **Governed budgets and reserves.** A domain can be under pressure even when the raw device reports free bytes, because reserves set aside headroom.
- **Bounded by design.** Snapshot history, subscription queues, trend windows, and JSON input are all bounded.
- **Vendor-neutral.** The core has no link-time dependency on a vendor API. Providers load NVIDIA/driver or Windows libraries dynamically and degrade to `Unavailable` when absent.
- **Thread-safe.** Immutable snapshots, atomic policy replacement, single-writer refresh, and **no callbacks while any internal lock is held**.
- **Deterministic and replayable.** Synthetic and trace providers reproduce pressure transitions exactly.

## Building

Memory Pressure is a CMake project (min 3.22) targeting C++20. MSVC is the primary supported toolchain; Clang/GCC are supported behind the standard compiler checks.

**From a developer prompt on Windows:**

```text
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

**CUDA support**: the CUDA provider is always compiled in but loads the NVIDIA driver/runtime dynamically at runtime, so the library builds and runs even on CPU-only machines (where it degrades to `Unavailable`). The `MEMORY_PRESSURE_ENABLE_CUDA` option is a placeholder for stricter gating; leave it on.

**Build options:**

| Option | Default |
|--------|---------|
| `MEMORY_PRESSURE_BUILD_CLI` | `ON` |
| `MEMORY_PRESSURE_BUILD_TESTS` | `ON` |
| `MEMORY_PRESSURE_BUILD_EXAMPLES` | `ON` |
| `MEMORY_PRESSURE_BUILD_BENCHMARKS` | `ON` |
| `MEMORY_PRESSURE_ENABLE_CUDA` | `ON` (auto-detected) |

## Layout

```text
include/memory_pressure/   the public API (types, budget, policy, hysteresis,
                           score, velocity, domain, snapshot, response, events,
                           trace, serialize, json, providers/...)
src/                       library implementation (core, runtime, serialize,
                           json, trace, providers/...)
cli/                       the `memory-pressure` command-line tool
examples/                  runnable examples
tests/                     the test suite
docs/                      design and reference documentation
```

The core library is a static target `memory_pressure`, consumed as the CMake alias **`MemoryPressure::memory_pressure`**:

```cmake
find_package(MemoryPressure CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE MemoryPressure::memory_pressure)
target_compile_features(my_app PRIVATE cxx_std_20)
```

Consumers include `<memory_pressure/runtime.h>` (and the other public headers) and link `MemoryPressure::memory_pressure`. Install with `cmake --install build` to get the package config, headers, and library under the configured prefixes.

## Command-line tool

The `memory-pressure` executable (built when `MEMORY_PRESSURE_BUILD_CLI=ON`) supports these commands:

| Command | Purpose |
|---------|---------|
| `info` | show version and detected platform / capabilities (Windows + CUDA) |
| `observe` | sample providers and print raw observations |
| `domains` | refresh and list pressure domains |
| `status [--json]` | refresh and print current pressure status |
| `snapshot [--json]` | refresh and print the latest snapshot |
| `watch [--steps N] [--interval-ms M]` | bounded periodic refresh display |
| `budget --domain <id> --hard B [--emergency B]` | set a governed budget |
| `policy` | show the active policy |
| `pressure` | show pressure levels |
| `admit --domain <id> --bytes N` | show an admission hint |
| `explain --domain <id>` | show a structured explanation |
| `simulate --scenario <name> [--steps N]` | run a synthetic scenario |
| `events [--json]` | collect and print events from a refresh |
| `stats [--json]` | show runtime telemetry |
| `selftest` | run built-in self-checks |
| `benchmark` | run micro-benchmarks |

Examples:

```text
memory-pressure info
memory-pressure status --json
memory-pressure domains
memory-pressure budget --domain <id> --hard 8GiB --emergency 1GiB
memory-pressure admit --domain <id> --bytes 1073741824
memory-pressure simulate --scenario multi-gpu --steps 60
memory-pressure benchmark
```

## Libraries to use in code

- `PressureRuntime` -- the orchestrator: register providers, set budgets, set a policy, `refresh()` to produce an immutable `Snapshot`, and query it via `admit()`, `backpressure()`, `response_for()`, `explain()`.
- `Provider` -- implement `name()`, `supported()`, `sample(now_ms)` to observe a native resource.
- `Budget` / `Reserve` / `PressurePolicy` -- configure governance and sensitivity.
- `Subscription` / `PressureEvent` -- consume bounded event streams.
- `serialize.h` / `json.h` -- strict, versioned JSON for traces and interop.

See `docs/ARCHITECTURE.md` for the pipeline and `docs/POLICY.md` for the policy surface.

## Boundaries against other fabrics

Memory Pressure is a **sensing and signaling** runtime, not an actuator. It does not implement the larger fabrics it co-exists with; it supplies the pressure knowledge they consume. See `docs/INTEROP.md` for the full boundary table.

| Fabric | Memory Pressure provides | Memory Pressure does not do |
|--------|--------------------------|-----------------------------|
| Unified Buffer | accelerator/host pressure | it does not allocate or manage unified buffers |
| FlashTier | `RequestPersistence`, `RequestCompaction`, reserves | it does not manage flash capacity |
| Reclaim Fabric | `RequestReclaim` / `ReliefRequest` with `target_bytes` | it never reclaims anything |
| Transfer Fabric | `RequestDemotion`, transfer-staging reserve awareness | it moves no bytes |
| Topology Fabric | `TierRole` for aggregation & demotion feasibility | it does not own topology |
| Compute Fabric | per-domain accelerator pressure, `Backpressure` | it does not schedule work |

Where those fabrics exist, Memory Pressure is a consumer of nothing but its own configuration and a producer of pressure signals.

## Documentation

- `docs/ARCHITECTURE.md` -- module layout, pipeline, provider/runtime separation, thread-safety model.
- `docs/DESIGN.md` -- design principles.
- `docs/PRESSURE_MODEL.md`, `docs/THRESHOLDS.md`, `docs/HYSTERESIS.md`, `docs/DOMAINS.md`, `docs/BUDGETS.md`, `docs/POLICY.md` -- the model.
- `docs/BACKPRESSURE.md`, `docs/CROSS_TIER.md`, `docs/EVENTS.md` -- signals.
- `docs/PROVIDERS.md`, `docs/WINDOWS.md`, `docs/CUDA.md` -- providers and extension seams.
- `docs/SERIALIZATION.md`, `docs/TESTING.md`, `docs/BENCHMARKS.md` -- persistence, correctness, and methodology.
- `docs/INTEROP.md`, `docs/SECURITY.md`, `docs/LIMITATIONS.md` -- boundaries, hardening, and honest limitations.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
