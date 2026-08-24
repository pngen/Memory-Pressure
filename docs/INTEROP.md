# Interop and boundaries

Memory Pressure is a small, embeddable **sensing and signaling** runtime. It does not allocate, move, or place memory, and it does not implement the larger fabrics that do. This document states the boundary: what Memory Pressure **supplies**, what it **owns**, and how it relates to the surrounding fabrics.

## What Memory Pressure supplies

Memory Pressure produces a vendor-neutral model of pressure plus explicit, bounded signals. Concretely, it exposes:

- **Pressure model.** `PressureLevel`, `PressureScore`, `TrendEstimate`, `DomainState`, `Snapshot`, `DomainObservation` -- the vocabulary and interpretation of pressure.
- **Governance configuration.** `Budget`, `Reserve`, `Thresholds`, `HysteresisConfig`, `PressurePolicy` -- the knobs that tune sensitivity.
- **Synchronous response signals.** `Backpressure`, `AdmissionHint`, `DomainResponse`, `ReliefRequest` (`Reclaim` / `Demotion`) -- what a consumer should do, with a target and an urgency.
- **Asynchronous notifications.** Bounded `Subscription`/`PressureEvent` streams (enter/escalate/relieve/recover, provider health, budget/reserve/policy/generation change, reclaim/demotion/backpressure requests).
- **Persistence/replay.** Strict JSON serialization of policies, snapshots, events, traces, and stats, plus deterministic `TraceProvider` replay.
- **Observation plumbing.** The `Provider` abstraction and in-tree providers (Windows, CUDA, storage, synthetic, trace).

## What Memory Pressure does NOT own

Memory Pressure is deliberately not an actuator. It does not own any of the following; it only emits recommendations about them:

- **Object selection.** It never chooses which allocation to reclaim or demote.
- **Byte movement.** It never moves memory, and has no transfer engine. `RequestDemotion` / `RequestPersistence` / `RequestCompaction` are requests, not operations.
- **Work placement.** It never schedules work and has no scheduler.
- **Allocation/quota enforcement.** It does not deny an allocation itself; `RejectNewWork` / `ReduceAdmission` are recommendations, and `AdmissionHint`/`Backpressure` are advisory.

## Boundaries against the surrounding fabrics

The following fabrics are external systems that Memory Pressure co-exists with. Memory Pressure does not implement any of them; it provides the pressure signal they consume.

| Fabric | Relationship | Memory Pressure provides | Memory Pressure does NOT do |
|--------|--------------|------------------------|------------------------------|
| **Unified Buffer** | unified memory / buffer management | observability of accelerator/host memory and pressure signals | it does not allocate or manage unified buffers |
| **FlashTier** | flash-backed tiering | `RequestPersistence`, `RequestCompaction`, and reserve awareness | it does not manage flash capacity or move pages |
| **Reclaim Fabric** | cooperative reclamation | `RequestReclaim` / `ReliefRequest` (Reclaim) with `target_bytes` and `minimum_useful_relief` | it does not reclaim anything |
| **Transfer Fabric** | data movement/staging | `RequestDemotion`, `TransferStaging` reserve awareness, `Backpressure` | it has no transfer engine and moves no bytes |
| **Topology Fabric** | device/tier topology | `TierRole` used for aggregation and cross-tier demotion feasibility | it does not own or discover topology |
| **Compute Fabric** | scheduling/placement | per-domain accelerator pressure and `Backpressure` | it does not schedule or place work |
| **Admission Fabric (future)** | request admission | `AdmissionHint` (`Accept`/`AcceptWithCaution`/`Defer`/`Reject`) and `Backpressure` | it does not enforce admission; it is advisory |
| **Quota Fabric (future)** | per-tenant quota | budgets/reserves, `ReserveKind` | it does not enforce quotas |
| **Bandwidth Fabric (future)** | transfer bandwidth | aggregate pressure and `Backpressure` | it does not govern bandwidth |

## The actuator boundary in practice

The cleanest way to consume Memory Pressure is to read a `Snapshot` (immutable) and act on `DomainState::responses` / `backpressure()` / `admit()` / `response_for()`. The signals that a fabric would consume are already in the model:

- `ResponseAction::RequestReclaim` -> hand a `ReliefRequest` (Reclaim) to a Reclaim Fabric.
- `ResponseAction::RequestDemotion` -> hand a `ReliefRequest` (Demotion) to a tiering/transfer component (subject to the cross-tier rule, CROSS_TIER.md).
- `ResponseAction::RequestPersistence` / `RequestCompaction` -> hand to a FlashTier/Topology component.
- `ResponseAction::ReduceAdmission` / `RejectNewWork` / `Backpressure` -> hand to an Admission/Quota component.
- `EmergencyStopGrowth` -> a hard stop-growth recommendation.

A consumer decides what to do with a recommendation; Memory Pressure never forces it.

## Scope note

Memory Pressure's own scope is the **pressure model and its signals**. Unified Buffer, FlashTier, Reclaim Fabric, Transfer Fabric, Topology Fabric, Compute Fabric, and the future Admission/Quota/Bandwidth fabrics are not part of the `memory_pressure` library. Where those fabrics exist, Memory Pressure is a provider of pressure knowledge and a consumer of nothing but its own configuration.