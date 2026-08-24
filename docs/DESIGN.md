# Design

This document records the design principles that shape Memory Pressure. They
are reflected directly in the API: the types exposed in `include/memory_pressure`
and the behavior implemented in `src/`.

## Core question

> When memory becomes scarce, how should the system know and respond `before`
> scarcity becomes failure?

Memory Pressure answers this by separating `knowing` (observation and
interpretation) from `responding` (recommendation). It never decides that a
specific object should be reclaimed, never moves a byte, and never places work.
It declares how much relief is needed and how urgent it is, and leaves the
actuation to the runtimes that own the data.

## Design principles

### 1. Explicit pressure, not inferred pressure

The pressure level of a domain is `explicitly modeled` and carried as data
across the API. Every `DomainState` carries a `PressureLevel`, a
`PressureScore`, a `TrendEstimate`, and -- critically -- a `Provenance` and
`Confidence` for every observation. Nothing is left for a consumer to re-derive
or guess:

- `PressureLevel` is an explicit ordered enum
  (`Normal` < `Elevated` < `High` < `Critical` < `Exhausted`), with a
  distinct `Unknown` for `no trustworthy observation`.
- `Provenance` records `where` a number came from (a native API, a policy
  constant, a synthetic input, an imported snapshot, an inferred metric, ...).
- `Confidence` records `how reliable` the number is
  (`Authoritative`, `High`, `Medium`, `Low`, `Unknown`).

A consumer can always display, log, or reason about the evidence behind a
conclusion rather than trusting an opaque scalar.

### 2. Boundedness

Everything that can grow without a bound is bounded, and the bound is part of
the contract:

- `Snapshot` history is bounded (`set_history_limit`, default 64).
- `Subscription` queues are bounded (`Subscription` `max_queue`, default
  256, with an explicit `EventOverflowPolicy`). Events that do not fit are
  dropped or rejected and counted -- never silently unbounded.
- `TrendEstimator` `window_samples`, default 16, and `max_window_age_ms`,
  default 60 s, bound the trend window.
- `JsonParseLimits` bound JSON depth, byte length, and string length.
- `PressurePolicy::max_relief_bytes` bounds relief requests.
- Snapshot event-class summaries are bounded.

Boundedness is what makes the runtime safe to embed in a fault-tolerant
system: no input, no provider, and no event storm can cause unbounded
allocation.

### 3. Event-driven `and` polling, with a clear split

Memory Pressure supports both consumption models and deliberately separates
them:

- `Polling (synchronous query).` All queries (`admit`, `backpressure`,
  `response_for`, `explain`) operate against the `current immutable
  snapshot`. A consumer reads a `std::shared_ptr<const Snapshot>` and can
  query it repeatedly without locks or races. This is the primary path for
  admission control and backpressure, where a decision must be synchronous and
  race-free.
- `Event-driven (push).` `Subscription` delivers `PressureEvent`s to a
  callback or a polled queue. Subscriptions are bounded, filterable, and
  safe to close at any time. Callbacks are always invoked outside internal
  locks, so a callback may call back into the runtime.

The snapshot is the immutable source of truth for queries; events are a bounded
notification channel. Neither replaces the other.

### 4. Vendor-neutrality

The core library has `no hard dependency on any vendor API`. It is buildable
on CPU-only systems:

- Providers are an abstract interface; vendor code lives only behind a
  `Provider` subclass.
- The CUDA provider loads `nvcuda`/`cudart` (or `libcuda.so`/`libcudart.so`)
  `dynamically`, so a system without NVIDIA libraries still builds and runs --
  the provider simply reports `Unavailable`.
- The Windows provider is guarded by `#ifdef _WIN32` and reports
  `Unavailable` elsewhere.
- All domain identity uses the vendor-neutral `PressureDomainId` (128-bit).

Domain `types` (`DomainType`) describe resource `economics`
(`AcceleratorMemory`, `HostMemory`, `PinnedHostMemory`, `SystemCommit`,
...) rather than a specific provider, so a domain can be fed by different
providers on different systems.

### 5. Separation of data from interpretation

`DomainObservation` is raw provider truth; `DomainState` is the interpreted
conclusion. They are distinct types, and the runtime keeps the two separate so
that providers can change without invalidating history. A `DomainState` is a
pure function of (observation, budget, policy, retained hysteresis/trend state)
and is recomputed every refresh.

### 6. Determinism and replay

The pressure model is deterministic. Given an identical sequence of raw levels
and timestamps, the hysteresis machine produces an identical sequence. The
`SyntheticProvider` and the `TraceProvider` exist so that a recorded
scenario (or a real trace) can be replayed deterministically and the pressure
transitions re-derived. This makes the model testable and reproducible.

### 7. Explicit-vs-inferred

The distinction between `explicit` and `inferred` data is a first-class axis,
carried by `Provenance`:

- `Explicit / measured:` from a native API
  (`WindowsGlobalMemoryStatusEx`, `WindowsPerformanceInfo`,
  `WindowsProcessMemoryInfo`, `CudaDriverApi`, `Filesystem`).
- `Configured:` `ConfiguredPolicy` -- from a budget or policy constant.
- `Imported / synthetic:` `ImportedSnapshot`, `SyntheticInput`.
- `Inferred:` `InferredMetric` -- derived rather than directly measured.

A consumer can therefore tell whether a value is a measured fact or an
assumption, which matters when the value drives an admission or demotion
decision.

### 8. Recommendations, not commands

Every `ResponseAction` is a `signal/recommendation`: `Warn`, `Throttle`,
`Defer`, `RejectNewWork`, `RequestReclaim`, `RequestDemotion`,
`RequestPersistence`, `RequestCompaction`, `ReduceAdmission`,
`ReserveCapacity`, `EmergencyStopGrowth`, `Custom`. The runtime declares
`what` would help and `how urgent` it is, but never performs it. This boundary
is what keeps Memory Pressure embeddable without becoming the authority over
every downstream resource.

### 9. Honest incompleteness

The design refuses to pretend to knowledge it does not have:

- Overflow of a subscription queue is counted and reported, not hidden.
- Provider staleness makes a domain `stale` and lowers confidence rather than
  silently keeping the last good value forever.
- Unknown pressure (`PressureLevel::Unknown`) is a real, distinct state that
  does not participate in escalation ordering.
- Capacity values are serialized as integers and read back exactly only below
  2^53 (~9 PB); larger values are explicitly out of the lossless range (see
  SERIALIZATION.md and LIMITATIONS.md).
