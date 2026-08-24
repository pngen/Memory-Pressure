# Budgets

A **governed budget** is the capacity ceiling Memory Pressure enforces for a
domain. A budget is `plain data`: the runtime applies policy on top of it. A
domain with no attached budget is **ungoverned** and is observed without a
policy-defined ceiling. The model is defined in `memory_pressure/budget.h`.

## Budget

`Budget` is the governed ceiling for a single pressure domain:

```cpp
struct Budget {
    std::uint64_t hard_capacity = 0;         // governed ceiling
    std::optional<std::uint64_t> soft_capacity; // soft limit (soft pressure)
    std::vector<Reserve> reserves;           // explicit reserves
    std::uint64_t emergency_reserve_bytes = 0;
    std::uint64_t minimum_free_reserve = 0;
    std::uint64_t reclaim_target = 0;
    std::uint64_t demotion_target = 0;
    std::uint64_t admission_headroom = 0;
    std::string owner;                        // configured owner / policy label
    std::uint64_t version = 0;
};
```

### Capacity fields

| Field | Meaning |
|-------|---------|
| `hard_capacity` | the governed ceiling (may be **lower** than the physical maximum). `0` means **ungoverned**. |
| `soft_capacity` | optional soft limit; above this triggers soft pressure. |
| `emergency_reserve_bytes` | convenience amount held aside for emergencies. |
| `minimum_free_reserve` | a hard floor that must remain free. |
| `reclaim_target` | desired bytes to reclaim under stress. |
| `demotion_target` | desired bytes to demote under stress. |
| `admission_headroom` | headroom reserved for new work. |
| `owner` | configured owner / policy label. |
| `version` | budget version, bumped on change. |

A budget can be **lower** than the physical device maximum. That is deliberate:
you may want a domain to begin throttling well before the device is physically
exhausted, leaving headroom for other tenants.

### Derived quantities

`Budget` exposes helper methods used by the runtime to compute capacity after
reserves:

| Method | Definition |
|--------|-----------|
| `reserve_bytes()` | sum of `emergency_reserve_bytes` plus each reserve's bytes. |
| `usable_capacity()` | `hard_capacity - reserve_bytes()`, floored at 0. |
| `available(committed)` | `usable_capacity() - committed`, floored at 0. |

> `usable_capacity` is the capacity actually available for general allocation
> after reserves are set aside. A domain can therefore be **under pressure
> even while the raw device still reports free bytes** -- because those bytes
> are reserved for a purpose (emergency, migration, checkpoint, recovery, ...).

## Reserves

A `Reserve` is a portion of governed capacity that is **not available for
general allocation**. Reserves reduce allocatable headroom, driving pressure
earlier than the raw free-bytes number would suggest.

```cpp
struct Reserve {
    ReserveKind kind = ReserveKind::Custom;
    std::string name;
    std::uint64_t bytes = 0;
};
```

`ReserveKind` enumerates common reserved purposes:

| Enum | Meaning |
|------|---------|
| `Emergency` | emergency capacity |
| `Migration` | migration staging |
| `Checkpoint` | checkpoint capacity |
| `TransferStaging` | transfer staging buffer |
| `Recovery` | recovery capacity |
| `Fragmentation` | fragmentation slack |
| `System` | OS/system headroom |
| `Application` | application-reserved headroom |
| `Custom` | caller-defined |

Each reserve contributes to `reserve_bytes()` and therefore reduces
`usable_capacity()`. The runtime reports the total reserved bytes on the
domain as `DomainState::reserved_bytes`.

## Governed vs ungoverned

- **Governed:** a budget with a non-zero `hard_capacity` is attached to the
  domain id via `PressureRuntime::set_budget(id, budget)`. The runtime derives
  `total_capacity = budget.hard_capacity` and
  `usable_capacity = budget.usable_capacity()`, and computes
  `utilization = committed / usable_capacity`.
- **Ungoverned:** no budget (or a budget with `hard_capacity = 0`). The runtime
  falls back to the provider-reported `total_capacity` / `usable_capacity`
  and computes utilization against the provider ceiling.
  `DomainState::governed` is `false`.

`DomainBudgetAssignment` (a `present` flag plus a `budget`) is the explicit
attach type; the runtime uses it to distinguish `no governance` from an empty
budget.

## Attaching a budget

```cpp
PressureRuntime rt;
Budget b;
b.hard_capacity = 8ULL * 1024 * 1024 * 1024;  // 8 GiB governed ceiling
b.emergency_reserve_bytes = 1ULL * 1024 * 1024 * 1024;
auto err = rt.set_budget(domain_id, b);        // optional<string>
if (err) { /* invalid budget: *err describes the first problem */ }
rt.clear_budget(domain_id);                     // remove governance
```

`set_budget` validates the budget first and returns `std::optional<std::string>`
describing the first problem, or leaves it unchanged on invalidity. The CLI
`budget --domain <id> --hard <bytes> [--emergency <bytes>]` wraps this.

## Validation

`validate_budget(const Budget&)` performs internal-consistency checks and
returns `std::nullopt` when valid, else the first problem in prose:

| Condition | Error |
|-----------|-------|
| `hard_capacity == 0` but a non-zero `soft_capacity` | soft capacity set but no hard capacity |
| `soft_capacity > hard_capacity` | soft capacity exceeds hard capacity |
| reserve total > hard_capacity | reserves exceed hard capacity |
| `emergency_reserve_bytes > hard_capacity` | emergency reserve exceeds hard capacity |
| `minimum_free_reserve > hard_capacity` | minimum free reserve exceeds hard capacity |
| `reclaim_target > hard_capacity` | reclaim target exceeds hard capacity |
| `demotion_target > hard_capacity` | demotion target exceeds hard capacity |
| `admission_headroom > hard_capacity` | admission headroom exceeds hard capacity |

A zero `hard_capacity` means `ungoverned`; the validator still checks the rest
but does not reject the budget for being ungoverned.

`default_budget()` returns a fully-ungoverned budget
(`hard_capacity = 0`) that derives capacity from the provider.

## How a budget drives behavior

Once a domain is governed, the budget participates in the interpretation pipeline:

- **Reserve deficit.** If `committed > usable_capacity`, the reserve-deficit
  component of the score is non-zero.
- **Available capacity.** `available = usable_capacity - committed` (0 when over).
- **Relief targets.** `relief_bytes_for(level, budget)` derives the
  reclaim/demotion target from `reclaim_target` / `demotion_target` or a
  fraction of hard capacity when a target is not set.
- **Admission gating.** `DomainState::budget.admission_headroom` is compared
  with `available` in `admit()` to decide whether a reserve is available.

See also: DOMAINS.md (governed/ungoverned state), POLICY.md (weights that use
budget-derived components), BACKPRESSURE.md (how admission headroom is checked).
