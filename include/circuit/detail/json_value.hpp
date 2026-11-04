/// @file json_value.hpp
/// @brief -- Minimal in-house JSON value type + parser + writer for
/// @ingroup ir
///
/// Circuit serialisation. NOT a public API:
/// lives in qplayer::detail because bars external JSON libraries
/// (vcpkg/Conan/nlohmann etc.) and we only need the subset Circuit IR
/// produces -- objects, arrays, strings, numbers (double / int64), bool, null.
///
/// Numbers: stored as double internally. Integers up to 2^53 round-trip
/// exactly (IEEE-754 double mantissa). QubitIndex / ClassicalBitIndex are
/// uint32 -> fits comfortably. duration_ns is already double so it round-
/// trips bit-exact when written with std::setprecision(17).
///
/// Parser tolerances: rejects trailing commas, comments, NaN/Infinity, and
/// numbers beyond double range. Errors carry the byte offset of the failing
/// character so Circuit::from_json can surface "circuit.json: <reason>" with
/// useful diagnostics.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace qplayer::detail {

class JsonValue;
/// A JSON array: an ordered list of values.
using JsonArray  = std::vector<JsonValue>;
/// A JSON object; the writer sorts keys so output is byte-stable.
using JsonObject = std::map<std::string, JsonValue>;  // insertion-order-agnostic; we sort keys in writer for stability

/// Which of the seven JSON types a JsonValue currently holds.
enum class JsonKind { Null, Bool, Int, Double, String, Array, Object };

/// Discriminated-union value type. Heap-allocates nested containers so the
/// payload size is small (sizeof(JsonValue) ~ 32 bytes on x86_64). Movable +
/// copyable so callers can pass values around without ownership ceremony.
class JsonValue {
public:
    JsonValue() : kind_(JsonKind::Null) {}
    explicit JsonValue(bool v)                : kind_(JsonKind::Bool),   bool_(v) {}
    explicit JsonValue(int v)                 : kind_(JsonKind::Int),    int_(v) {}
    explicit JsonValue(std::int64_t v)        : kind_(JsonKind::Int),    int_(v) {}
    explicit JsonValue(std::uint32_t v)       : kind_(JsonKind::Int),    int_(static_cast<std::int64_t>(v)) {}
    explicit JsonValue(std::uint64_t v)       : kind_(JsonKind::Int),    int_(static_cast<std::int64_t>(v)) {}
    explicit JsonValue(double v)              : kind_(JsonKind::Double), double_(v) {}
    explicit JsonValue(std::string v)         : kind_(JsonKind::String), string_(std::move(v)) {}
    explicit JsonValue(const char* v)         : kind_(JsonKind::String), string_(v) {}
    explicit JsonValue(JsonArray v)           : kind_(JsonKind::Array),  array_(std::make_unique<JsonArray>(std::move(v))) {}
    explicit JsonValue(JsonObject v)          : kind_(JsonKind::Object), object_(std::make_unique<JsonObject>(std::move(v))) {}

    // Copy: deep-copy the heap nodes.
    JsonValue(const JsonValue& other) { copy_from(other); }
    JsonValue& operator=(const JsonValue& other) {
        if (this != &other) {
            reset();
            copy_from(other);
        }
        return *this;
    }
    JsonValue(JsonValue&&) noexcept            = default;
    JsonValue& operator=(JsonValue&&) noexcept = default;
    ~JsonValue()                                = default;

    JsonKind kind() const noexcept { return kind_; }

    bool                is_null()   const noexcept { return kind_ == JsonKind::Null;   }
    bool                is_bool()   const noexcept { return kind_ == JsonKind::Bool;   }
    bool                is_int()    const noexcept { return kind_ == JsonKind::Int;    }
    bool                is_double() const noexcept { return kind_ == JsonKind::Double; }
    bool                is_number() const noexcept { return kind_ == JsonKind::Int || kind_ == JsonKind::Double; }
    bool                is_string() const noexcept { return kind_ == JsonKind::String; }
    bool                is_array()  const noexcept { return kind_ == JsonKind::Array;  }
    bool                is_object() const noexcept { return kind_ == JsonKind::Object; }

    // Accessors: throw CircuitError("kind mismatch") when the active kind
    // does not match the requested view.
    bool                as_bool()   const;
    std::int64_t        as_int()    const;
    double              as_double() const;
    const std::string&  as_string() const;
    const JsonArray&    as_array()  const;
    const JsonObject&   as_object() const;

    // Object helpers: contains(key) returns whether the key exists; at(key)
    // returns the value (throws CircuitError on missing key + non-object).
    bool               contains(const std::string& key) const;
    const JsonValue&   at      (const std::string& key) const;

private:
    void copy_from(const JsonValue& other) {
        kind_ = other.kind_;
        switch (kind_) {
            case JsonKind::Null:   break;
            case JsonKind::Bool:   bool_   = other.bool_;   break;
            case JsonKind::Int:    int_    = other.int_;    break;
            case JsonKind::Double: double_ = other.double_; break;
            case JsonKind::String: string_ = other.string_; break;
            case JsonKind::Array:  array_  = std::make_unique<JsonArray>(*other.array_);   break;
            case JsonKind::Object: object_ = std::make_unique<JsonObject>(*other.object_); break;
        }
    }
    void reset() {
        kind_ = JsonKind::Null;
        string_.clear();
        array_.reset();
        object_.reset();
    }

    JsonKind                     kind_   = JsonKind::Null;
    bool                         bool_   = false;
    std::int64_t                 int_    = 0;
    double                       double_ = 0.0;
    std::string                  string_;
    std::unique_ptr<JsonArray>   array_;
    std::unique_ptr<JsonObject>  object_;
};

// ── Parse / write API ───────────────────────────────────────────────────────
// parse_json: returns the root value. On syntax error throws CircuitError
//   with message "JSON syntax error at offset N: <reason>". The offset is
//   the byte index into `text` of the failing character.
// write_json: emits canonical text -- object keys sorted lexicographically,
//   numbers via std::setprecision(17), strings with minimal escaping. Pretty
//   formatting (one item per line, two-space indent) is enabled when pretty
//   is true; compact (no whitespace) when false.

JsonValue   parse_json(std::string_view text);
std::string write_json(const JsonValue& value, bool pretty = true);

}  // namespace qplayer::detail
