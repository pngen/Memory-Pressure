#pragma once
// Core shared types for Memory Pressure.
//
// These types are vendored into every other header. They define the
// enumerated vocabulary of the pressure model and the opaque 128-bit
// identity used to name pressure domains across providers and processes.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <functional>

namespace memory_pressure {

// ---------------------------------------------------------------------------
// Pressure levels.  Severity is ordered; Unknown is a distinct "no evidence"
// state that does not participate in escalation ordering.
// ---------------------------------------------------------------------------
enum class PressureLevel : std::uint8_t {
    Normal    = 0,   // healthy, plenty of headroom
    Elevated  = 1,   // resources getting tight
    High      = 2,   // constrained, responses warranted
    Critical  = 3,   // near exhaustion, urgent response
    Exhausted = 4,   // no usable headroom
    Unknown   = 5    // no trustworthy observation available
};

// ---------------------------------------------------------------------------
// Domain types.  These identify the resource *economics* of a domain, not the
// provider.  A domain's economics dictate its reserve and threshold policy.
// ---------------------------------------------------------------------------
enum class DomainType : std::uint8_t {
    HostMemory             = 0,
    PinnedHostMemory       = 1,
    AcceleratorMemory      = 2,
    SharedHostMemory       = 3,
    FileBackedMemory       = 4,
    PersistentStorageCapacity = 5,
    ProcessCommit          = 6,
    SystemCommit           = 7,
    Custom                 = 8,
    Unknown                = 9
};

// Confidence in a particular observation / conclusion.
enum class Confidence : std::uint8_t {
    Authoritative = 0,  // directly measured by a native API
    High          = 1,  // highly reliable, possibly derived with strong basis
    Medium        = 2,  // derived, plausible
    Low           = 3,  // inferred / weak
    Unknown       = 4
};

// Provenance: where a value came from.  Distinguishes directly-observed data
// from configured, imported, synthetic, or merely inferred data.
enum class Provenance : std::uint8_t {
    WindowsGlobalMemoryStatusEx = 0,
    WindowsPerformanceInfo      = 1,
    WindowsProcessMemoryInfo    = 2,
    WindowsProcessQuery         = 3,
    CudaDriverApi               = 4,
    Filesystem                  = 5,
    RuntimeRegistration         = 6,
    ConfiguredPolicy            = 7,
    SyntheticInput              = 8,
    ImportedSnapshot            = 9,
    InferredMetric              = 10,
    AllocationProbe             = 11,
    CudaRuntimeApi              = 12,
    Unknown                     = 13
};

// Validity of a single observation.
enum class Validity : std::uint8_t {
    Valid      = 0,
    Stale      = 1,
    Partial    = 2,
    Failed     = 3,
    Unavailable = 4
};

// Health of a provider.
enum class ProviderStatus : std::uint8_t {
    Healthy  = 0,
    Stale    = 1,
    Failed   = 2,
    Unavailable = 3,
    Partial  = 4
};

// Direction of a bounded trend estimate.
enum class TrendDirection : std::uint8_t {
    Flat    = 0,
    Rising  = 1,
    Falling = 2,
    Unknown = 3
};

// Explicit pressure responses.  These are *signals/recommendations* to other
// runtimes; Memory Pressure never directly becomes every downstream actuator.
enum class ResponseAction : std::uint8_t {
    None                 = 0,
    Warn                 = 1,
    Throttle             = 2,
    Defer                = 3,
    RejectNewWork        = 4,
    RequestReclaim       = 5,
    RequestDemotion      = 6,
    RequestPersistence   = 7,
    RequestCompaction    = 8,
    ReduceAdmission      = 9,
    ReserveCapacity      = 10,
    EmergencyStopGrowth  = 11,
    Custom               = 12
};

// Admission hint outcome for a specific request.
enum class AdmissionDecision : std::uint8_t {
    Accept            = 0,
    AcceptWithCaution = 1,
    Defer             = 2,
    Reject            = 3
};

// Aggregate pressure modeling role within a tier hierarchy.
enum class TierRole : std::uint8_t {
    Root       = 0,
    Accelerator = 1,
    Staging    = 2,
    Buffer     = 3,
    Persistent = 4,
    Custom     = 5
};

// ---------------------------------------------------------------------------
// Opaque 128-bit pressure-domain identity.
//
// Domain identity is a stable, provider-independent name for a pressure
// domain.  Never construct identity from a transient device ordinal alone;
// combine a stable provider identity with a stable native resource identity.
// ---------------------------------------------------------------------------
struct PressureDomainId {
    std::uint64_t high = 0;
    std::uint64_t low  = 0;

