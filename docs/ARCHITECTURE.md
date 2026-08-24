# Architecture

Memory Pressure is a C++20 systems runtime that detects, models, propagates, and
governs memory pressure across heterogeneous AI infrastructure. It lives in the
`memory_pressure` namespace and is exposed as a single static library,
`MemoryPressure::memory_pressure`.

This document describes the module layout, the observation → interpretation →
response pipeline, the separation between *providers* and the *runtime*, and the
thread-safety model.

## Repository layout

```
include/memory_pressure/
  types.h        Core shared vocabulary (enums, PressureDomainId, string helpers)
  version.h      Version metadata (1.0.0)
  budget.h       Budget, Reserve, validation
  policy.h       Versioned PressurePolicy (thresholds, hysteresis, weights, rules)
  hysteresis.h   Threshold evaluation + HysteresisState machine
  score.h        PressureScore and score_domain()
  velocity.h     TrendEstimator / TrendEstimate
  domain.h       DomainObservation (raw) + DomainState (interpreted)
  snapshot.h     Snapshot, SnapshotDiff
  response.h     Backpressure, AdmissionHint, ReliefRequest, DomainResponse
  events.h       PressureEvent, Subscription, EventOverflowPolicy
  trace.h        PressureTrace, TraceFrame, TraceProvider
  serialize.h    JSON (de)serialization for policy/snapshot/events/trace/stats
  json.h         Strict, bounded JSON value type + parser/serializer
  providers/
    provider.h   Provider base class + ProviderSample
    windows.h    WindowsHostProvider
    cuda.h       CudaDeviceProvider
    storage.h    StorageProvider
    synthetic.h  SyntheticProvider + scenarios
src/
  json.cpp       JSON parser/serializer
  core.cpp       Pure functions: budget/policy validation, scoring, trends,
                 threshold+hysteresis, subscriptions, snapshot diff
  runtime.cpp    PressureRuntime: the orchestration pipeline
  serialize.cpp  JSON (de)serialization
  trace.cpp      TraceProvider
  providers/     Provider implementations
cli/main.cpp     memory-pressure command-line tool
examples/        Runnable examples (see examples/CMakeLists.txt)
tests/           Test suite (see TESTING.md)
```

The library is built from `src/CMakeLists.txt` as a static target named
`memory_pressure`, aliased as `MemoryPressure::memory_pressure`. Consumers link
`MemoryPressure::memory_pressure` and include `<memory_pressure/...>`.

## The pipeline

The runtime executes a single loop, driven by the caller calling
`PressureRuntime::refresh(now_ms)`. Each refresh performs the
observation → interpretation → response pipeline.

### 1. Observation

`refresh()` polls every registered `Provider` via
`Provider::sample(now_ms)`. Each provider returns a `ProviderSample`, which
carries a `ProviderStatus` and a `std::vector<DomainObservation>`. A
`DomainObservation` is the *raw, provider-reported truth* for one domain. It is
never interpreted: it records provider identity, native resource identity,
total/committed/resident/available bytes, an optional provider-reported
reclaimable and reserved amount, plus `Confidence`, `Provenance`, and `Validity`
stamps. Unobserved fields use `std::nullopt` so that "unknown" is never silently
conflated with "zero".

Provider status is folded into the snapshot:

| `ProviderStatus` | Effect |
|------------------|--------|
| `Healthy`        | normal observation |
| `Stale`          | sets `stale_data`, emits `ProviderStale` |
| `Partial`        | sets `partial_data` |
| `Failed`         | sets `partial_data`, emits `ProviderFailed`, adds a warning |
| `Unavailable`    | sets `partial_data`, emits `ProviderFailed` |

A provider that throws is treated as `Failed`.

### 2. Interpretation

For every observation, the runtime produces a `DomainState`. The runtime keeps a
persistent per-domain `DomainRuntimeState` that owns a `HysteresisState` and a
`TrendEstimator` so that pressure history is retained across refreshes.

Interpretation applies, in order:

1. **Budget resolution** — a governing `Budget` is looked up for the domain's id.
   If a budget with a non-zero `hard_capacity` exists, the domain is
   *governed*; otherwise it is *ungoverned* and the provider's raw
   `total_capacity`/`usable_capacity` is used.
2. **Utilization** — `committed / usable_capacity`. When `usable_capacity` is
   zero, utilization is `0.0`.
3. **Trend** — the committed amount is fed to the domain's `TrendEstimator`,
   which produces a bounded `TrendEstimate` (direction, rate in bytes/sec,
   confidence).
4. **Level** — `evaluate_level(utilization, prev, thresholds)` classifies the
   raw threshold-band result; `HysteresisState::update(raw, now_ms)` converts
   it into a sticky, debounced resolved level.
5. **Score** — `score_domain(...)` produces a `PressureScore` from the
   normalized components and configured weights.
6. **Response** — `initial_actions(level)` yields the base `ResponseAction`
   list for the resolved level.

The `DomainState` also records hysteresis hold state, level-entry time, the
peak severity in the current episode, the provenance/confidence/validity
derived from the observation, and a human-readable `explanation`.

Domains that were present last refresh but absent now are either *retained as
stale* (when their provider is failed/stale/unavailable) or *removed*. This
prevents transient provider outages from silently deleting a pressured domain.

### 3. Response

Interpretation is closed by deciding what to *recommend*. Memory Pressure
never becomes the actuator. It emits signals:

