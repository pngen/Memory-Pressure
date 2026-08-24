# Memory Pressure core library target.
add_library(memory_pressure STATIC
  json.cpp
  core.cpp
  runtime.cpp
  serialize.cpp
  trace.cpp
  providers/windows.cpp
  providers/cuda.cpp
  providers/storage.cpp
  providers/synthetic.cpp
)

add_library(MemoryPressure::memory_pressure ALIAS memory_pressure)

target_include_directories(memory_pressure
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_features(memory_pressure PUBLIC cxx_std_20)

if(MSVC)
  # PUBLIC so the dependency is exported to downstream consumers of the
  # static library via the installed package config.
  target_link_libraries(memory_pressure PUBLIC psapi)
endif()
