# Policy

The **pressure policy** is the versioned configuration that drives the whole interpretation stage: thresholds, hysteresis, scoring weights, aggregation, provider freshness, and response rules. It is defined in `memory_pressure/policy.h` as plain immutable data, validated before publication, and referenced by version on every snapshot.

## PressurePolicy

```cpp
struct PressurePolicy {
    std::uint64_t version = 1;
    std::string name = "default";
    Thresholds thresholds;
    HysteresisConfig hysteresis;
    ScoreWeights weights;
    AggregateConfig aggregate;
    FreshnessConfig freshness;
    std::vector<ResponseRule> response_rules;
    std::uint64_t max_queued_events = 4096;
    std::uint64_t max_relief_bytes = (std::numeric_limits<std::uint64_t>::max)();
};
```

| Field | Default | Meaning |
|-------|---------|---------|
| `version` | 1 | policy version (referenced by snapshots). |
| `name` | `default` | human-readable name. |
| `thresholds` | see THRESHOLDS.md | utilization level boundaries. |
| `hysteresis` | see HYSTERESIS.md | debounce configuration. |
| `weights` | see below | normalized score weights. |
| `aggregate` | see below | cross-domain aggregation configuration. |
| `freshness` | see below | provider freshness bounds. |
| `response_rules` | High / Critical / Exhausted | level-to-action rules. |
| `max_queued_events` | 4096 | global event bound. |
| `max_relief_bytes` | `uint64_t max` | bound on relief requests. |

`revision()` produces a stable identifier for the policy, and `operator==` compares `version`, `name`, and the enter thresholds (`elevated_enter`, `high_enter`, `critical_enter`, `exhausted_enter`).

## Scoring weights

`ScoreWeights` controls the weighted composite score. All values must be finite and non-negative (enforced by `validate_policy`):

| Field | Default | Component normalized in `[0,1]` |
|-------|---------|------------------------------------|
| `utilization` | 1.0 | committed / usable |
| `free_deficit` | 1.0 | 1 - free_fraction |
| `reserve_deficit` | 1.0 | reserve deficit fraction |
| `growth_rate` | 0.5 | positive trend confidence |
| `allocation_failure` | 2.0 | min(1, failures / 5) |
| `fragmentation` | 0.25 | fragmentation (reserved for future use) |
| `reclaimable_deficit` | 0.5 | reclaimable deficit |
| `confidence_penalty` | 0.5 | confidence-derived penalty |
| `stale_penalty` | 0.75 | validity-derived penalty |

### How the score is computed

`score_domain(...)` (implemented in `core.cpp`) normalizes each component to `[0,1]`, applies the weights as a weighted average, and clamps the result to `[0,1]`:

```text
value = clamp01( sum(weight_i * component_i) / sum(weight_i) )
```

Confidence and validity penalties are feed into the same weighted-mean formula. The penalty schedules are:

| `Confidence` | penalty |
|---------------|---------|
| `Authoritative` | 0.0 |
| `High` | 0.15 |
| `Medium` | 0.40 |
| `Low` | 0.65 |
| `Unknown` | 1.0 |

| `Validity` | penalty |
|-------------|---------|
| `Valid` | 0.0 |
| `Stale` | 0.85 |
| `Partial` | 0.35 |
| `Failed` | 1.0 |
| `Unavailable` | 1.0 |

Per-component values are exposed on `PressureScore` (and on `DomainState`) so a consumer can inspect which component drove the score.

## Aggregation

`AggregateConfig` controls how per-domain results combine into the snapshot aggregate:

| Field | Default | Meaning |
|-------|---------|---------|
| `consider_hierarchy` | true | consider the tier hierarchy. |
| `role_weight` | (empty) | role multiplier map over `TierRole`; default 1.0 for absent roles. |
| `bottleneck_penalty` | 1.0 | extra weight on the max-criticality domain. |
| `reserve_exhaustion_penalty` | 1.25 | extra weight if a critical domain is reserve-exhausted. |
| `response_feasibility_penalty` | 0.75 | discount if a response is infeasible. |

`aggregate_level` is the max severity rank over present domains; `aggregate_score` is the role-weighted average of per-domain scores (see PRESSURE_MODEL.md). The role-weight map is serialized as a JSON object keyed by `TierRole` name (e.g. `ROOT`, `ACCELERATOR`, `STAGING`, `PERSISTENT`, `BUFFER`).

## Provider freshness

`FreshnessConfig` bounds how long a provider's data is considered fresh:

| Field | Default | Meaning |
|-------|---------|---------|
| `default_max_age_us` | 10,000,000 (10 s) | default freshness bound in microseconds. |
| `per_provider_max_age_us` | (empty) | per-provider name -> bound overrides. |

A provider that exceeds its freshness bound is treated as `stale`; the snapshot sets `stale_data` and the runtime retains previously observed domains as stale rather than deleting them (see ARCHITECTURE.md).

## Response rules

A `ResponseRule` maps a `PressureLevel` to a list of `ResponseAction`s. The default policy installs three rules:

| Level | Actions |
|-------|---------|
| `High` | `Warn`, `RequestReclaim` |
| `Critical` | `RequestDemotion`, `RequestReclaim`, `ReduceAdmission`, `Throttle` |
| `Exhausted` | `EmergencyStopGrowth`, `RequestReclaim`, `RequestDemotion`, `RejectNewWork` |

These are recommendations. The runtime also produces a base action list per level independently (see PRESSURE_MODEL.md), and the cross-tier policy can strip `RequestDemotion` (CROSS_TIER.md). `max_relief_bytes` caps the relief the runtime will request. `max_queued_events` bounds queued events.

## Atomic replacement

`PressureRuntime::set_policy(PressurePolicy)` applies a policy **atomically**:

1. `validate_policy(p)` runs first. On failure it returns the problem description and leaves the previous policy in place -- an invalid policy is never published.
2. On success, the per-domain `HysteresisState` instances are recreated from the new hysteresis config (under `refresh_mutex_`).
3. The new policy is swapped into `policy_` as `shared_ptr<const PressurePolicy>`.
4. `stats_.policy_changes` is bumped.

Because the policy is an immutable shared pointer, concurrent readers see either the old or the new policy, never a partially-constructed one. Change is detected via `diff_snapshots` (`policy_changed` when `policy_version` differs).

## Validation

`validate_policy(const PressurePolicy&)` returns `std::nullopt` when valid, else the first problem. It enforces:

- All `Thresholds` values are finite and in `[0,1]`.
- The `Thresholds` ordering is non-decreasing (THRESHOLDS.md).
- All `ScoreWeights` are finite and non-negative.
- `max_queued_events` > 0.
- `min_dwell_observations` > 0.

The default policy (`default_policy()`) is `version 1`, `name = "default"`, with the three response rules above and `max_relief_bytes = uint64_t max`.