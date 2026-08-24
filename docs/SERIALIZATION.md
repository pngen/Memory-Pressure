# Serialization

Memory Pressure serializes its model to and from **strict, versioned JSON**. The substrate is `memory_pressure/json.h` / `src/json.cpp`; the type-specific converters live in `memory_pressure/serialize.h` / `src/serialize.cpp`. The reader rejects malformed, oversized, non-finite, unknown-schema-version, and semantically-impossible input before anything downstream acts on it.

## The JSON module

`Json` is a bounded, variant-backed value type:

```cpp
class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    Json(); Json(bool); Json(double); Json(std::int64_t); Json(std::uint64_t);
    Json(const char*); Json(std::string); Json(std::string_view);
    static Json array(); static Json object(); static Json null();
    ...
};
```

Numbers are stored as `double`. Object key order is deterministic (a `std::map`) so serialization is stable. Primitive accessors return `std::nullopt` when the accessor does not apply rather than fabricating a value. `as_int64()` / `as_uint64()` fail when the value is not integral or is out of the target range. Object access via `operator[]`/`find`/`has` is provided.

## Strict parser limits

`json_parse(text, limits)` returns `std::optional<Json>` and produces `std::nullopt` on any malformed, oversized, or non-finite input. The bounds exist so adversarial input cannot cause unbounded allocation:

```cpp
struct JsonParseLimits {
    std::size_t max_depth = 128;             // nesting depth
    std::size_t max_bytes = 4 * 1024 * 1024; // total parse input bytes
    std::size_t max_string = 1024 * 1024;    // single string length
    bool reject_non_finite = true;           // reject NaN / Infinity literals
};
```

The parser enforces them directly: an extra trailing character fails (`if (!p.at_end()) return nullopt`), a number that parses to a non-finite double is rejected when `reject_non_finite` is set, and the serializer (`json_serialize`) returns `std::nullopt` if it would emit NaN/infinity. `json_dump` is the convenience variant that always returns a string (emitting `"null"` for a non-finite value).

## Schema versions

Each wire format carries a schema version literal; a reader refuses a document with an unknown version:

| Constant | Value | Used by the read of |
|----------|-------|--------------------|
| `kSnapshotSchemaVersion` | 1 | `snapshot_from_json` |
| `kPolicySchemaVersion` | 1 | `policy_from_json` |
| `kEventSchemaVersion` | 1 | `event_to_json` |
| `kTraceSchemaVersion` | 1 | `trace_from_json` |

These constants are bumped when the wire format changes. For example `policy_from_json` returns `std::nullopt` if the `schema` field is missing or not equal to `kPolicySchemaVersion`, and `snapshot_from_json`/`trace_from_json` do the same for their own schemas. `budget_from_json`, `observation_from_json`, and `domain_state_from_json` validate their structure without a top-level schema key.

## Strict validation

Beyond JSON well-formedness, the semantic layer validates the content:

- **Non-finite numbers.** `write_double` rejects a number that is not finite; the parser rejects NaN/infinity literals.
- **Malformed input.** A non-object, a missing required field, or a wrong-type field returns `std::nullopt`. For example `observation_from_json` requires a valid `id` hex string and integer capacity fields; `policy_from_json` requires `version`, `name`, and the full `thresholds` object.
- **Oversized input.** The parse limits cap input size; the readers then validate the semantics (e.g. `validate_policy`, `validate_budget`) and reject impossible values.
- **Duplicate domain ids.** `snapshot_from_json` keeps a `seen` list and returns `std::nullopt` if two domains share an id. `PressureDomainId::from_hex` requires exactly 32 hex characters, so a forged/wrong-length id fails. The `adversarial_snapshot_forged_ids` test exercises this.

Every `..._from_json` that can produce an invalid semantic object ends by running its validator (`validate_policy`, `validate_budget`) and returning `std::nullopt` on failure.

## What can be (de)serialized

The `serialize.h` surface is symmetric for the model types that need persistence:

| Type | to Json | from Json |
|------|---------|-----------|
| `PressurePolicy` | `policy_to_json` | `policy_from_json` |
| `Budget` | `budget_to_json` | `budget_from_json` |
| `DomainObservation` | `observation_to_json` | `observation_from_json` |
| `DomainState` | `domain_state_to_json` | (via snapshot) |
| `Snapshot` | `snapshot_to_json` | `snapshot_from_json` |
| `PressureEvent` | `event_to_json` | (write-only) |
| `RuntimeStats` | `stats_to_json` | (write-only) |
| `PressureTrace` | `trace_to_json` | `trace_from_json` |

These are used by the CLI `status --json` / `snapshot --json` / `policy` / `events --json` / `stats --json` outputs and by the trace replay / policy & snapshot serialization examples.

## Round-trip and the <2^53 capacity bound

Capacity fields are stored as `Json(std::int64_t)` and read back with `as_uint64()`. Because `Json` stores numbers as `double`, an integer is preserved exactly only while it is below 2^53 (9,007,199,254,740,992). `Json::as_uint64` rejects values above that bound explicitly (`if (v > 9007199254740992.0) return nullopt`).

The practical consequence: **capacities through ~9 PiB round-trip losslessly**. That exceeds any realistic device or host capacity today, but the bound is real and documented. Cache, snapshot, and trace round-trips are exact below it; value above it are **not** representable losslessly and are rejected by the reader rather than silently truncated. See LIMITATIONS.md.

## Example

```cpp
auto j = snapshot_to_json(*snap);              // Json
auto s = json_serialize(j);                    // optional<string>
if (s) { /* write *s, e.g. to stdout or a file */ }

auto parsed = json_parse(*s);                  // optional<Json>
if (parsed) {
    auto back = snapshot_from_json(*parsed);     // optional<Snapshot>
}
```

The serialization tests cover observation round-trip, JSON NaN/oversize rejection, string escaping, snapshot rejection of duplicate domains, policy/snapshot round-trip, and trace JSON round-trip (see TESTING.md).