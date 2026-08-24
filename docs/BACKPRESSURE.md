# Backpressure

Backpressure is how Memory Pressure **declares** that a domain is under pressure so a consumer can slow down or defer work before a harder failure. It is a synchronous, race-free signal derived from the **current snapshot**, not a push event. The types are defined in `memory_pressure/response.h`.

## The Backpressure signal

```cpp
struct Backpressure {
    PressureDomainId target;
    PressureLevel severity = PressureLevel::Unknown;
    std::string reason;
    ResponseAction recommended_response = ResponseAction::None;
    std::uint64_t max_new_allocation = 0;
    double admission_reduction = 0.0;      // [0,1] suggested admission-rate reduction
    std::uint64_t defer_duration_ms = 0;
    std::uint64_t generation = 0;
    std::uint64_t issued_at_ms = 0;
    std::uint64_t expiry_ms = 0;         // when this signal is stale
    Confidence confidence = Confidence::Unknown;
    TierRole target_role = TierRole::Root;
};
```

`PressureRuntime::backpressure(id)` fills it from the current snapshot:

| Field | Source |
|-------|--------|
| `severity` | `DomainState::level` |
| `confidence` | `DomainState::confidence` |
| `generation` | `Snapshot::generation` |
| `issued_at_ms` | `Snapshot::timestamp_ms` |
| `expiry_ms` | `domain.timestamp_ms + 10 s` (a stale signal is rejected) |
| `target_role` | `role_for(domain.type)` |
| `reason` | `"<type> <level>"`, e.g. `HOST_MEMORY HIGH` |
| `recommended_response` | the first action in `DomainState::responses` |

The two fields a consumer most often uses are:

- `max_new_allocation` -- the largest single allocation that is reasonably safe right now. It is `available / 4`, or `0` when the domain is governed and has no available bytes.
- `admission_reduction` -- a suggested fractional reduction in admission rate:`0.5` at `High`, `0.8` at `Critical`, `1.0` at `Exhausted`, `0.0` otherwise.

A consumer can query `backpressure(id)` on every new-work decision. It is cheap and lock-free at the point of use because it only reads the already-published snapshot.

## Admission hints

For memory-specific admission questions, use `PressureRuntime::admit(id, bytes)`, which returns an `AdmissionHint`:

```cpp
struct AdmissionHint {
    AdmissionDecision decision = AdmissionDecision::Reject;
    std::string explanation;
    std::uint64_t requested_bytes = 0;
    std::uint64_t safe_bytes = 0;   // max new allocation that is safe right now
    PressureLevel domain_level = PressureLevel::Unknown;
    bool reserve_available = false;
};
```

`admit(id, bytes)` resolves the domain in the current snapshot, records `requested_bytes`, `safe_bytes = DomainState::available`, and `reserve_available = (available >= budget.admission_headroom)`, then applies a per-level decision table:

| Level | Rule | Decision |
|-------|------|----------|
| `Normal` | `bytes <= available` | `Accept` else `Defer` |
| `Elevated` | `bytes <= available/2` | `AcceptWithCaution` else `Defer` |
| `High` | `bytes <= available/4` | `AcceptWithCaution` else `Defer` |
| `Critical` | always | `Reject` |
| `Exhausted` | always | `Reject` |
| `Unknown` | always | `Defer` |

The decision and its reasoning are returned together, so a consumer can log or surface the explanation. When there is no snapshot, `admit` returns `Reject` with the reason `no snapshot available`; when the domain is unknown, it returns `Defer` with `domain unknown`.

## How consumers query synchronously

All query methods (`admit`, `backpressure`, `response_for`, `explain`, `current_snapshot`, `snapshot_by_id`) are synchronous and read an immutable snapshot. The pattern is:

```cpp
auto snap = rt.current_snapshot();          // shared_ptr<const Snapshot>
if (snap) {
    auto bp = rt.backpressure(domain_id);   // reads snap
    if (bp.admission_reduction > 0.0) { ... }
}
```

Because `refresh()` publishes a new snapshot by atomically swapping a `shared_ptr<const Snapshot>`, a query can run concurrently with a refresh and always sees a consistent, immutable view. There is no lock held across the query (see ARCHITECTURE.md, EVENTS.md).

`response_for(id)` is the richer variant: it returns a `DomainResponse` with the action list and an optional `ReliefRequest` (reclaim or demotion) carrying a concrete `target_bytes`, an `urgency`, a `generation`, and a `minimum_useful_relief` threshold. `explain(id)` returns the human-readable explanation string.

## Relationship to events

`BackpressureIssued` events are emitted when a domain's responses include `ReduceAdmission` or `RejectNewWork` (see EVENTS.md). The synchronous `backpressure()`/`admit()` methods are the recommended path for a decision point; the event stream is the recommended path for a watcher or a control plane that reacts asynchronously.

See also: POLICY.md (response rules), CROSS_TIER.md (when demotion is suppressed), EVENTS.md (bounded delivery).