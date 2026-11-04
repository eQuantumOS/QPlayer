// json_value.cpp -- implementation of the minimal JSON parser + writer used
// by Circuit serialisation. Hand-rolled recursive-descent parser;
// rejects extensions (trailing commas, comments, NaN/Inf) so we cannot
// silently accept malformed inputs from foreign tools.

#include "circuit/detail/json_value.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

#include "error.hpp"

namespace qplayer::detail {

// ── JsonValue accessors ──────────────────────────────────────────────────────

bool JsonValue::as_bool() const {
    if (kind_ != JsonKind::Bool) {
        throw CircuitError{"JsonValue: expected bool, got kind " +
                           std::to_string(static_cast<int>(kind_))};
    }
    return bool_;
}

std::int64_t JsonValue::as_int() const {
    if (kind_ == JsonKind::Int)    return int_;
    if (kind_ == JsonKind::Double) return static_cast<std::int64_t>(double_);
    throw CircuitError{"JsonValue: expected int, got kind " +
                       std::to_string(static_cast<int>(kind_))};
}

double JsonValue::as_double() const {
    if (kind_ == JsonKind::Double) return double_;
    if (kind_ == JsonKind::Int)    return static_cast<double>(int_);
    throw CircuitError{"JsonValue: expected double, got kind " +
                       std::to_string(static_cast<int>(kind_))};
}

const std::string& JsonValue::as_string() const {
    if (kind_ != JsonKind::String) {
        throw CircuitError{"JsonValue: expected string, got kind " +
                           std::to_string(static_cast<int>(kind_))};
    }
    return string_;
}

const JsonArray& JsonValue::as_array() const {
    if (kind_ != JsonKind::Array) {
        throw CircuitError{"JsonValue: expected array, got kind " +
                           std::to_string(static_cast<int>(kind_))};
    }
    return *array_;
}

const JsonObject& JsonValue::as_object() const {
    if (kind_ != JsonKind::Object) {
        throw CircuitError{"JsonValue: expected object, got kind " +
                           std::to_string(static_cast<int>(kind_))};
    }
    return *object_;
}

bool JsonValue::contains(const std::string& key) const {
    if (kind_ != JsonKind::Object) return false;
    return object_->find(key) != object_->end();
}

const JsonValue& JsonValue::at(const std::string& key) const {
    const auto& obj = as_object();
    const auto it = obj.find(key);
    if (it == obj.end()) {
        throw CircuitError{"JsonValue: missing required key \"" + key + "\""};
    }
    return it->second;
}

// ─────────────────────────────────────────────────────────────────────────────
// Parser -- recursive descent over std::string_view
// ─────────────────────────────────────────────────────────────────────────────

namespace {

struct Parser {
    std::string_view src;
    std::size_t      pos = 0;

    [[noreturn]] void error(const std::string& reason) const {
        throw CircuitError{"JSON syntax error at offset " + std::to_string(pos) +
                           ": " + reason};
    }

