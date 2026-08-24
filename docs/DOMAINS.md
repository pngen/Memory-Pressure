# Domains

A `pressure domain` is a named, independently-governed slice of a resource. In
Memory Pressure a domain is identified by a `PressureDomainId` and described by
a `DomainType`. Domains carry both a raw `DomainObservation` (provider truth)
and an interpreted `DomainState` (runtime conclusion).

## PressureDomainId

`PressureDomainId` is an opaque, vendor-neutral 128-bit identity:

```cpp
struct PressureDomainId {
    std::uint64_t high = 0;
    std::uint64_t low  = 0;
};
```

### Properties

- **Stable and provider-independent.** A domain id is a stable name for a
  pressure domain; it spans providers and processes.
- **Ordered** (`operator<`) and **equality-comparable** (`operator==`, `!=`),
  with a dedicated **hash** (`PressureDomainIdHash`) so it can be a key in
  `std::unordered_map` / `std::unordered_set`.
- `is_zero()` reports the all-zero id (used to signal `no domain`).
- `to_hex()` renders a 32-character lower-case hex big-endian string.
- `from_hex(std::string_view)` parses an exactly-32-char hex string and
  returns `std::optional<PressureDomainId>`; wrong length or a non-hex
  character fails with `std::nullopt`.
- `fnv1a(std::string_view)` builds a deterministic 64-bit seed from text using
  FNV-1a, used to derive stable identities from labels.

### Identity construction rule

`Never construct a domain identity from a transient device ordinal alone.`
Combine a **stable provider identity** with a **stable native resource
identity** (for example the CUDA device UUID, or a fixed resource tag). In-tree
providers follow this rule:

- `WindowsHostProvider` builds ids as
  `PressureDomainId{0x77696E64, fnv1a(\"windows::\" + tag)}`.
- `CudaDeviceProvider` builds ids from the device UUID when available, else
  `PressureDomainId{0x63756461, fnv1a(name + \"#\" + ordinal)}` (a UUID-less
  fallback that is still stable for a given name/ordinal pair).
- `StorageProvider` builds ids as
  `PressureDomainId{0x73746F72, fnv1a(\"storage::\" + path)}`.

## DomainType: economics, not provider

`DomainType` identifies the resource `economics` of a domain, not the provider
that produced it:

| Value | Enum | Meaning |
|-------|------|---------|
| 0 | `HostMemory` | physical host RAM |
| 1 | `PinnedHostMemory` | host-pinned (page-locked) memory |
| 2 | `AcceleratorMemory` | accelerator device memory (GPU, etc.) |
| 3 | `SharedHostMemory` | shared host memory (inter-process / shared) |
| 4 | `FileBackedMemory` | memory backed by a file/mmap |
| 5 | `PersistentStorageCapacity` | persistent storage capacity |
| 6 | `ProcessCommit` | current-process commit |
| 7 | `SystemCommit` | system commit limit/usage |
| 8 | `Custom` | custom, caller-defined |
| 9 | `Unknown` | unknown / unclassified |

The type drives the domain's **reserve and threshold policy** because different
resource economics require different handling (a pinned-memory reserve and an
accelerator reserve are not interchangeable).

### Type to tier role

The runtime maps a `DomainType` to a `TierRole` for cross-tier aggregation
(`role_for` in `runtime.cpp`):

| `DomainType` | `TierRole` |
|--------------|-------------|
| `AcceleratorMemory` | `Accelerator` |
| `HostMemory`, `SystemCommit`, `ProcessCommit` | `Root` |
| `PinnedHostMemory`, `SharedHostMemory` | `Staging` |
| `PersistentStorageCapacity`, `FileBackedMemory` | `Persistent` |
| (other / `Custom`) | `Custom` |

## DomainObservation: raw provider truth

`DomainObservation` is the `raw, provider-reported truth` for one domain. The
runtime never interprets it. Fields that a provider cannot observe use
`std::nullopt` so that `unknown` is never silently conflated with `zero`:

