# Security

Memory Pressure is written to be embedded in a fault-tolerant system, so it treats input and provider data as potential adversaries. Its defenses are: strict parsing, bounded allocation, evidence integrity, and a lock-disciplined concurrency model. The adversarial and failure test suites (`test_adversarial.cpp`, `test_failure.cpp`) exercise these paths.

## Adversarial input handling

Any JSON that reaches the runtime is parsed through the strict `json_parse` boundary. It is designed to reject hostile input before anything downstream acts on it:

- **Malformed input** returns `std::nullopt`: a wrong type, a trailing character, a bad escape, an unpaired surrogate, an unterminated string, or an invalid number fails parsing.
- **Oversized input** is rejected by `JsonParseLimits`: nesting depth capped at 128, total input bytes capped at 4 MiB, and a single string capped at 1 MiB. These bounds stop adversarial input from causing unbounded allocation during parsing.
- **Non-finite numbers** are rejected (`reject_non_finite = true`); `NaN`/`Infinity` literals fail in the parser and via `std::isfinite` checks. A non-finite value is never silently accepted.
- **String hygiene.** Control characters below `0x20` are rejected, and `\uXXXX` escapes are validated (including surrogate-pair handling: an unpaired low surrogate fails).
- **Domain id forgery.** `PressureDomainId::from_hex` requires exactly 32 hex characters, and `snapshot_from_json` rejects a snapshot that repeats a domain id.

### Bounded allocation

Every structure that could grow without a bound is bounded, so no input or event volume can cause unbounded allocation:

- JSON parse depth/bytes/string length are capped (`JsonParseLimits`).
- Snapshot history is bounded (`set_history_limit`, default 64).
- Subscription queues are bounded (default 256 in the runtime); overflow is counted (`dropped()`/`rejected()`), never unbounded.
- Trend windows are bounded (`window_samples`, `max_window_age_ms`).
- Snapshot event summaries are bounded.
- Relief requests are bounded by `max_relief_bytes`.

## Limits and validation

Configuration that could encode an impossible or hazardous policy is rejected before use:

- `validate_budget` rejects a soft capacity above the hard capacity, reserves that exceed capacity, and targets that exceed capacity.
- `validate_policy` rejects non-finite or out-of-range thresholds, non-ascending threshold ordering, negative weights, and a non-positive `max_queued_events` / `min_dwell_observations`.
- A zero `hard_capacity` means ungoverned, not unlimited-and-governed; the validator still checks the rest.

## Provenance / confidence integrity

Memory Pressure never fabricates evidence. Every observation carries a `Provenance` (where the value came from) and a `Confidence` (how reliable it is), and every `Snapshot` carries an aggregate `confidence`/`provenance` derived from the cycle. This matters for security because a consumer can distinguish a measured fact from an assumption:

- A stale or invalid observation is stamped `Validity::Stale`/`Failed` and the domain keeps `Confidence::Low`; the snapshot sets `stale_data`.
- A provider that fails or throws is recorded as `ProviderStatus::Failed`; its previous domains are retained as stale with lowered confidence, but the runtime does not pretend they are fresh.
- The runtime treats `Unknown` provenance/confidence as a real, distinct state, and `PressureLevel::Unknown` does not participate in escalation ordering.

There is no code path that upgrades a low-confidence or inferred value to `Authoritative`. `Confidence::Authoritative` is only set by a provider that reads a native API directly (e.g. `WindowsGlobalMemoryStatusEx`, `WindowsPerformanceInfo`).

## Thread-safety

The concurrency model is designed so a query never races with a write, and a callback never runs under a lock:

- The current snapshot is an immutable `std::shared_ptr<const Snapshot>` swapped atomically by the single-writer `refresh()`; a reader holds its own reference and sees a consistent view.
- Policy is an immutable `shared_ptr<const PressurePolicy>`; `set_policy` validates then swaps, so an invalid policy is never published and readers see only whole policies.
- `refresh()` is serialized by `refresh_mutex_`; budgets are guarded by `config_mutex_`; provider/registry access by `registry_mutex_`; stats by `stats_mutex_`.
- **No callback under lock.** `Subscription::push` enqueues under the subscription mutex, then unlocks before invoking the callback. A callback may therefore call back into the runtime (`admit`, `backpressure`, `set_policy`) without risking deadlock or lock-order inversion.

The `concurrency_*` tests (parallel readers during refresh, policy replacement during refresh, subscription storm during transitions) and the `adversarial_subscription_churn_and_shutdown` / `failure_queue_saturation_accounted` tests guard these properties.

## Provider exception safety

`refresh()` wraps each `Provider::sample(now_ms)` in `try/catch`. A provider that throws is treated as `ProviderStatus::Failed`: the snapshot sets `partial_data`, adds a warning, marks the provider health as failed, and the run continues. A single bad provider cannot take down the refresh cycle.

## Test coverage

See TESTING.md for the adversarial and failure cases.