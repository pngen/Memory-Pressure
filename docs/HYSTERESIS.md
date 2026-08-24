# Hysteresis

Hysteresis (debounce) is `mandatory` for a pressure domain: without it a
domain can flap around a threshold. The machine in
`memory_pressure/hysteresis.h` consumes a `raw level` (the threshold-band result
from `evaluate_level`) and returns a `sticky, debounced resolved level`. It is
deterministic for an identical sequence of raw levels and timestamps.

## Configuration

`HysteresisConfig` lives inside the versioned `PressurePolicy`:

| Field | Default | Meaning |
|-------|---------|---------|
| `min_dwell_observations` | 2 | confirmations required before a transition is accepted |
| `min_dwell_duration_ms` | 0.0 | minimum time a level must be held |
| `escalation_delay_ms` | 0.0 | delay before `escalating` |
| `recovery_delay_ms` | 0.0 | delay before `recovering` |
| `immediate_emergency_escalation` | true | bypass delay (not dwell) on `Exhausted` |
| `max_debounce_observations` | 4 | bound on confirmations before acting |

The configuration is used to construct the per-domain `HysteresisState`. On the
default policy `min_dwell_observations = 2`, so a raw crossing must be observed
twice before the resolved level changes.

## The HysteresisState machine

```cpp
class HysteresisState {
public:
    explicit HysteresisState(const HysteresisConfig& cfg);
    PressureLevel update(PressureLevel raw, std::uint64_t now_ms);
    PressureLevel current() const noexcept;   // Unknown before first observation
    bool in_hold() const noexcept;
    std::uint32_t pending_observations() const noexcept;
    void reset() noexcept;
};
```

- `update(raw, now_ms)` feeds a raw level at a time and returns the resolved level.
- `current()` is the currently resolved level (`Unknown` after construction).
- `in_hold()` is true while a transition is pending confirmation.
- `pending_observations()` is the number of confirmations collected for the pending transition.
- `reset()` returns the machine to `Normal`.

## Transition rules

### Confirm current level

If `raw == level_`, the pending transition is cleared and the current level is
confirmed. No change is recorded.

### Emergency escalation

Escalation is measured as `severity_rank(raw) > severity_rank(level_)`; a lowering raw
level is `recovering`. If `raw == Exhausted`, the raw level is escalating, and
`immediate_emergency_escalation` is enabled, the machine adopts the raw
`Exhausted` level immediately -- it does not enter the pending/confirmation path at
all (neither the dwell confirmations nor the delay apply). The config field is
documented as "bypass delay (not dwell) on EXHAUSTED", but the implementation
returns immediately, so both the confirmation count and the delay are skipped on the
emergency path.

### Pending confirmation

For any other candidate transition, the machine starts or accumulates a pending
observation:

- If `pending_ != raw`, start a new pending window: `pending_ = raw`, `pending_obs_ = 1`,
  `pending_since_ms_ = now_ms`.
- Otherwise, increment `pending_obs_`, capped at `max_debounce_observations`.

A transition is committed only when `all three` conditions hold:

| Condition | Test |
|-----------|------|
| observation count | `pending_obs_ >= min_dwell_observations` |
| dwell duration | `now_ms - pending_since_ms_ >= min_dwell_duration_ms` |
| delay | `now_ms - pending_since_ms_ >= wait`, where `wait` is `escalation_delay_ms` when escalating, else `recovery_delay_ms` |

Until then the machine returns the old `level_`. This is what makes transitions
`sticky`.

## Oscillation prevention

Two mechanisms work together to stop a domain from oscillating around a single edge:

1. **Threshold bands.** The enter and exit thresholds are separated
(THRESHOLDS.md), so a value that bounces just below an enter threshold never
leaves its current band.
2. **Hysteresis machine.** Even when a raw crossing does occur, the resolved level
does not change until the raw level is confirmed for the configured dwell and
delay. A single-sample noise spike is absorbed by the confirmation window.

The `property_no_negative_available_hysteresis_dwell` test and the hysteresis
invariant checks in `test_property.cpp` exercise this (see TESTING.md) -- a value in
the `High` band that oscillates just above `high_exit` never de-escalates.

## Determinism

The machine is a pure function of its sequence of `(raw, now_ms)` inputs. The same
sequence always produces the same resolved levels, which is what makes the
`SyntheticProvider` and `TraceProvider` able to reproduce pressure transitions
deterministically (see DESIGN.md).

See also: THRESHOLDS.md (raw classification), PRESSURE_MODEL.md (levels),
POLICY.md (where the hysteresis config lives).