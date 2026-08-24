# Limitations

This document is an honest inventory of what Memory Pressure does not yet cover and what is a design boundary rather than a gap. It exists so consumers do not over-trust the library or mistake a seam for an implementation.

## GPU validation scope

- **Single-GPU validation.** The CUDA provider has been validated on a single NVIDIA device (compute capability 12.0, i.e. an RTX 5090-class Blackwell device). The `cuda...` path is validated for one device; behavior on other devices is not validated.
- **No real multi-GPU validation.** There is no validation against a real multi-GPU system. The `MultiGpu` / `OneRecovering` synthetic scenarios model a multi-GPU workload, but they are **synthetic** and do not prove real multi-GPU behavior, balancing, or device identity on a multi-device box.
- **Device identity fallback.** When the CUDA device UUID is unavailable, identity falls back to a hash of `name + ordinal`. That is stable for a given name/ordinal pair but is **not** stable across reinstallation or a different driver that re-numbers or renames devices.
- **Windows only for the host provider.** `WindowsHostProvider` is Windows-only (guarded by `#ifdef _WIN32`). On any other platform it reports `Unavailable`, and there is no Linux/other-OS host provider in the tree.

## JSON / capacity bound

- **`< 2^53` capacity round-trip.** `Json` stores numbers as double. Integer capacity round-trips are exact only below 2^53 (9,007,199,254,740,992, ~9 PiB); `as_uint64` **rejects** values above that bound rather than silently truncating. Any capacity greater than ~9 PiB cannot be represented losslessly in JSON.
- **Integrality is enforced.** A non-integral or out-of-range integer accessor returns `std::nullopt`. This is a strictness guarantee and a limitation if you ever need sub-byte or very-large value interchange.

## Pinned host memory budget is a policy, not hardware capacity

- `DomainType::PinnedHostMemory` exists and is governed, but the in-tree providers exercise **no dedicated native measurement** of pinned (page-locked host) memory. A governing budget on a pinned-memory domain is therefore a **policy designation**, not an authoritative hardware ceiling. The runtime enforces the budget you give it, but it is not measuring what the device can actually hold.

## Unimplemented providers / seams

The following are **extension seams only** in this tree and are **not** implemented as working providers:

- Linux or other-OS host memory provider (only `WindowsHostProvider` exists).
- AMD ROCm/HIP accelerator provider (only NVIDIA CUDA exists).
- Intel oneAPI / Level Zero accelerator provider (only NVIDIA CUDA exists).
- Non-Windows filesystem backend (`StorageProvider` uses `GetDiskFreeSpaceExW` on Windows and reports `Unavailable` elsewhere).

The core library builds and runs on hosts without these backends; they degrade to `Unavailable`, but they are not present.

## Score weights declared but not fully wired

- `ScoreWeights::fragmentation` and `ScoreWeights::reclaimable_deficit` are declared, but the current `score_domain` implementation does **not** compute a fragmentation or reclaimable-deficit component and does not include them in the weighted mean. `PressureScore` exposes the components it computes (utilization, free deficit, reserve deficit, growth, allocation failure, confidence penalty, stale penalty) only. Those two weights are reserved for a future component and currently have no effect on the score.

## Benchmarks

- The only in-tree benchmark is the CLI `benchmark` command over a synthetic 64-domain runtime. There is **no separate `benchmarks/` build tree** (no `benchmarks/CMakeLists.txt`), so `MEMORY_PRESSURE_BUILD_BENCHMARKS` should remain `OFF`. The synthetic path measures the runtime, not a real device observation. No reproducible, cross-platform throughput figures are asserted by the project.

## Concurrency model

- **Single-writer refresh.** `refresh()` is serialized (`refresh_mutex_`). Two concurrent `refresh()` calls cannot interleave, so the write path is not parallel. Readers are concurrent, but a single dominant polling writer is the expected mode.
- **No timeout wrappers in the harness.** The test harness has no per-test timeout, so a deadlock in a test hangs the run rather than failing it. Guard CI with an external timeout.

## Other honest boundaries

- **Aggregate is max-driven.** `aggregate_level` is the max severity rank. A single highly-stressed domain can set the aggregate to a high level even when most domains are healthy; this is intentional, but it is a coarse model, not a balanced average (the `aggregate_score` is the weighted average, the `aggregate_level` is the max).
- **Role weights default to 1.0.** `role_weight` is empty by default, so all tiers weight equally until configured; this is a conservative default, not a tuned topology model.
- **Freshness is a 10 s default.** Provider freshness bounds default to 10 s. A provider that becomes silently stale is only detected at the next refresh and only if it reports a stale status or its data age exceeds the bound.
- **Cross-tier demotion suppression only for accelerators at `High`/`Critical`.** The rule does not suppress `RequestDemotion` at `Exhausted`, and it does not model infeasibility for non-accelerator domains.
- **`events_probe.h` is a minimal probe surface.** It is not the full event model; the runtime emits the full `events.h` model.
- **Recommendations, not enforcement.** Memory Pressure never moves bytes, selects objects, or enforces quota. Any guarantee of `reclaim happened` or `quota enforced` is the responsibility of the consuming fabric.

See also: CUDA.md (validation scope), SERIALIZATION.md (< 2^53 bound), PROVIDERS.md (unimplemented seams), BENCHMARKS.md (methodology).