    constexpr bool operator==(const PressureDomainId& o) const noexcept {
        return high == o.high && low == o.low;
    }
    constexpr bool operator!=(const PressureDomainId& o) const noexcept {
        return !(*this == o);
    }
    constexpr bool operator<(const PressureDomainId& o) const noexcept {
        return std::tie(high, low) < std::tie(o.high, o.low);
    }

    constexpr bool is_zero() const noexcept { return high == 0 && low == 0; }

    // Report the domain id as a 32-char lower-case hex big-endian string.
    std::string to_hex() const {
        char buf[33];
        std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                      static_cast<unsigned long long>(high),
                      static_cast<unsigned long long>(low));
        return std::string(buf);
    }

    // Parse a 32-char hex string produced by to_hex().  Failed returns nullopt.
    static std::optional<PressureDomainId> from_hex(std::string_view s) noexcept {
        auto parse16 = [](std::string_view chunk) -> std::optional<std::uint64_t> {
            if (chunk.size() != 16) return std::nullopt;
            std::uint64_t v = 0;
            for (char c : chunk) {
                std::uint64_t nibble = 0;
                if (c >= '0' && c <= '9') nibble = static_cast<std::uint64_t>(c - '0');
                else if (c >= 'a' && c <= 'f') nibble = static_cast<std::uint64_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') nibble = static_cast<std::uint64_t>(c - 'A' + 10);
                else return std::nullopt;
                v = (v << 4) | nibble;
            }
            return v;
        };
        if (s.size() != 32) return std::nullopt;
        auto hi = parse16(s.substr(0, 16));
        auto lo = parse16(s.substr(16, 16));
        if (!hi || !lo) return std::nullopt;
        return PressureDomainId{*hi, *lo};
    }

    // Build a deterministic seed from text using FNV-1a (64-bit).  Useful for
    // generating stable synthetic identities from string labels.
    static std::uint64_t fnv1a(std::string_view key) noexcept {
        std::uint64_t h = 14695981039346656037ULL;
        for (char c : key) {
            h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
            h *= 1099511628211ULL;
        }
        return h;
    }
};

// Hash support so PressureDomainId can be a std::unordered_map/unordered_set key.
struct PressureDomainIdHash {
    std::size_t operator()(const PressureDomainId& id) const noexcept {
        // Mix both halves with a strong finalizer.
        std::uint64_t h = id.high ^ (id.low + 0x9e3779b97f4a7c15ULL + (id.high << 6) + (id.high >> 2));
        h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33;
        return static_cast<std::size_t>(h);
    }
};

// ---------------------------------------------------------------------------
// Enum <-> string helpers (finite, explicit; used by serialization + CLI).
// ---------------------------------------------------------------------------
inline const char* to_string(PressureLevel v) noexcept {
    switch (v) {
        case PressureLevel::Normal:    return "NORMAL";
        case PressureLevel::Elevated:  return "ELEVATED";
        case PressureLevel::High:      return "HIGH";
        case PressureLevel::Critical:  return "CRITICAL";
        case PressureLevel::Exhausted: return "EXHAUSTED";
        case PressureLevel::Unknown:   return "UNKNOWN";
    }
    return "UNKNOWN";
}

inline const char* to_string(DomainType v) noexcept {
    switch (v) {
        case DomainType::HostMemory:              return "HOST_MEMORY";
        case DomainType::PinnedHostMemory:        return "PINNED_HOST_MEMORY";
        case DomainType::AcceleratorMemory:       return "ACCELERATOR_MEMORY";
        case DomainType::SharedHostMemory:        return "SHARED_HOST_MEMORY";
        case DomainType::FileBackedMemory:        return "FILE_BACKED_MEMORY";
        case DomainType::PersistentStorageCapacity: return "PERSISTENT_STORAGE_CAPACITY";
        case DomainType::ProcessCommit:           return "PROCESS_COMMIT";
        case DomainType::SystemCommit:            return "SYSTEM_COMMIT";
        case DomainType::Custom:                  return "CUSTOM";
        case DomainType::Unknown:                 return "UNKNOWN";
    }
    return "UNKNOWN";
}