    void skip_ws() {
        while (pos < src.size()) {
            const char c = src[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos;
            else break;
        }
    }

    char peek() const {
        if (pos >= src.size()) {
            throw CircuitError{"JSON syntax error at offset " + std::to_string(pos) +
                               ": unexpected end of input"};
        }
        return src[pos];
    }

    char consume() {
        const char c = peek();
        ++pos;
        return c;
    }

    bool match_literal(const char* lit) {
        const std::size_t len = std::strlen(lit);
        if (pos + len > src.size()) return false;
        for (std::size_t i = 0; i < len; ++i) {
            if (src[pos + i] != lit[i]) return false;
        }
        pos += len;
        return true;
    }

    JsonValue parse_value() {
        skip_ws();
        if (pos >= src.size()) error("expected value");
        const char c = src[pos];
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') return JsonValue(parse_string());
        if (c == 't' || c == 'f') return parse_bool();
        if (c == 'n') return parse_null();
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        error(std::string{"unexpected character '"} + c + "'");
    }

    JsonValue parse_null() {
        if (!match_literal("null")) error("expected 'null'");
        return JsonValue{};
    }

    JsonValue parse_bool() {
        if (match_literal("true"))  return JsonValue(true);
        if (match_literal("false")) return JsonValue(false);
        error("expected 'true' or 'false'");
    }

    std::string parse_string() {
        if (consume() != '"') error("expected '\"'");
        std::string out;
        while (pos < src.size()) {
            const char c = src[pos++];
            if (c == '"') return out;
            if (c == '\\') {
                if (pos >= src.size()) error("unterminated escape sequence");
                const char esc = src[pos++];
                switch (esc) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        // Decode 4 hex digits to a code point. Surrogate
                        // pairs are NOT decoded -- Circuit JSON never emits
                        // non-ASCII strings (gate names, register names),
                        // so reject anything beyond U+007F at parse time.
                        if (pos + 4 > src.size()) error("\\uXXXX requires 4 hex digits");
                        unsigned code = 0;
                        for (int i = 0; i < 4; ++i) {
                            const char h = src[pos++];
                            unsigned d;
                            if (h >= '0' && h <= '9') d = h - '0';
                            else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                            else error("invalid hex digit in \\uXXXX");
                            code = (code << 4) | d;
                        }
                        if (code > 0x7F) error("non-ASCII code points are not supported");
                        out += static_cast<char>(code);
                        break;
                    }
                    default: error(std::string{"unknown escape '\\"} + esc + "'");
                }
            } else {
                out += c;
            }
        }
        error("unterminated string");
    }

    JsonValue parse_number() {
        const std::size_t start = pos;
        if (src[pos] == '-') ++pos;
        if (pos >= src.size()) error("expected digit after '-'");

        bool is_float = false;

        if (src[pos] == '0') {
            ++pos;
        } else if (src[pos] >= '1' && src[pos] <= '9') {
            while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
        } else {
            error("expected digit");
        }

        if (pos < src.size() && src[pos] == '.') {
            is_float = true;
            ++pos;
            if (pos >= src.size() || src[pos] < '0' || src[pos] > '9') {
                error("expected digit after '.'");
            }
            while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
        }

        if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
            is_float = true;
            ++pos;
            if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) ++pos;
            if (pos >= src.size() || src[pos] < '0' || src[pos] > '9') {
                error("expected digit in exponent");
            }
            while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
        }

        const std::string token{src.substr(start, pos - start)};
        if (is_float) {
            try {
                const double v = std::stod(token);
                if (std::isnan(v) || std::isinf(v)) error("NaN / Infinity not allowed");
                return JsonValue(v);
            } catch (const std::out_of_range&) {
                error("number out of double range");
            } catch (const std::invalid_argument&) {
                error("malformed number");
            }
        } else {
            try {
                const std::int64_t v = std::stoll(token);
                return JsonValue(v);
            } catch (const std::out_of_range&) {
                error("integer out of int64 range");
            } catch (const std::invalid_argument&) {
                error("malformed integer");
            }
        }
    }

    JsonValue parse_array() {
        if (consume() != '[') error("expected '['");
        JsonArray arr;
        skip_ws();
        if (pos < src.size() && src[pos] == ']') {
            ++pos;
            return JsonValue(std::move(arr));
        }
        while (true) {
            arr.push_back(parse_value());
            skip_ws();
            if (pos >= src.size()) error("expected ',' or ']' in array");
            const char c = src[pos++];
            if (c == ']') return JsonValue(std::move(arr));
            if (c != ',') error("expected ',' or ']' in array");
            skip_ws();
            // Reject trailing commas.
            if (pos < src.size() && src[pos] == ']') error("trailing comma in array");
        }
    }

    JsonValue parse_object() {
        if (consume() != '{') error("expected '{'");
        JsonObject obj;
        skip_ws();
        if (pos < src.size() && src[pos] == '}') {
            ++pos;
            return JsonValue(std::move(obj));
        }
        while (true) {
            skip_ws();
            const std::string key = parse_string();
            skip_ws();
            if (pos >= src.size() || src[pos] != ':') error("expected ':' after key");
            ++pos;
            JsonValue v = parse_value();
            obj.emplace(std::move(key), std::move(v));
            skip_ws();
            if (pos >= src.size()) error("expected ',' or '}' in object");
            const char c = src[pos++];
            if (c == '}') return JsonValue(std::move(obj));
            if (c != ',') error("expected ',' or '}' in object");
            skip_ws();
            if (pos < src.size() && src[pos] == '}') error("trailing comma in object");
        }
    }
};

}  // namespace

