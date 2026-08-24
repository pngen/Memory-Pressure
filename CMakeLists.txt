cmake_minimum_required(VERSION 3.22)

project(MemoryPressure
        VERSION 1.0.0
        DESCRIPTION "Vendor-neutral runtime for detecting, modeling, propagating, and governing memory pressure across heterogeneous AI infrastructure"
        HOMEPAGE_URL "https://github.com/summon-software-labs/memory-pressure"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global project settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(MEMORY_PRESSURE_BUILD_CLI       "Build the memory-pressure command-line tool" ON)
option(MEMORY_PRESSURE_BUILD_TESTS     "Build the test suite" ON)
option(MEMORY_PRESSURE_BUILD_EXAMPLES  "Build the runnable examples" ON)
option(MEMORY_PRESSURE_BUILD_BENCHMARKS "Build the benchmarks" ON)
option(MEMORY_PRESSURE_ENABLE_CUDA     "Enable CUDA provider support (auto-detected)" ON)

# ---------------------------------------------------------------------------
# Compiler configuration.  MSVC is the primary supported toolchain; portability
# flags are provided for Clang / GCC behind the standard compiler checks.
# ---------------------------------------------------------------------------
if(MSVC)
  add_compile_options(/W4 /WX /permissive- /utf-8 /wd4201)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

# ---------------------------------------------------------------------------
# Library target
# ---------------------------------------------------------------------------
add_subdirectory(src)

# ---------------------------------------------------------------------------
# Optional sub-projects
# ---------------------------------------------------------------------------
if(MEMORY_PRESSURE_BUILD_CLI)
  add_subdirectory(cli)
endif()
if(MEMORY_PRESSURE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(MEMORY_PRESSURE_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(MEMORY_PRESSURE_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

# ---------------------------------------------------------------------------
# Installation + package config
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS memory_pressure
        EXPORT MemoryPressureTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/memory_pressure
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT MemoryPressureTargets
        FILE MemoryPressureTargets.cmake
        NAMESPACE MemoryPressure::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MemoryPressure)

configure_package_config_file(
  cmake/MemoryPressureConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/MemoryPressureConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MemoryPressure)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/MemoryPressureConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/MemoryPressureConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/MemoryPressureConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MemoryPressure)

install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DOCDIR} OPTIONAL)
