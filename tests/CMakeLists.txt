# Memory Pressure test suite.
add_executable(memory_pressure_tests
  main.cpp
  test_core.cpp
  test_providers.cpp
  test_serialization.cpp
  test_property.cpp
  test_adversarial.cpp
  test_concurrency.cpp
  test_failure.cpp
)

target_link_libraries(memory_pressure_tests PRIVATE MemoryPressure::memory_pressure)
target_include_directories(memory_pressure_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(memory_pressure_tests PRIVATE cxx_std_20)

include(GoogleTest)
add_test(NAME memory_pressure_tests COMMAND memory_pressure_tests)
