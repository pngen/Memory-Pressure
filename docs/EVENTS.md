# Events

Events are the bounded notification channel that carries material pressure changes out of the runtime. They are defined in `memory_pressure/events.h`. A consumer may either poll a `Subscription` queue or register a callback. All quantities are bounded, and callbacks are **never invoked while an internal lock is held**.

## PressureEventType

```cpp
enum class PressureEventType : std::uint8_t {
    DomainAdded = 0, DomainRemoved = 1, PressureEntered = 2,
    PressureEscalated = 3, PressureRelieved = 4, PressureRecovered = 5,
    BudgetChanged = 6, ReserveChanged = 7, ProviderStale = 8,
    ProviderFailed = 9, ProviderRecovered = 10, BackpressureIssued = 11,
    ReclaimRequested = 12, DemotionRequested = 13, PolicyChanged = 14,
    GenerationChanged = 15, Custom = 16
};
```

| Type | Emitted when |
|------|--------------|
| `DomainAdded` | a domain first appears in a snapshot. |
| `DomainRemoved` | a domain disappears (and was not retained as stale). |
| `PressureEntered` | a domain enters pressure from `Normal`. |
| `PressureEscalated` | a domain escalates from a non-normal level. |
| `PressureRelieved` | a domain de-escalates but does not reach `Normal`. |
| `PressureRecovered` | a domain returns to `Normal`. |
| `BudgetChanged` | a domain's budget (hard capacity or reserve total) changed. |
| `ReserveChanged` | a domain's reserves changed. |
| `ProviderStale` | a provider was observed stale. |
| `ProviderFailed` | a provider failed or became unavailable. |
| `ProviderRecovered` | (reserved) a provider recovered. |
| `BackpressureIssued` | `ReduceAdmission` or `RejectNewWork` was recommended. |
| `ReclaimRequested` | `RequestReclaim` was recommended. |
| `DemotionRequested` | `RequestDemotion` was recommended. |
| `PolicyChanged` | the policy version changed. |
| `GenerationChanged` | the snapshot generation incremented (a material change). |
| `Custom` | caller-defined. |

Each event is a `PressureEvent`, which carries a monotonically-increasing `id`, the `type`, the policy `generation`, a `timestamp_ms`, an optional `domain` id, a human-readable `detail` string, and a `severity_rank`. `to_string(PressureEventType)` yields the canonical string form used by the CLI and by JSON serialization.

## Subscriptions

`PressureRuntime::subscribe(filter, callback)` returns a `shared_ptr<Subscription>`. A subscription is created with a default `max_queue = 256` and `EventOverflowPolicy::DropOldest` (set inside the runtime's `subscribe`). The filter selects which events flow:

```cpp
struct SubscriptionFilter {
    std::vector<PressureDomainId> domains;
    std::vector<DomainType> domain_types;
    std::vector<PressureLevel> severities;
    std::vector<PressureEventType> event_types;
};
```

`Subscription::matches(e)` filters on the requested `domains`, `event_types`, and `severities` (a `severity` filter matches when `severity_rank(filter_level) == e.severity_rank`). The filter also carries `domain_types` for callers that want type-based filtering; an empty filter matches every event.

A `Subscription` is **non-copyable** and owns a bounded queue behind a mutex. Its API:

| Method | Behavior |
|--------|----------|
| `push(e)` | enqueue (see overflow below); invokes the callback outside the lock. |
| `try_pop(out)` | pop the oldest queued event; false when empty. |
| `pending()` | current queue depth. |
| `set_callback(cb)` | replace the callback. |
| `close()` / `closed()` | mark closed and clear the callback. |
| `clear()` | drop the queued events. |
| `dropped()` / `rejected()` / `delivered()` | counters. |

`PressureRuntime::close_subscription(sub)` lets a consumer close a subscription, which is safe even during shutdown (the destructor also closes all subscriptions).

## Overflow policies

When an event arrives for a subscription whose queue is full, the behavior depends on `EventOverflowPolicy`:

| Policy | Behavior | Counter |
|--------|----------|---------|
| `DropOldest` | pop the oldest event, enqueue the new one. | `dropped()` |
| `DropNewest` | drop the new event. | `dropped()` |
| `Reject` | reject the new event. | `rejected()` |

The runtime's `subscribe` uses `DropOldest`, and `push` returns false when the event does not match the filter, the subscription is closed, or the event is dropped/rejected. Counters make the loss observable rather than silent.

## No-callback-under-lock

`Subscription::push` enqueues the event under the subscription mutex, then **unlocks before** invoking the user callback. This is a hard contract: a callback can never run while an internal runtime or subscription lock is held. It lets a callback call back into the runtime (`admit`, `backpressure`, `set_policy`) without any risk of deadlock or lock-order inversion.

## How events are produced

During `refresh()`, the runtime collects a batch of pending events from the cycle (provider health transitions, domain add/remove, level enter/escalate/relieve/recover, generation change, reclaim/demotion/backpressure requests). After the snapshot is published and all internal locks are released, each event is pushed to every open subscription (see ARCHITECTURE.md). The snapshot also carries a bounded `SnapshotEventSummary` (`count`, `dropped`, and a bounded `classes` list) so a snapshot's size is independent of event volume.

> Note: a minimal `events_probe.h` exists in the tree that declares only `DomainAdded`/`DomainRemoved`. The **full** event model used by the runtime is `events.h`; `events_probe.h` is a small forwarding/probe surface, not the model the runtime emits.

See also: ARCHITECTURE.md (pipeline and publication), BACKPRESSURE.md (synchronous signals vs events), TESTING.md (event-bound and churn tests).