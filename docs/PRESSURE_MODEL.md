# Pressure Model

Memory Pressure models pressure as an explicit, ordered, five-level scale plus a
distinct `no evidence` state. The model is defined in `memory_pressure/types.h`
and the classification logic lives in `memory_pressure/hysteresis.h` (implemented
in `src/core.cpp`).

## Pressure levels

`PressureLevel` is an enum class over `std::uint8_t`:

| Value | Enum | Meaning | Severity rank |
|-------|------|---------|---------------|
| 0 | `Normal` | healthy, plenty of headroom | 0 |
| 1 | `Elevated` | resources getting tight | 1 |
| 2 | `High` | constrained, responses warranted | 2 |
| 3 | `Critical` | near exhaustion, urgent response | 3 |
| 4 | `Exhausted` | no usable headroom | 4 |
| 5 | `Unknown` | no trustworthy observation available | -1 |

## Severity ordering and `Unknown`

Severity is strictly ordered by `severity_rank(PressureLevel)`:

`Normal=0 < Elevated=1 < High=2 < Critical=3 < Exhausted=4`.

`Unknown` is a distinct `no evidence` state that `does not participate in
escalation ordering`:

- `severity_rank(Unknown) == -1`, below every known level.
- `is_known_level(Unknown) == false`; the five ordered levels are the only
  `known` levels.
- When a snapshot has no domains, the aggregate level is `Unknown` (see
  below).
- A domain with no trustworthy observation resolves to `Unknown`, which the
  admission path treats as a defer (it is not confident enough to reject, and
  not safe enough to accept).

`Unknown` is deliberately not placed between `Normal` and `Exhausted`. It is
not a severity at all -- it is an absence of evidence. Comparisons that treat it
as rank -1 ensure it never outranks a real, measured level.

## Classification

Classification is a two-stage process (see THRESHOLDS.md and HYSTERESIS.md):

1. `Raw` threshold-band classification via
   `PressureLevel evaluate_level(double utilization, PressureLevel current,
   const Thresholds& t)`.
2. `Resolved` sticky, debounced level via
   `HysteresisState::update(PressureLevel raw, std::uint64_t now_ms)`.

At `evaluate_level` the utilization is compared against the enter thresholds
when climbing and the exit thresholds when falling, producing hysteresis bands.
The `current` level (`Normal`/`Unknown` on a fresh or reset machine) controls
whether the classifier climbs from the bottom or applies band-stickness.

### Escalation (climbing)

When the domain is at `Normal`/`Unknown`, the raw level is the highest enter
threshold reached: `Exhausted` (>= `exhausted_enter`), then `Critical` (>>=
`critical_enter`), then `High` (>= `high_enter`), then `Elevated` (>>=
`elevated_enter`), else `Normal`. From a non-normal current level the same
enter thresholds are applied; the domain stays within its band unless an exit
threshold is crossed.

### De-escalation (falling)

Falling uses `exit` thresholds:

- `Elevated` -> `Normal` when utilization <= `normal_exit`.
- `High` -> `Elevated` when utilization <= `high_exit`.
- `Critical` -> `High` when utilization <= `critical_exit`.
- `Exhausted` -> `Critical` when utilization <= `exhausted_exit`.

The gap between an exit threshold and the prior level's enter threshold is the
hysteresis band that prevents flapping around a single edge.

## Default policy response actions

The base action list per level is a function of the resolved level
(`initial_actions` in `runtime.cpp`). The default policy also installs
`ResponseRule` entries for `High`/`Critical`/`Exhausted` (see POLICY.md).

| Level | Base actions (runtime) |
|-------|-----------------------|
| `Normal` | `None` |
| `Elevated` | `Warn` |
| `High` | `Warn`, `RequestReclaim` |
| `Critical` | `RequestDemotion`, `RequestReclaim`, `ReduceAdmission`, `Throttle` |
| `Exhausted` | `EmergencyStopGrowth`, `RejectNewWork`, `RequestDemotion`, `RequestReclaim` |
| `Unknown` | `None` |

The cross-tier policy may strip `RequestDemotion` from an accelerator at
`High`/`Critical` when the host tier is critical or absent (CROSS_TIER.md).

## Aggregate level

The snapshot's `aggregate_level` is derived from the maximum severity rank over
all present domains (`runtime.cpp`):

| Max severity rank among domains | Aggregate level |
|-------------------------------|----------------|
| none (no domains) | `Unknown` |
| >= 4 (any `Exhausted`) | `Exhausted` |
| >= 3 (any `Critical`) | `Critical` |
| >= 2 (any `High`) | `High` |
| >= 1 (any `Elevated`) | `Elevated` |
| else | `Normal` |

`aggregate_score` is the role-weighted average of per-domain scores.

## String conversion

Every level has an explicit string form and parser, used by the CLI and by
JSON serialization:

- `to_string(PressureLevel)` returns one of `NORMAL`, `ELEVATED`, `HIGH`,
  `CRITICAL`, `EXHAUSTED`, `UNKNOWN`.
- `pressure_level_from_string(std::string_view)` returns
 `std::optional<PressureLevel>` (`std::nullopt` on failure).

See also: THRESHOLDS.md (band semantics), HYSTERESIS.md (sticky resolution),
CROSS_TIER.md (cross-tier interaction).
