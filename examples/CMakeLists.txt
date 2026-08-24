# Memory Pressure runnable examples.
set(MP_EXAMPLES
  observe_host
  observe_cuda
  custom_budget
  thresholds_hysteresis
  admission_check
  backpressure_subscription
  reclaim_request
  demotion_request
  cross_tier_policy
  synthetic_gpu_spike
  pressure_recovery
  multi_gpu_synthetic
  trace_replay
  policy_serialization
  snapshot_serialization
  integration_consumer
  host_pressure
)

foreach(ex ${MP_EXAMPLES})
  add_executable(${ex} ${ex}.cpp)
  target_link_libraries(${ex} PRIVATE MemoryPressure::memory_pressure)
  target_compile_features(${ex} PRIVATE cxx_std_20)
endforeach()
