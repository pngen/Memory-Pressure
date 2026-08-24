#pragma once
// Versioned, bounded JSON value type and strict parser/serializer.
//
// Memory Pressure serializes policy, snapshots, events, explanations, and
// statistics through this module.  It is deliberately strict so that
// malformed, oversized, or non-finite input is rejected *before* any
// allocation decision is made downstream.
//
// Numbers are stored as double.  Capacities in practice are far below 2^53
// (9 PB), so integer capacity round-trips are exact; this is stated in
// LIMITATIONS.md and enforced with bounds checks in the semantic layer.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace memory_pressure {

class Json {
public:
    enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

    // Construction.
    Json() = default;                            // null
    Json(bool b);
    Json(double v);
    Json(std::int64_t v);
    Json(std::uint64_t v);
    Json(const char* s);
    Json(std::string s);
    Json(std::string_view s);

    static Json array();
    static Json object();
    static Json null();

    // Type queries.
    Type type() const noexcept;
    bool is_null() const noexcept;
    bool is_bool() const noexcept;
    bool is_number() const noexcept;
    bool is_string() const noexcept;
    bool is_array() const noexcept;
    bool is_object() const noexcept;

    // Primitive accessors.  Return std::nullopt when the accessor does not
    // apply, rather than throwing or fabricating a value.
    std::optional<bool> as_bool() const noexcept;
    std::optional<double> as_number() const noexcept;
    std::optional<std::string> as_string() const noexcept;

    // Integer view that fails when the value is not integral or is out of
    // range for the target type.
    std::optional<std::int64_t> as_int64() const noexcept;
    std::optional<std::uint64_t> as_uint64() const noexcept;

    // Array accessors (only valid for arrays).
    const std::vector<Json>& array_items() const noexcept;
    std::vector<Json>& array_items() noexcept;
    void push_back(Json v);

    // Object accessors (only valid for objects).  A std::map keeps key order
    // deterministic for stable serialization.
    const std::map<std::string, Json>& object_items() const noexcept;
    std::map<std::string, Json>& object_items() noexcept;
    Json& operator[](const std::string& key);
    Json& operator[](std::string&& key);
    bool has(const std::string& key) const noexcept;
    const Json* find(const std::string& key) const noexcept;

    // Size accounting and bounds.
    std::size_t size() const noexcept;   // array length / object entry count
    std::size_t byte_size() const noexcept; // approximate serialized size in bytes

    // Semantic validation: reject NaN / infinity when present.
    bool is_finite() const noexcept;

    // Traversal helpers used by validators.
    void visit(const std::function<void(const Json&)>& f) const;

private:
    struct Data;
    explicit Json(std::shared_ptr<Data> d);
    static const std::vector<Json>& empty_arr() noexcept;
    static const std::map<std::string, Json>& empty_obj() noexcept;
    std::shared_ptr<Data> data_;
};

// ---------------------------------------------------------------------------
// Strict parser.  Returns nullopt on any malformed, oversized, or non-finite
// input.  Limits apply to nesting depth, aggregate byte length, and string
// length so adversarial input cannot cause unbounded allocation.
// ---------------------------------------------------------------------------
struct JsonParseLimits {
    std::size_t max_depth = 128;          // nesting depth
    std::size_t max_bytes = 4 * 1024 * 1024; // total parse input bytes
    std::size_t max_string = 1024 * 1024;    // single string length
    bool reject_non_finite = true;         // reject NaN / Infinity literals
};

std::optional<Json> json_parse(std::string_view text, const JsonParseLimits& limits = {});

// ---------------------------------------------------------------------------
// Serializer.  Compact output with strict escaping.  Returns nullopt when the
// value contains NaN / infinity (never silently emit invalid JSON).
// ---------------------------------------------------------------------------
std::optional<std::string> json_serialize(const Json& value);

// Variant of the above that always succeeds for valid finite values; provided
// for convenience in internal paths where values are known finite.
std::string json_dump(const Json& value);

inline const char* to_string(Json::Type t) noexcept {
    switch (t) {
        case Json::Type::Null:   return "null";
        case Json::Type::Bool:   return "bool";
        case Json::Type::Number: return "number";
        case Json::Type::String: return "string";
        case Json::Type::Array:  return "array";
        case Json::Type::Object: return "object";
    }
    return "unknown";
}

} // namespace memory_pressure