- **Per-domain responses** — `DomainState::responses` holds the base
  `ResponseAction` list. A cross-tier rule (see CROSS_TIER.md) may strip
  `RequestDemotion` from an accelerator domain when the host tier is
  critically pressured or absent.
- **Aggregate** — a single `aggregate_level` (max-severity) and a weighted
  `aggregate_score` over all domains.
- **Events** — material changes (added/removed domains, level entered/escalated/
  relieved/recovered, generation change, budget/reserve/policy change, provider
  health transitions, reclaim/demotion/backpressure requests) become
  `PressureEvent`s.
- **Queries** — `admit`, `backpressure`, `response_for`, and `explain` answer
  synchronous consumer questions against the *current* snapshot.

### 4. Publication

The new `Snapshot` is published by atomically swapping the
`current_snapshot_` shared pointer. The old snapshot remains valid as long as
any consumer holds a reference, and is retained in a bounded history
(`set_history_limit`, default 64). Single-writer `refresh()` is serialized.

After publication and *after* all internal locks are released, pending events
are pushed to each open `Subscription`.

## Provider / runtime separation

Providers and the runtime are deliberately decoupled:

- **`Provider` is an abstract interface** with three virtuals: `name()`,
  `supported()`, and `sample(now_ms)`. A provider is the only component that
  talks to a foreign API (Windows native APIs, NVIDIA CUDA runtime/driver, the
  filesystem) or a recorded trace.
- **A provider emits observations; it never decides pressure.** It has no access
  to budgets, thresholds, hysteresis, or policy. It cannot invent a level.
- **The runtime owns all pressure semantics.** Interpreted state is wholly
  produced by the runtime from observations plus configuration.

This separation means providers can be added, removed, or swapped without
invalidating any historical `DomainState`, because state is keyed by the
domain id and recomputed from the latest observation plus retained
hysteresis/trend state.

Provider registration is additive and idempotent by provider name
(`register_provider`). Providers are registered under a synthetic id derived
from `"provider::" + name()` using FNV-1a.

Subclasses shipped in-tree:

| Provider | Class | Source |
|----------|-------|--------|
| Windows host memory | `WindowsHostProvider` (`"windows"`) | `windows.cpp` |
| NVIDIA CUDA device memory | `CudaDeviceProvider` (`"cuda.driver"`) | `cuda.cpp` |
| Filesystem / persistent capacity | `StorageProvider` (`"storage"`) | `storage.cpp` |
| Deterministic synthetic | `SyntheticProvider` (`"synthetic"`) | `synthetic.cpp` |
| Recorded trace replay | `TraceProvider` (`"trace"`) | `trace.cpp` |

See PROVIDERS.md for the full provider contract and the extension seams.

## Modules and responsibilities

The module layout maps cleanly onto the three pipeline stages plus the
supporting substrate:

- **Vocabulary** (`types.h`) — the enums and the 128-bit `PressureDomainId`.
  Vendored into every header so the type is stable across the library.
- **Interpretation building blocks** — `budget.h`, `policy.h`,
  `hysteresis.h`, `score.h`, `velocity.h` are pure, deterministic, and free of
  I/O. `core.cpp` implements them.
- **Domain model** — `domain.h` distinguishes raw `DomainObservation` from
  interpreted `DomainState`.
- **Runtime** — `runtime.h`/`runtime.cpp` compose everything and publish
  `Snapshot`s.
- **Response** — `response.h` defines `Backpressure`, `AdmissionHint`,
  `ReliefRequest`, and `DomainResponse`.
- **Events** — `events.h` defines the bounded `Subscription` model.
- **Persistence / interop** — `json.h`/`serialize.h` provide the strict JSON
  substrate for traces, snapshots, policy, and stats.

## Thread-safety model

The runtime is safe for concurrent readers and a single writer. The contract is
documented in `runtime.h`:

- **Current snapshot** is an immutable `std::shared_ptr<const Snapshot>` and is
  swapped atomically by `refresh()`. Readers hold their own reference, so a
  snapshot is never mutated after publication.
- **Policy** is an immutable `std::shared_ptr<const PressurePolicy>`.
  `set_policy` validates first and swaps atomically, publishing the new policy
  only on success.
- **Budgets and domain state** are guarded by internal mutexes
  (`config_mutex_`, and `domain_states_` is touched only inside `refresh()`
  which is serialized by `refresh_mutex_`).
- **`refresh()` is serialized** by `refresh_mutex_` — a single writer. Two
  concurrent `refresh()` calls cannot interleave.
- **Event callbacks are invoked outside all internal locks.** `Subscription::push`
  enqueues under the subscription mutex, then unlocks before calling the
  user callback. This guarantees a callback can never run while an internal
  runtime lock is held (which would permit re-entrancy and lock-order
  inversion).
- **Provider/registry access** is guarded by `registry_mutex_`.

The concurrency test suite exercises parallel readers during refresh, policy
replacement during refresh, and subscription churn during transitions (see
TESTING.md).

### Locking summary

| Guard | Protects |
|-------|----------|
| `refresh_mutex_` | serializes `refresh()`; also serializes hysteresis re-creation in `set_policy` |
| `current_mutex_` | `current_snapshot_`, `history_` |
| `config_mutex_` | `budgets_` |
| `registry_mutex_` | `providers_`, `subscriptions_`, provider-health map |
| `stats_mutex_` | `stats_` counters |
| `Subscription::mutex_` | the subscription's own queue and callback |
