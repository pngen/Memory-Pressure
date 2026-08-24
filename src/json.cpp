#include "memory_pressure/json.h"

#include <charconv>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>

namespace memory_pressure {

namespace {

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Append a single JSON string escape for the given char code.
void append_json_escape(std::string& out, char c) {
    switch (c) {
        case '"':  out.push_back('\\'); out.push_back('"'); break;
        case '\\': out.push_back('\\'); out.push_back('\\'); break;
        case '\b': out.push_back('\\'); out.push_back('b'); break;
        case '\f': out.push_back('\\'); out.push_back('f'); break;
        case '\n': out.push_back('\\'); out.push_back('n'); break;
        case '\r': out.push_back('\\'); out.push_back('r'); break;
        case '\t': out.push_back('\\'); out.push_back('t'); break;
        default: {
            char esc[8];
            std::snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
            out += esc;
            break;
        }
    }
}

struct Parser {
    const std::string_view s;
    std::size_t pos = 0;
    const JsonParseLimits limits;
    std::size_t depth = 0;
    bool failed = false;

    Parser(std::string_view text, const JsonParseLimits& lim) : s(text), limits(lim) {}

    bool at_end() const noexcept { return pos >= s.size(); }
    char peek() const noexcept { return pos < s.size() ? s[pos] : '\0'; }
    char get() noexcept { return pos < s.size() ? s[pos++] : '\0'; }

    void skip_ws() {
        while (!at_end()) {
            char c = s[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos;
            else break;
        }
    }

    bool fail() { failed = true; return false; }

    bool parse_value(Json& out) {
        if (failed) return false;
        skip_ws();
        if (at_end()) return fail();
        char c = peek();
        if (c == '{') return parse_object(out);
        if (c == '[') return parse_array(out);
        if (c == '"') return parse_string(out);
        if (c == 't') return parse_literal(out, "true", true);
        if (c == 'f') return parse_literal(out, "false", false);
        if (c == 'n') return parse_literal_null(out);
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out);
        return fail();
    }

    bool parse_literal(Json& out, const char* lit, bool val) {
        const std::size_t n = std::strlen(lit);
        if (s.substr(pos, n) != lit) return fail();
        pos += n;
        out = Json(val);
        return true;
    }
    bool parse_literal_null(Json& out) {
        const char* lit = "null";
        const std::size_t n = std::strlen(lit);
        if (s.substr(pos, n) != lit) return fail();
        pos += n;
        out = Json();
        return true;
    }

    bool parse_object(Json& out) {
        if (depth >= limits.max_depth) return fail();
        ++depth;
        get();
        Json obj = Json::object();
        skip_ws();
        if (peek() == '}') { get(); --depth; out = std::move(obj); return true; }
        for (;;) {
            skip_ws();
            if (peek() != '"') return fail();
            Json key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (peek() != ':') return fail();
            get();
            Json val;
            if (!parse_value(val)) return false;
            obj[std::move(key.as_string().value_or(std::string()))] = std::move(val);
            skip_ws();
            char e = get();
            if (e == '}') { --depth; out = std::move(obj); return true; }
            if (e != ',') return fail();
        }
    }

    bool parse_array(Json& out) {
        if (depth >= limits.max_depth) return fail();
        ++depth;
        get();
        Json arr = Json::array();
        skip_ws();
        if (peek() == ']') { get(); --depth; out = std::move(arr); return true; }
        for (;;) {
            Json val;
            if (!parse_value(val)) return false;
            arr.push_back(std::move(val));
            skip_ws();
            char e = get();
            if (e == ']') { --depth; out = std::move(arr); return true; }
            if (e != ',') return fail();
        }
    }

    bool parse_string(Json& out) {
        if (depth >= limits.max_depth) return fail();
        get();
        std::string str;
        str.reserve(64);
        for (;;) {
            if (at_end()) return fail();
            unsigned char c = static_cast<unsigned char>(get());
            if (c == '"') break;
            if (c < 0x20) return fail();
            if (c == '\\') {
                if (at_end()) return fail();
                char e = get();
                switch (e) {
                    case '"': str.push_back('"'); break;
                    case '\\': str.push_back('\\'); break;
                    case '/': str.push_back('/'); break;
                    case 'b': str.push_back('\b'); break;
                    case 'f': str.push_back('\f'); break;
                    case 'n': str.push_back('\n'); break;
                    case 'r': str.push_back('\r'); break;
                    case 't': str.push_back('\t'); break;
                    case 'u': if (!parse_unicode_escape(str)) return false; break;
                    default: return fail();
                }
            } else {
                str.push_back(static_cast<char>(c));
            }
            if (str.size() > limits.max_string) return fail();
        }
        out = Json(std::move(str));
        return true;
    }

