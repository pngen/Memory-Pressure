# Cross-tier pressure and response policy

Memory Pressure models domains across a hierarchy of tiers. `TierRole` describes a domain's position in that hierarchy (`Root`, `Accelerator`, `Staging`, `Buffer`, `Persistent`, `Custom`), and the runtime uses it for two purposes: aggregate scoring and a **cross-tier response policy** that prevents an infeasible action from being requested.

## Tier roles

The runtime maps a `DomainType` to a `TierRole` (`role_for` in `runtime.cpp`):

| `DomainType` | Semantics | `TierRole` |
|---------------|-----------|-------------|
| `AcceleratorMemory` | accelerator memory | `Accelerator` |
| `HostMemory`, `SystemCommit`, `ProcessCommit` | root/host | `Root` |
| `PinnedHostMemory`, `SharedHostMemory` | staging | `Staging` |
| `PersistentStorageCapacity`, `FileBackedMemory` | persistent | `Persistent` |
| other / `Custom` | caller-defined | `Custom` |

### Host-like domains

The runtime treats a domain as **host-like** when its `DomainType` is `HostMemory`, `PinnedHostMemory`, `SharedHostMemory`, `ProcessCommit`, or `SystemCommit`. These are the tiers into which accelerator memory would be demoted, and the ones considered when deciding whether a demotion is feasible.

## Aggregate pressure

At the snapshot level, `aggregate_level` is the max severity rank across all present domains, and `aggregate_score` is the role-weighted average of per-domain scores:

```text
aggregate_level = max(severity_rank(d.level) for d in domains)
aggregate_score = clamp01( sum(weight(role(d)) * d.score) / sum(weight(role(d))) )
```

The `weight(role)` comes from `policy.aggregate.role_weight` (1.0 when a role is absent). `bottleneck_penalty`, `reserve_exhaustion_penalty`, and `response_feasibility_penalty` are the knobs for weighting a bounding tier (see POLICY.md).

## The cross-tier demotion rule

Demotion moves accelerator memory into host-like memory. If the host tier is itself critically pressured there is no room to demote into, so requesting a demotion would be **infeasible**. The runtime therefore suppresses `RequestDemotion` on accelerator domains in that case.

For each accelerator domain at `High` or `Critical`, the runtime removes `RequestDemotion` from its responses when either condition holds:

- the host tier is **not present at all** (no host-like domain), or
- the host severity is **>= CRITICAL**.

The effect is the scenario in the title: **GPU HIGH + host CRITICAL prevents demotion.** When the accelerator is `High` and the host is at `Critical` (or higher, or absent), the runtime does not request that GPU memory be demoted to the host.

| Accelerator level | Host present / host severity | Result |
|-------------------|------------------------------|--------|
| `High` | host absent | `RequestDemotion` removed |
| `High` | host < `Critical` | `RequestDemotion` kept |
| `High` | host >= `Critical` | `RequestDemotion` removed |
| `Critical` | host < `Critical` | `RequestDemotion` kept |
| `Critical` | host >= `Critical` | `RequestDemotion` removed |
| `Exhausted` | any | `RequestDemotion` kept (no suppression) |

The suppression applies **only** to accelerator domains (`DomainType::AcceleratorMemory`) and **only** at `High`/`Critical`. At `Exhausted` the base action list already includes `EmergencyStopGrowth` and `RejectNewWork`, and demotion is retained -- the domain is past the point of a feasibility filter.

## Why this matters

Without this rule, a heterogeneous system could emit a `RequestDemotion` for GPU memory while the host is in its own `Critical` state. The demotion target would have no headroom, the downstream consumer would attempt an infeasible move, and the host would be driven further into exhaustion. The cross-tier rule makes the signal feasible before it is recommended.

See also: POLICY.md (aggregate config and response rules), PRESSURE_MODEL.md (levels), BACKPRESSURE.md (how signals drive admission).