# Testing

Memory Pressure has a self-contained test suite in `tests/`, built as `memory_pressure_tests` and registered with CTest (`add_test(NAME memory_pressure_tests COMMAND memory_pressure_tests)` in `tests/CMakeLists.txt`). It uses a small hand-rolled harness rather than a third-party framework.

## The harness

`tests/test_harness.h` defines the `mp_test` namespace:

```cpp
struct TestCase { std::string name; std::function<void()> fn; };
#define TEST(name) ...      // registers a TestCase
#define EXPECT(cond) ...    // records a check; tracks check/failure counts
int run_all();              // runs every registered case; returns 0 on zero failures
```

`run_all()` prints `[ RUN ]`, then `[ PASS ]` or `[ FAIL ]`, and finally a summary of checks and failures. The exit code is non-zero when any check failed. `tests/main.cpp` invokes `run_all()`.

### No timeout wrappers

The harness runs each test **synchronously and does not wrap tests in a timeout** (there is no `alarm`, no watchdog thread, no timeout helper). This has a deliberate consequence: if a test deadlocks, the run hangs rather than failing. The concurrency tests spawn real `std::thread`s that are joined against a `std::atomic<bool> stop` flag, so a broken lock would manifest as a hang, not a clean failure. When guarding CI, run the suite under an external timeout; do not assume the harness will time out a blocked test.

## Test suites

### `test_core.cpp`

Exercises the pure model: domain-id hex round-trip, budget validation and usable/available math, policy validation, threshold hysteresis (no-flap and fresh-climb), scoring bounds, trend estimation, admission decisions, snapshot generation changes, snapshot diff detection, subscription bounded delivery, backpressure + response, policy/snapshot round-trip, and rejection of duplicate domains in a snapshot.

### `test_providers.cpp`

Real-provider tests that degrade gracefully: `windows_provider_observes_host` (host observations, not a hard assert on a specific machine), `cuda_provider_real_or_unavailable` (a machine without CUDA must not fail the suite), `storage_provider_observes_filesystem`, `synthetic_scenarios_are_deterministic`, `synthetic_multi_gpu_model`, and `synthetic_provider_failure_honest`.

### `test_serialization.cpp`

Covers JSON round-trip and strictness: `observation_json_roundtrip`, `trace_replay_is_deterministic`, `trace_json_roundtrip_preserves_frames`, `json_rejects_nan_and_oversize`, and `json_serialize_string_escaping`.

### `test_property.cpp`

Property-based tests over randomized sequences:

- `property_random_sequences_preserve_invariants` runs thousands of randomized trials and asserts snapshot invariants (score in `[0,1]`, utilization finite and non-negative, `available <= usable_capacity`, known/unknown levels, aggregate score in `[0,1]`).
- `property_no_negative_available_hysteresis_dwell` forces a `High` level and oscillates just below the exit threshold, asserting the resolved level never de-escalates.
- `property_identical_seed_identical_sequence` runs the same seeded scenario twice and asserts the score/utilization sequence is identical.

### Seed reproducibility

The property tests use a hand-rolled, fully deterministic `Rng` (a linear-congruential generator) seeded with a **fixed constant** (e.g. `0xDEC0DE`) rather than a random seed. There is no runtime random-seed logging: the seed is a literal in the source, so any failing trial is reproducible by re-running the same binary. `property_identical_seed_identical_sequence` makes that guarantee explicit by requiring `run(12345) == run(12345)`.

### `test_adversarial.cpp`

Adversarial input handling: `capacity_zero_and_near_max`, `committed_exceeds_capacity`, `json_rejects_bad_observations`, `snapshot_forged_ids`, `event_storm_bounded`, `provider_failure_storm_coherent`, and `subscription_churn_and_shutdown`. These confirm the runtime and JSON layer stay bounded and coherent when fed hostile or degenerate input.

### `test_concurrency.cpp`

Thread-safety under real concurrency: `parallel_readers_during_refresh` (a writer refreshes while four readers query the snapshot, admit, and serialize; no errors), `policy_replacement_during_refresh` (a policy thread swaps policies while a reader reads and a writer refreshes), and `subscription_storm_while_transitioning` (many subscriptions consume events during a sawtooth transition) .

### `test_failure.cpp`

Failure-path behavior: `provider_recovers`, `malformed_observation_handled`, `queue_saturation_accounted`, and `stale_telemetry_not_trusted_forever`.

## Deterministic replay

`TraceProvider` (in `trace.cpp`) replays a `PressureTrace` frame-by-frame; `trace_replay_is_deterministic` and `trace_json_roundtrip_preserves_frames` confirm that a serialized trace replays to the same pressure transitions. Along with `property_identical_seed_identical_sequence`, this is how the model's determinism is guarded: the same input always produces the same output. See DESIGN.md and HYSTERESIS.md.

## Running the suite

Build with tests enabled (the default) and run the target, or use CTest:

```text
cmake -S . -B build -DMEMORY_PRESSURE_BUILD_TESTS=ON
cmake --build build --config Release --target memory_pressure_tests
build/tests/memory_pressure_tests        # direct run
ctest --test-dir build -C Release -R memory_pressure_tests
```

See also: BENCHMARKS.md (methodology), ARCHITECTURE.md (thread-safety the concurrency tests guard).