JsonValue parse_json(std::string_view text) {
    Parser p{text, 0};
    JsonValue root = p.parse_value();
    p.skip_ws();
    if (p.pos != text.size()) {
        throw CircuitError{"JSON syntax error at offset " + std::to_string(p.pos) +
                           ": trailing data after root value"};
    }
    return root;
}

// ─────────────────────────────────────────────────────────────────────────────
// Writer -- canonical (sorted keys), pretty or compact
// ─────────────────────────────────────────────────────────────────────────────

namespace {

void escape_string(std::ostringstream& out, const std::string& s) {
    out << '"';
    for (char c : s) {
        switch (c) {
            case '"':  out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b";  break;
            case '\f': out << "\\f";  break;
            case '\n': out << "\\n";  break;
            case '\r': out << "\\r";  break;
            case '\t': out << "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    // Control character -- emit \u00XX. Circuit IR strings never
                    // contain these but we encode safely anyway.
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out << buf;
                } else {
                    out << c;
                }
        }
    }
    out << '"';
}

void write_indent(std::ostringstream& out, int depth) {
    for (int i = 0; i < depth; ++i) out << "  ";
}

void write_value(std::ostringstream& out, const JsonValue& v, bool pretty, int depth);

void write_array(std::ostringstream& out, const JsonArray& arr, bool pretty, int depth) {
    if (arr.empty()) { out << "[]"; return; }
    out << '[';
    for (std::size_t i = 0; i < arr.size(); ++i) {
        if (i > 0) out << ',';
        if (pretty) { out << '\n'; write_indent(out, depth + 1); }
        write_value(out, arr[i], pretty, depth + 1);
    }
    if (pretty) { out << '\n'; write_indent(out, depth); }
    out << ']';
}

void write_object(std::ostringstream& out, const JsonObject& obj, bool pretty, int depth) {
    if (obj.empty()) { out << "{}"; return; }
    out << '{';
    bool first = true;
    for (const auto& [key, val] : obj) {  // std::map iterates in sorted order
        if (!first) out << ',';
        first = false;
        if (pretty) { out << '\n'; write_indent(out, depth + 1); }
        escape_string(out, key);
        out << ':';
        if (pretty) out << ' ';
        write_value(out, val, pretty, depth + 1);
    }
    if (pretty) { out << '\n'; write_indent(out, depth); }
    out << '}';
}

void write_value(std::ostringstream& out, const JsonValue& v, bool pretty, int depth) {
    switch (v.kind()) {
        case JsonKind::Null:   out << "null"; break;
        case JsonKind::Bool:   out << (v.as_bool() ? "true" : "false"); break;
        case JsonKind::Int:    out << v.as_int(); break;
        case JsonKind::Double: {
            // std::setprecision(17) gives round-trip-safe double output.
            std::ostringstream tmp;
            tmp << std::setprecision(17) << v.as_double();
            // Ensure the value parses back as a double (must contain '.' or 'e').
            std::string token = tmp.str();
            if (token.find('.') == std::string::npos &&
                token.find('e') == std::string::npos &&
                token.find('n') == std::string::npos /* nan/inf already rejected */) {
                token += ".0";
            }
            out << token;
            break;
        }
        case JsonKind::String: escape_string(out, v.as_string()); break;
        case JsonKind::Array:  write_array (out, v.as_array(),  pretty, depth); break;
        case JsonKind::Object: write_object(out, v.as_object(), pretty, depth); break;
    }
}

}  // namespace

std::string write_json(const JsonValue& value, bool pretty) {
    std::ostringstream out;
    write_value(out, value, pretty, /*depth=*/0);
    if (pretty) out << '\n';
    return out.str();
}

}  // namespace qplayer::detail