    bool parse_unicode_escape(std::string& out) {
        std::uint32_t cp = 0;
        for (int i = 0; i < 4; ++i) {
            if (at_end()) return fail();
            int h = hex_value(get());
            if (h < 0) return fail();
            cp = (cp << 4) | static_cast<std::uint32_t>(h);
        }
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (pos + 1 >= s.size() || s[pos] != '\\' || s[pos + 1] != 'u') return fail();
            pos += 2;
            std::uint32_t lo = 0;
            for (int i = 0; i < 4; ++i) {
                if (at_end()) return fail();
                int h = hex_value(get());
                if (h < 0) return fail();
                lo = (lo << 4) | static_cast<std::uint32_t>(h);
            }
            if (lo < 0xDC00 || lo > 0xDFFF) return fail();
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return fail();
        }
        append_utf8(out, cp);
        return true;
    }

    bool parse_number(Json& out) {
        const std::size_t start = pos;
        if (peek() == '-') get();
        if (at_end()) return fail();
        if (peek() == '0') {
            get();
        } else if (peek() >= '1' && peek() <= '9') {
            while (!at_end() && peek() >= '0' && peek() <= '9') get();
        } else {
            return fail();
        }
        if (!at_end() && peek() == '.') {
            get();
            if (at_end() || peek() < '0' || peek() > '9') return fail();
            while (!at_end() && peek() >= '0' && peek() <= '9') get();
        }
        if (!at_end() && (peek() == 'e' || peek() == 'E')) {
            get();
            if (!at_end() && (peek() == '+' || peek() == '-')) get();
            if (at_end() || peek() < '0' || peek() > '9') return fail();
            while (!at_end() && peek() >= '0' && peek() <= '9') get();
        }
        const char* b = s.data() + start;
        const char* e = s.data() + pos;
        double value = 0.0;
        auto[pc, ec] = std::from_chars(b, e, value, std::chars_format::general);
        if (ec != std::errc() || pc != e) return fail();
        if (!std::isfinite(value) && limits.reject_non_finite) return fail();
        out = Json(value);
        return true;
    }
};

void serialize_to(const Json& v, std::string& out, bool& finite_ok) {
    switch (v.type()) {
        case Json::Type::Null: out += "null"; break;
        case Json::Type::Bool: out += (v.as_bool().value_or(false) ? "true" : "false"); break;
        case Json::Type::Number: {
            const double d = v.as_number().value_or(0.0);
            if (!std::isfinite(d)) { finite_ok = false; return; }
            char buf[64];
            if (std::floor(d) == d && d >= static_cast<double>(std::numeric_limits<std::int64_t>::min())
                && d <= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
                auto res = std::to_chars(buf, buf + sizeof(buf), static_cast<std::int64_t>(d));
                out.append(buf, res.ptr);
            } else {
                auto res = std::to_chars(buf, buf + sizeof(buf), d, std::chars_format::general);
                out.append(buf, res.ptr);
            }
            break;
        }
        case Json::Type::String: {
            out.push_back('"');
            for (char c : v.as_string().value_or(std::string())) {
                const unsigned char uc = static_cast<unsigned char>(c);
                if (uc < 0x20) append_json_escape(out, c);
                else if (c == '"' || c == '\\') append_json_escape(out, c);
                else out.push_back(c);
            }
            out.push_back('"');
            break;
        }
        case Json::Type::Array: {
            out.push_back('[');
            const auto& items = v.array_items();
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (i) out.push_back(',');
                serialize_to(items[i], out, finite_ok);
            }
            out.push_back(']');
            break;
        }
        case Json::Type::Object: {
            out.push_back('{');
            const auto& obj = v.object_items();
            bool first = true;
            for (const auto& [k, val] : obj) {
                if (!first) out.push_back(',');
                first = false;
                Json sk(k);
                serialize_to(sk, out, finite_ok);
                out.push_back(':');
                serialize_to(val, out, finite_ok);
            }
            out.push_back('}');
            break;
        }
    }
}

} // anonymous namespace

struct Json::Data {
    Type type = Type::Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;
};

Json::Json(std::shared_ptr<Data> d) : data_(std::move(d)) {}
Json::Json(bool b) : data_(std::make_shared<Data>()) { data_->type = Type::Bool; data_->b = b; }
Json::Json(double v) : data_(std::make_shared<Data>()) { data_->type = Type::Number; data_->num = v; }
Json::Json(std::int64_t v) : data_(std::make_shared<Data>()) { data_->type = Type::Number; data_->num = static_cast<double>(v); }
Json::Json(std::uint64_t v) : data_(std::make_shared<Data>()) { data_->type = Type::Number; data_->num = static_cast<double>(v); }
Json::Json(const char* s) : data_(std::make_shared<Data>()) { data_->type = Type::String; data_->str = s ? s : ""; }
Json::Json(std::string s) : data_(std::make_shared<Data>()) { data_->type = Type::String; data_->str = std::move(s); }
Json::Json(std::string_view s) : data_(std::make_shared<Data>()) { data_->type = Type::String; data_->str.assign(s); }

Json Json::array() { Json j; j.data_ = std::make_shared<Data>(); j.data_->type = Type::Array; return j; }
Json Json::object() { Json j; j.data_ = std::make_shared<Data>(); j.data_->type = Type::Object; return j; }
Json Json::null() { return Json(); }