inline const char* to_string(Confidence v) noexcept {
    switch (v) {
        case Confidence::Authoritative: return "AUTHORITATIVE";
        case Confidence::High:          return "HIGH";
        case Confidence::Medium:        return "MEDIUM";
        case Confidence::Low:           return "LOW";
        case Confidence::Unknown:       return "UNKNOWN";
    }
    return "UNKNOWN";
}

inline const char* to_string(Provenance v) noexcept {
    switch (v) {
        case Provenance::WindowsGlobalMemoryStatusEx: return "WINDOWS_GLOBAL_MEMORY_STATUS_EX";
        case Provenance::WindowsPerformanceInfo:      return "WINDOWS_PERFORMANCE_INFO";
        case Provenance::WindowsProcessMemoryInfo:    return "WINDOWS_PROCESS_MEMORY_INFO";
        case Provenance::WindowsProcessQuery:         return "WINDOWS_PROCESS_QUERY";
        case Provenance::CudaDriverApi:               return "CUDA_DRIVER_API";
        case Provenance::Filesystem:                  return "FILESYSTEM";
        case Provenance::RuntimeRegistration:         return "RUNTIME_REGISTRATION";
        case Provenance::ConfiguredPolicy:            return "CONFIGURED_POLICY";
        case Provenance::SyntheticInput:              return "SYNTHETIC_INPUT";
        case Provenance::ImportedSnapshot:            return "IMPORTED_SNAPSHOT";
        case Provenance::InferredMetric:              return "INFERRED_METRIC";
        case Provenance::AllocationProbe:             return "ALLOCATION_PROBE";
        case Provenance::CudaRuntimeApi:              return "CUDA_RUNTIME_API";
        case Provenance::Unknown:                     return "UNKNOWN";
    }
    return "UNKNOWN";
}

inline const char* to_string(Validity v) noexcept {
    switch (v) {
        case Validity::Valid:       return "VALID";
        case Validity::Stale:       return "STALE";
        case Validity::Partial:     return "PARTIAL";
        case Validity::Failed:      return "FAILED";
        case Validity::Unavailable: return "UNAVAILABLE";
    }
    return "UNAVAILABLE";
}

inline const char* to_string(ProviderStatus v) noexcept {
    switch (v) {
        case ProviderStatus::Healthy:     return "HEALTHY";
        case ProviderStatus::Stale:       return "STALE";
        case ProviderStatus::Failed:      return "FAILED";
        case ProviderStatus::Unavailable: return "UNAVAILABLE";
    }
    return "UNAVAILABLE";
}

