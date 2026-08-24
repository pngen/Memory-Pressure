# Thresholds

Thresholds are the utilization boundaries that decide when a domain enters and
leaves each pressure level. They are a part of the versioned `PressurePolicy`
and are defined in `memory_pressure/policy.h`. Threshold comparison uses
**reach on >= enter, leave on <= exit**, so a non-empty band between an exit and
the neighbouring enter threshold produces hysteresis.

## The Thresholds struct

`Thresholds` holds nine fraction values in `[0, 1]`:

| Field | Default | Meaning |
|-------|---------|---------|
| `normal_exit` | 0.40 | leave ELEVATED -> NORMAL |
| `elevated_enter` | 0.60 | enter ELEVATED |
| `elevated_exit` | 0.50 | leave ELEVATED -> NORMAL |
| `high_enter` | 0.85 | enter HIGH |
| `high_exit` | 0.78 | leave HIGH -> ELEVATED |
| `critical_enter` | 0.92 | enter CRITICAL |
| `critical_exit` | 0.86 | leave CRITICAL -> HIGH |
| `exhausted_enter` | 0.985 | enter EXHAUSTED |
| `exhausted_exit` | 0.93 | leave EXHAUSTED -> CRITICAL |

All are normalized utilization fractions. They are plain immutable data inside
the policy; validation occurs before publication.

## Enter / exit semantics

Escalation uses **enter** thresholds; de-escalation uses **exit** thresholds.
The band between an exit threshold and the next enter threshold is **sticky**
(see HYSTERESIS.md for the confirmation stage).

| Transition | Condition |
|-----------|-----------|
| -> ELEVATED | `utilization >= elevated_enter` |
| -> HIGH | `utilization >= high_enter` |
| -> CRITICAL | `utilization >= critical_enter` |
| -> EXHAUSTED | `utilization >= exhausted_enter` |
| ELEVATED -> NORMAL | `utilization <= elevated_exit` |
| HIGH -> ELEVATED | `utilization <= high_exit` |
| CRITICAL -> HIGH | `utilization <= critical_exit` |
| EXHAUSTED -> CRITICAL | `utilization <= exhausted_exit` |

Note the `normal_exit` field (0.40) is the threshold at which an ELEVATED domain
returns to NORMAL in the raw classifier. The runtime also exposes the default
`elevated_exit` (0.50) as the exit for ELEVATED generally; the classifier in
`core.cpp` uses `normal_exit` for `Elevated -> Normal`.

## Band-stickness

For a known non-normal current level, the raw classifier stays within the current
band unless it climbs past an enter threshold or falls past the current level's
exit threshold. Concretely, at `High` the domain remains `High` while
`high_exit < utilization < critical_enter`; it only escalates at
`>= critical_enter` and only de-escalates at `<= high_exit`. This is what
prevents a domain from flapping around a single utilization edge.

## Ascending-ordering validation

`validate_policy` enforces two properties on the threshold set:

1. **Finite and in `[0,1]`** for every field.
2. **Non-decreasing ordering** across the full window chain, which guarantees
   the severity thresholds never descend and hysteresis bands never invert:

```text
normal_exit <= elevated_exit <= elevated_enter
elevated_enter <= high_exit      <= high_enter
high_enter     <= critical_exit  <= critical_enter
critical_enter <= exhausted_exit <= exhausted_enter
```

The validation chain is (`ascending(a, b) = a <= b`):

```text
normal_exit -> elevated_exit -> elevated_enter
elevated_enter -> high_exit -> high_enter -> critical_exit
critical_exit -> critical_enter -> exhausted_exit -> exhausted_enter
```

Violations produce errors such as `thresholds are not in ascending order
(descending severity or inverted band)` or `thresholds must all be finite and
within [0,1]`.

## Why the defaults are conservative

The default set is deliberately conservative and back-loaded:

- `elevated_enter = 0.60` -- the system starts to `warn` well before trouble.
- `high_enter = 0.85`, `critical_enter = 0.92` -- responses and rejections are
  issued only where headroom is genuinely scarce.
- `exhausted_enter = 0.985` -- `Exhausted` is reserved for effectively full
  utilization.

Because `soft_capacity` and `minimum_free_reserve` (BUDGETS.md) subtract from
usable capacity first, a domain can reach these utilization thresholds even
while the raw device still reports free bytes.

See also: HYSTERESIS.md (confirmation), PRESSURE_MODEL.md (level semantics),
POLICY.md (how thresholds live in the policy).