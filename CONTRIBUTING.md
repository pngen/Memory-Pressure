# Contributing to Memory Pressure

Thank you for contributing to Memory Pressure. This project is a vendor-neutral
runtime for observing and governing memory pressure across heterogeneous AI
infrastructure.

## Ground rules

- The library must always compile cleanly with `/W4 /WX` on MSVC (Zero warnings).
- C++20 only. No exceptions for the core hot path unless absolutely necessary.
- Any change that touches the public API must update the relevant documentation.
- Pressure facts must remain bounded, deterministic, and concurrency-safe.
- Never claim a provider or feature is implemented unless it is.
- Do not disable or weaken a failing test to make the suite green.

## Workflow

1. Fork and create a topic branch.
2. Make small, focused changes.
3. Add or update tests in `tests/`.
4. Build in both Debug and Release and run the full test suite.
5. Run the property, adversarial, concurrency, and failure-injection tests.
6. Ensure `ctest` passes from a clean tree.
7. Open a pull request describing the change and its motivation.

## Style

- Namespaces: `memory_pressure`
- Types: PascalCase. Functions and variables: snake_case.
- Enums: SCREAMING_SNAKE_CASE.
- Prefer immutable snapshots and atomic publication for shared state.

## Testing requirements

- No test timeouts. Do not hide a hang behind a watchdog.
- Property tests must log the failing seed.
- Real-hardware tests must use governed budgets below physical capacity.

By contributing you agree to the Apache-2.0 license terms above.