inline const char* to_string(TrendDirection v) noexcept {
    switch (v) {
        case TrendDirection::Flat:    return "FLAT";
        case TrendDirection::Rising:  return "RISING";
        case TrendDirection::Falling: return "FALLING";
        case TrendDirection::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

inline const char* to_string(ResponseAction v) noexcept {
    switch (v) {
        case ResponseAction::None:                return "NONE";
        case ResponseAction::Warn:                return "WARN";
        case ResponseAction::Throttle:            return "THROTTLE";
        case ResponseAction::Defer:               return "DEFER";
        case ResponseAction::RejectNewWork:       return "REJECT_NEW_WORK";
        case ResponseAction::RequestReclaim:      return "REQUEST_RECLAIM";
        case ResponseAction::RequestDemotion:     return "REQUEST_DEMOTION";
        case ResponseAction::RequestPersistence:  return "REQUEST_PERSISTENCE";
        case ResponseAction::RequestCompaction:   return "REQUEST_COMPACTION";
        case ResponseAction::ReduceAdmission:     return "REDUCE_ADMISSION";
        case ResponseAction::ReserveCapacity:     return "RESERVE_CAPACITY";
        case ResponseAction::EmergencyStopGrowth: return "EMERGENCY_STOP_GROWTH";
        case ResponseAction::Custom:              return "CUSTOM";
    }
    return "UNKNOWN";
}

inline const char* to_string(AdmissionDecision v) noexcept {
    switch (v) {
        case AdmissionDecision::Accept:            return "ACCEPT";
        case AdmissionDecision::AcceptWithCaution: return "ACCEPT_WITH_CAUTION";
        case AdmissionDecision::Defer:             return "DEFER";
        case AdmissionDecision::Reject:            return "REJECT";
    }
    return "UNKNOWN";
}

inline const char* to_string(TierRole v) noexcept {
    switch (v) {
        case TierRole::Root:        return "ROOT";
        case TierRole::Accelerator: return "ACCELERATOR";
        case TierRole::Staging:     return "STAGING";
        case TierRole::Buffer:      return "BUFFER";
        case TierRole::Persistent:  return "PERSISTENT";
        case TierRole::Custom:      return "CUSTOM";
    }
    return "CUSTOM";
}

// ---------------------------------------------------------------------------
// String -> enum (used by CLI and serialization).  Returns nullopt on failure.
// ---------------------------------------------------------------------------
inline std::optional<PressureLevel> pressure_level_from_string(std::string_view s) noexcept {
    if (s == "NORMAL")    return PressureLevel::Normal;
    if (s == "ELEVATED")  return PressureLevel::Elevated;
    if (s == "HIGH")      return PressureLevel::High;
    if (s == "CRITICAL")  return PressureLevel::Critical;
    if (s == "EXHAUSTED") return PressureLevel::Exhausted;
    if (s == "UNKNOWN")   return PressureLevel::Unknown;
    return std::nullopt;
}

inline std::optional<DomainType> domain_type_from_string(std::string_view s) noexcept {
    if (s == "HOST_MEMORY")                 return DomainType::HostMemory;
    if (s == "PINNED_HOST_MEMORY")          return DomainType::PinnedHostMemory;
    if (s == "ACCELERATOR_MEMORY")          return DomainType::AcceleratorMemory;
    if (s == "SHARED_HOST_MEMORY")          return DomainType::SharedHostMemory;
    if (s == "FILE_BACKED_MEMORY")          return DomainType::FileBackedMemory;
    if (s == "PERSISTENT_STORAGE_CAPACITY") return DomainType::PersistentStorageCapacity;
    if (s == "PROCESS_COMMIT")              return DomainType::ProcessCommit;
    if (s == "SYSTEM_COMMIT")               return DomainType::SystemCommit;
    if (s == "CUSTOM")                      return DomainType::Custom;
    if (s == "UNKNOWN")                     return DomainType::Unknown;
    return std::nullopt;
}

inline std::optional<ResponseAction> response_action_from_string(std::string_view s) noexcept {
    if (s == "NONE")                  return ResponseAction::None;
    if (s == "WARN")                  return ResponseAction::Warn;
    if (s == "THROTTLE")              return ResponseAction::Throttle;
    if (s == "DEFER")                 return ResponseAction::Defer;
    if (s == "REJECT_NEW_WORK")       return ResponseAction::RejectNewWork;
    if (s == "REQUEST_RECLAIM")       return ResponseAction::RequestReclaim;
    if (s == "REQUEST_DEMOTION")      return ResponseAction::RequestDemotion;
    if (s == "REQUEST_PERSISTENCE")   return ResponseAction::RequestPersistence;
    if (s == "REQUEST_COMPACTION")    return ResponseAction::RequestCompaction;
    if (s == "REDUCE_ADMISSION")      return ResponseAction::ReduceAdmission;
    if (s == "RESERVE_CAPACITY")      return ResponseAction::ReserveCapacity;
    if (s == "EMERGENCY_STOP_GROWTH") return ResponseAction::EmergencyStopGrowth;
    if (s == "CUSTOM")                return ResponseAction::Custom;
    return std::nullopt;
}

inline std::optional<Confidence> confidence_from_string(std::string_view s) noexcept {
    if (s == "AUTHORITATIVE") return Confidence::Authoritative;
    if (s == "HIGH")          return Confidence::High;
    if (s == "MEDIUM")        return Confidence::Medium;
    if (s == "LOW")           return Confidence::Low;
    if (s == "UNKNOWN")       return Confidence::Unknown;
    return std::nullopt;
}

inline std::optional<AdmissionDecision> admission_from_string(std::string_view s) noexcept {
    if (s == "ACCEPT")              return AdmissionDecision::Accept;
    if (s == "ACCEPT_WITH_CAUTION") return AdmissionDecision::AcceptWithCaution;
    if (s == "DEFER")               return AdmissionDecision::Defer;
    if (s == "REJECT")              return AdmissionDecision::Reject;
    return std::nullopt;
}

inline std::optional<TierRole> tier_role_from_string(std::string_view s) noexcept {
    if (s == "ROOT")        return TierRole::Root;
    if (s == "ACCELERATOR") return TierRole::Accelerator;
    if (s == "STAGING")     return TierRole::Staging;
    if (s == "BUFFER")      return TierRole::Buffer;
    if (s == "PERSISTENT")  return TierRole::Persistent;
    if (s == "CUSTOM")      return TierRole::Custom;
    return std::nullopt;
}

inline std::optional<Provenance> provenance_from_string(std::string_view s) noexcept {
    if (s == "WINDOWS_GLOBAL_MEMORY_STATUS_EX") return Provenance::WindowsGlobalMemoryStatusEx;
    if (s == "WINDOWS_PERFORMANCE_INFO")        return Provenance::WindowsPerformanceInfo;
    if (s == "WINDOWS_PROCESS_MEMORY_INFO")     return Provenance::WindowsProcessMemoryInfo;
    if (s == "WINDOWS_PROCESS_QUERY")           return Provenance::WindowsProcessQuery;
    if (s == "CUDA_DRIVER_API")                 return Provenance::CudaDriverApi;
    if (s == "FILESYSTEM")                      return Provenance::Filesystem;
    if (s == "RUNTIME_REGISTRATION")            return Provenance::RuntimeRegistration;
    if (s == "CONFIGURED_POLICY")                return Provenance::ConfiguredPolicy;
    if (s == "SYNTHETIC_INPUT")                 return Provenance::SyntheticInput;
    if (s == "IMPORTED_SNAPSHOT")               return Provenance::ImportedSnapshot;
    if (s == "INFERRED_METRIC")                 return Provenance::InferredMetric;
    if (s == "ALLOCATION_PROBE")                return Provenance::AllocationProbe;
    if (s == "CUDA_RUNTIME_API")                 return Provenance::CudaRuntimeApi;
    if (s == "UNKNOWN")                          return Provenance::Unknown;
    return std::nullopt;
}

inline std::optional<Validity> validity_from_string(std::string_view s) noexcept {
    if (s == "VALID")       return Validity::Valid;
    if (s == "STALE")       return Validity::Stale;
    if (s == "PARTIAL")     return Validity::Partial;
    if (s == "FAILED")      return Validity::Failed;
    if (s == "UNAVAILABLE") return Validity::Unavailable;
    return std::nullopt;
}

inline std::optional<ProviderStatus> provider_status_from_string(std::string_view s) noexcept {
    if (s == "HEALTHY")     return ProviderStatus::Healthy;
    if (s == "STALE")       return ProviderStatus::Stale;
    if (s == "FAILED")      return ProviderStatus::Failed;
    if (s == "UNAVAILABLE") return ProviderStatus::Unavailable;
    if (s == "PARTIAL")     return ProviderStatus::Partial;
    return std::nullopt;
}

inline std::optional<TrendDirection> trend_from_string(std::string_view s) noexcept {
    if (s == "FLAT")    return TrendDirection::Flat;
    if (s == "RISING")  return TrendDirection::Rising;
    if (s == "FALLING") return TrendDirection::Falling;
    if (s == "UNKNOWN") return TrendDirection::Unknown;
    return std::nullopt;
}

// Severity rank; Unknown is treated as the least informative (does not
// participate in escalation ordering against known levels).
inline constexpr int severity_rank(PressureLevel v) noexcept {
    switch (v) {
        case PressureLevel::Normal:   return 0;
        case PressureLevel::Elevated: return 1;
        case PressureLevel::High:     return 2;
        case PressureLevel::Critical: return 3;
        case PressureLevel::Exhausted:return 4;
        case PressureLevel::Unknown:  return -1;
    }
    return -1;
}

// True if the level is one of the five known, ordered levels.
inline constexpr bool is_known_level(PressureLevel v) noexcept {
    return v == PressureLevel::Normal || v == PressureLevel::Elevated ||
           v == PressureLevel::High || v == PressureLevel::Critical ||
           v == PressureLevel::Exhausted;
}

inline std::uint64_t safe_gib(double v) noexcept { return static_cast<std::uint64_t>(v * 1024.0 * 1024.0 * 1024.0); }

} // namespace memory_pressure