Json::Type Json::type() const noexcept { return data_ ? data_->type : Type::Null; }
bool Json::is_null() const noexcept { return type() == Type::Null; }
bool Json::is_bool() const noexcept { return type() == Type::Bool; }
bool Json::is_number() const noexcept { return type() == Type::Number; }
bool Json::is_string() const noexcept { return type() == Type::String; }
bool Json::is_array() const noexcept { return type() == Type::Array; }
bool Json::is_object() const noexcept { return type() == Type::Object; }

std::optional<bool> Json::as_bool() const noexcept { if (!is_bool() || !data_) return std::nullopt; return data_->b; }
std::optional<double> Json::as_number() const noexcept { if (!is_number() || !data_) return std::nullopt; return data_->num; }
std::optional<std::string> Json::as_string() const noexcept { if (!is_string() || !data_) return std::nullopt; return data_->str; }

std::optional<std::int64_t> Json::as_int64() const noexcept {
    auto d = as_number(); if (!d) return std::nullopt;
    const double v = *d;
    if (!std::isfinite(v)) return std::nullopt;
    if (std::floor(v) != v) return std::nullopt;
    if (v < static_cast<double>(std::numeric_limits<std::int64_t>::min()) || v > static_cast<double>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
    return static_cast<std::int64_t>(v);
}
std::optional<std::uint64_t> Json::as_uint64() const noexcept {
    auto d = as_number(); if (!d) return std::nullopt;
    const double v = *d;
    if (!std::isfinite(v)) return std::nullopt;
    if (std::floor(v) != v) return std::nullopt;
    if (v < 0 || v > 9007199254740992.0) return std::nullopt;
    return static_cast<std::uint64_t>(v);
}

const std::vector<Json>& Json::array_items() const noexcept { return (data_ && data_->type == Type::Array) ? data_->arr : empty_arr(); }
std::vector<Json>& Json::array_items() noexcept { static std::vector<Json> dummy; return (data_ && data_->type == Type::Array) ? data_->arr : dummy; }
void Json::push_back(Json v) { if (data_ && data_->type == Type::Array) data_->arr.push_back(std::move(v)); }
const std::map<std::string, Json>& Json::object_items() const noexcept { return (data_ && data_->type == Type::Object) ? data_->obj : empty_obj(); }
std::map<std::string, Json>& Json::object_items() noexcept { static std::map<std::string, Json> dummy; return (data_ && data_->type == Type::Object) ? data_->obj : dummy; }
Json& Json::operator[](const std::string& key) {
    if (!data_ || data_->type != Type::Object) { data_ = std::make_shared<Data>(); data_->type = Type::Object; }
    return data_->obj[key];
}
Json& Json::operator[](std::string&& key) {
    if (!data_ || data_->type != Type::Object) { data_ = std::make_shared<Data>(); data_->type = Type::Object; }
    return data_->obj[std::move(key)];
}
bool Json::has(const std::string& key) const noexcept { return data_ && data_->type == Type::Object && data_->obj.find(key) != data_->obj.end(); }
const Json* Json::find(const std::string& key) const noexcept {
    if (!data_ || data_->type != Type::Object) return nullptr;
    auto it = data_->obj.find(key);
    return it == data_->obj.end() ? nullptr : &it->second;
}
std::size_t Json::size() const noexcept {
    if (!data_) return 0;
    if (data_->type == Type::Array) return data_->arr.size();
    if (data_->type == Type::Object) return data_->obj.size();
    return 0;
}
std::size_t Json::byte_size() const noexcept { auto s = json_serialize(*this); return s ? s->size() : 0; }
bool Json::is_finite() const noexcept {
    if (!data_) return true;
    if (data_->type == Type::Number) return std::isfinite(data_->num);
    bool ok = true;
    if (data_->type == Type::Array) { for (const auto& v : data_->arr) ok = ok && v.is_finite(); }
    else if (data_->type == Type::Object) { for (const auto& [k, v] : data_->obj) { (void)k; ok = ok && v.is_finite(); } }
    return ok;
}
void Json::visit(const std::function<void(const Json&)>& f) const { if (f) f(*this); }

const std::vector<Json>& Json::empty_arr() noexcept { static const std::vector<Json> v; return v; }
const std::map<std::string, Json>& Json::empty_obj() noexcept { static const std::map<std::string, Json> m; return m; }

std::optional<Json> json_parse(std::string_view text, const JsonParseLimits& limits) {
    if (text.size() > limits.max_bytes) return std::nullopt;
    Parser p(text, limits);
    Json root;
    if (!p.parse_value(root)) return std::nullopt;
    p.skip_ws();
    if (!p.at_end()) return std::nullopt;
    return root;
}

std::optional<std::string> json_serialize(const Json& value) {
    std::string out; out.reserve(256);
    bool ok = true;
    serialize_to(value, out, ok);
    if (!ok) return std::nullopt;
    return out;
}

std::string json_dump(const Json& value) {
    auto s = json_serialize(value);
    return s ? *s : std::string("null");
}

} // namespace memory_pressure