- Identity: `id`, `type`, `provider`, `native_resource_id`.
- Capacity: `total_capacity`, `committed`, `resident`, `available`,
  `unavailable`, optional `reclaimable`, optional `usable_capacity`,
  optional `reserved`.
- Evidence: `confidence`, `provenance`, `validity`.
- Time/context: `observed_at_ms`, `allocation_failures`, `growth_bytes_per_s`,
  `detail`.

Different providers populate different fields. For example the Windows host
provider sets `provenance = WindowsGlobalMemoryStatusEx` and stamps the host
memory observation `Authoritative`; a synthetic provider stamps `provenance =
SyntheticInput` and `confidence = High`. A provider that cannot measure
something leaves the optional empty.

## DomainState: interpreted conclusion

`DomainState` is the runtime's interpretation of an observation, produced by
applying budget, reserves, policy, hysteresis, and trend to the raw data. It is
computed fresh every `refresh` but retains per-domain history across refreshes
in the runtime's internal `DomainRuntimeState`.

### Governed vs ungoverned

A domain is **governed** when a budget with a non-zero `hard_capacity` is
attached to its id; otherwise it is **ungoverned**. The `DomainState` carries
both the budget and a `governed` flag. See BUDGETS.md.

- Governed: `total_capacity` = the governed hard capacity; `usable_capacity`
  = capacity after reserves; `utilization` = `committed / usable_capacity`.
- Ungoverned: `total_capacity` = provider-reported total; `usable_capacity`
  = provider-reported usable (or total); `utilization` is 0 when usable
  capacity is 0.

### Interpretation fields

| Field | Meaning |
|-------|---------|
| `level`, `previous_level` | resolved/previous pressure level |
| `in_hysteresis_hold`, `level_hold_observations` | hysteresis state |
| `score` and component fields | normalized weighted score |
| `stigma` | cumulative penalty (confidence + validity) |
| `trend`, `trend_rate`, `trend_confidence`, `trend_window_samples` | bounded trend estimate |
| `confidence`, `provenance`, `validity` | evidence for the whole state |
| `responses` | recommended `ResponseAction` list |
| `explanation` | structured, human-readable summary |
| `entered_level_at_ms`, `peak_level` | episode timing and peak severity |

`explanation` is built by the runtime as text of the form `<type> is <level>
because governed capacity=K committed=C available=A reserved=R
utilization=U score=S trend=T` (see `build_explanation` in `runtime.cpp`).

## Per-domain economics

The combination of `DomainType` and a governing `Budget` defines a domain's
economics:

- **Host memory (`HostMemory`).** Physical RAM. Governed by a budget that can
  be set below the physical maximum.
- **Pinned host memory (`PinnedHostMemory`).** Page-locked host memory. It has
  no dedicated native measurement in the in-tree providers, so a governing
  budget is a **policy designation**, not an authoritative hardware ceiling
  (see LIMITATIONS.md).
- **Accelerator memory (`AcceleratorMemory`).** One domain per accelerator
  (e.g. per CUDA device).
- **Shared host memory (`SharedHostMemory`).** Inter-process / shared host
  memory.
- **File-backed memory (`FileBackedMemory`) and persistent storage capacity
  (`PersistentStorageCapacity`).** Backed by the filesystem (`StorageProvider`).
- **Process / system commit (`ProcessCommit`, `SystemCommit`).** OS commit
  limits/usage (`WindowsHostProvider`).
- **`Custom` / `Unknown`.** Caller-defined or unclassified; map to a
  `Custom` tier role.

## Snapshot lookup

A snapshot exposes `find_domain(id)` (const and non-const overloads) to locate
an interpreted domain by id; it returns `nullptr` when the domain is absent.
Cross-snapshot structural changes are described by `diff_snapshots(a, b)` (see
EVENTS.md for how those drive events).

See also: BUDGETS.md (governed budgets/reserves), ARCHITECTURE.md (pipeline),
PROVIDERS.md (what each provider observes).
