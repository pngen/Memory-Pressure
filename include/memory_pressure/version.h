#pragma once
// Memory Pressure 1.0.0 — version metadata.
// Exposed through the library, CLI, and CMake package config.

#define MEMORY_PRESSURE_VERSION_MAJOR 1
#define MEMORY_PRESSURE_VERSION_MINOR 0
#define MEMORY_PRESSURE_VERSION_PATCH 0
#define MEMORY_PRESSURE_VERSION_STRING "1.0.0"

#define MEMORY_PRESSURE_VERSION_OPAQUE 0x00010000u

namespace memory_pressure {

inline constexpr int version_major() noexcept { return MEMORY_PRESSURE_VERSION_MAJOR; }
inline constexpr int version_minor() noexcept { return MEMORY_PRESSURE_VERSION_MINOR; }
inline constexpr int version_patch() noexcept { return MEMORY_PRESSURE_VERSION_PATCH; }
inline constexpr const char* version_string() noexcept { return MEMORY_PRESSURE_VERSION_STRING; }

} // namespace memory_pressure
