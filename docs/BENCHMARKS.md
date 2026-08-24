# Benchmarks

This document describes **how** Memory Pressure is benchmarked and what the numbers do and do not mean. It deliberately does not state throughput or latency figures, because those depend on the host, the provider set, and the configured policy. Measure on your own hardware before drawing conclusions.

## What is measured

The primary end-to-end quantity is the cost of one `PressureRuntime::refresh(now_ms)` over a full synthetic domain set. This is the observation + interpretation + response + publication cycle. It is the number most consumers care about, because it is the cost of turning a poll into an updated pressure picture.

The in-tree benchmark is the CLI `benchmark` command:

```text
memory-pressure benchmark
```

It builds a runtime with 64 synthetic `ACCELERATOR_MEMORY` domains (8 GiB each, staggered phase so they are not synchronized), registers a `SyntheticProvider`, performs warm-up refreshes, then runs 200 timed refreshes and reports the average microseconds per snapshot (`refresh (64 domains): <N> us/snapshot`). The synthetic provider is used so the measurement is deterministic and does not depend on a vendor library being present.

A second, related measurement is available through the CLI `selftest` command, which verifies the default policy validates and that a synthetic gradual-growth scenario actually produces a generation change over 20 refreshes. That is a correctness check, not a benchmark.

## Separating the stages

Reporting a single end-to-end number hides where time goes. When characterizing Memory Pressure, separate the three stages:

| Stage | What to time | Tooling |
|-------|--------------|---------|
| Observation | `Provider::sample(now_ms)` alone | call a single provider's `sample()` in a tight loop (synthetic, windows, cuda, storage, trace). |
| Interpretation / policy | threshold + hysteresis + scoring + aggregation on a fixed observation | call the pure functions (`evaluate_level`, `HysteresisState::update`, `score_domain`, `TrendEstimator::estimate`) directly. |
| End-to-end | one `refresh()` cycle | the CLI `benchmark` command. |

The pure interpretation stage is independent of I/O and can be benchmarked deterministically with no providers. The observation stage is dominated by the native API latency of the specific provider; on a real system that is what dominates a `refresh()`.

## What the in-tree harness does NOT claim

- It does **not** measure a vendor API's throughput or the cost of a real device query in a benchmark that is reproducible everywhere.
- The CLI benchmark uses a synthetic provider, so it isolates the runtime but says nothing about a real Windows/CUDA observation cost.
- There is **no separate `benchmarks/` build tree** in this checkout (no `benchmarks/CMakeLists.txt`). The `MEMORY_PRESSURE_BUILD_BENCHMARKS` CMake option and its `add_subdirectory(benchmarks)` are present in the top-level `CMakeLists.txt`, but the directory is not shipped, so the option should be left `OFF`. The CLI `benchmark` command is the only in-tree benchmark.

## Methodology guidance

When you benchmark Memory Pressure yourself, follow these rules to avoid misleading results:

- **Warm up** the runtime and its providers before timing (`refresh()` allocates per-domain state on first observation).
- **Use enough iterations** for a stable mean, and report the distribution (or at least the median) rather than a single best-case sample.
- **Separate observation from interpretation** when the goal is to explain a regression. The vendor API calls are usually the bottleneck; the model is pure arithmetic.
- **Do not claim impossible throughput.** Do not report a per-snapshot latency that is smaller than the smallest native API call your provider can make, and do not conflate the synthetic-path latency with real-device latency.
- **State the configuration:** domain count, provider set, policy, and whether the run used real or synthetic observations. A 64-domain synthetic refresh tells you about the runtime, not about a 1-GPU production box.

See also: TESTING.md (correctness suite), CUDA.md and WINDOWS.md (what a real observation involves), ARCHITECTURE.md (what a refresh does).