# memory-pressure CLI tool.
add_executable(memory-pressure main.cpp)

target_link_libraries(memory-pressure PRIVATE MemoryPressure::memory_pressure)
target_compile_features(memory-pressure PRIVATE cxx_std_20)

install(TARGETS memory-pressure RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
