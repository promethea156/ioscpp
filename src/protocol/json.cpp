#include "ioscpp/protocol/json.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ioscpp::protocol
{
namespace
{

Error protocol_error(std::string message)
{
    return Error{ErrorCode::Protocol, std::move(message)};
}

/// Appends `code_point` to `out` as UTF-8.
void append_utf8(std::string &out, std::uint32_t code_point)
{
    if (code_point <= 0x7f)
    {
        out.push_back(static_cast<char>(code_point));
    }
    else if (code_point <= 0x7ff)
    {
        out.push_back(static_cast<char>(0xc0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
    else if (code_point <= 0xffff)
    {
        out.push_back(static_cast<char>(0xe0 | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
    else
    {
        out.push_back(static_cast<char>(0xf0 | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
}

/// Reads one `\uXXXX` escape (the backslash and `u` are already consumed).
Result<std::uint32_t> parse_hex4(std::string_view text, std::size_t &position)
{
    if (position + 4 > text.size())
    {
        return tl::unexpected(protocol_error("a JSON unicode escape is truncated"));
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i)
    {
        const char c = text[position + static_cast<std::size_t>(i)];
        std::uint32_t digit = 0;
        if (c >= '0' && c <= '9')
        {
            digit = static_cast<std::uint32_t>(c - '0');
        }
        else if (c >= 'a' && c <= 'f')
        {
            digit = static_cast<std::uint32_t>(c - 'a' + 10);
        }
        else if (c >= 'A' && c <= 'F')
        {
            digit = static_cast<std::uint32_t>(c - 'A' + 10);
        }
        else
        {
            return tl::unexpected(protocol_error("a JSON unicode escape is not hexadecimal"));
        }
        value = (value << 4) | digit;
    }
    position += 4;
    return value;
}

/// A recursive-descent JSON reader over a `string_view`.
struct Parser
{
    std::string_view text;
    std::size_t position = 0;

    bool at_end() const noexcept
    {
        return position >= text.size();
    }

    char peek() const noexcept
    {
        return at_end() ? '\0' : text[position];
    }

    void skip_whitespace() noexcept
    {
        while (!at_end())
        {
            const char c = text[position];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            {
                ++position;
            }
            else
            {
                break;
            }
        }
    }

    Result<Json> parse_value()
    {
        switch (peek())
        {
            case '{':
                return parse_object();
            case '[':
                return parse_array();
            case '"':
            {
                auto string = parse_string();
                if (!string)
                {
                    return tl::unexpected(string.error());
                }
                return Json(std::move(*string));
            }
            case 't':
            case 'f':
            case 'n':
            {
                const char first = peek();
                const std::string_view literal = first == 't' ? "true" : (first == 'f' ? "false" : "null");
                if (text.substr(position, literal.size()) != literal)
                {
                    return tl::unexpected(protocol_error("a JSON literal is malformed"));
                }
                position += literal.size();
                if (first == 't')
                {
                    return Json(true);
                }
                if (first == 'f')
                {
                    return Json(false);
                }
                return Json();
            }
            default:
                return parse_number();
        }
    }

    Result<std::string> parse_string()
    {
        if (peek() != '"')
        {
            return tl::unexpected(protocol_error("a JSON string does not start with a quote"));
        }
        ++position;
        std::string out;
        while (true)
        {
            if (at_end())
            {
                return tl::unexpected(protocol_error("a JSON string is not terminated"));
            }
            const char c = text[position++];
            if (c == '"')
            {
                return out;
            }
            if (c != '\\')
            {
                out.push_back(c);
                continue;
            }
            if (at_end())
            {
                return tl::unexpected(protocol_error("a JSON escape is not terminated"));
            }
            switch (text[position++])
            {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u':
                {
                    auto high = parse_hex4(text, position);
                    if (!high)
                    {
                        return tl::unexpected(high.error());
                    }
                    std::uint32_t code_point = *high;
                    // A high surrogate is combined with a following low surrogate into
                    // one code point. A lone high surrogate is passed through
                    // unchanged, which encodes as invalid UTF-8.
                    // A following low surrogate is `\uXXXX`, six characters.
                    if (code_point >= 0xd800 && code_point <= 0xdbff && position + 6 <= text.size() &&
                        text[position] == '\\' && text[position + 1] == 'u')
                    {
                        std::size_t low_position = position + 2;
                        auto low = parse_hex4(text, low_position);
                        if (!low)
                        {
                            return tl::unexpected(low.error());
                        }
                        if (*low >= 0xdc00 && *low <= 0xdfff)
                        {
                            code_point = 0x10000 + ((code_point - 0xd800) << 10) + (*low - 0xdc00);
                            position = low_position;
                        }
                    }
                    append_utf8(out, code_point);
                    break;
                }
                default:
                    return tl::unexpected(protocol_error("a JSON string has an unknown escape"));
            }
        }
    }

    Result<Json> parse_number()
    {
        const std::size_t start = position;
        if (peek() == '-')
        {
            ++position;
        }
        while (peek() >= '0' && peek() <= '9')
        {
            ++position;
        }
        if (peek() == '.')
        {
            ++position;
            while (peek() >= '0' && peek() <= '9')
            {
                ++position;
            }
        }
        if (peek() == 'e' || peek() == 'E')
        {
            ++position;
            if (peek() == '+' || peek() == '-')
            {
                ++position;
            }
            while (peek() >= '0' && peek() <= '9')
            {
                ++position;
            }
        }
        const std::string_view token = text.substr(start, position - start);
        double value = 0;
        const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
        if (result.ec != std::errc() || result.ptr != token.data() + token.size())
        {
            return tl::unexpected(protocol_error("a JSON number is malformed"));
        }
        return Json(value);
    }

    Result<Json> parse_object()
    {
        ++position; // the opening brace
        Json::Object object;
        skip_whitespace();
        if (peek() == '}')
        {
            ++position;
            return Json::object(std::move(object));
        }
        while (true)
        {
            skip_whitespace();
            auto key = parse_string();
            if (!key)
            {
                return tl::unexpected(key.error());
            }
            skip_whitespace();
            if (peek() != ':')
            {
                return tl::unexpected(protocol_error("a JSON object member has no colon"));
            }
            ++position;
            skip_whitespace();
            auto value = parse_value();
            if (!value)
            {
                return tl::unexpected(value.error());
            }
            object.emplace(std::move(*key), std::move(*value));
            skip_whitespace();
            if (peek() == ',')
            {
                ++position;
                continue;
            }
            if (peek() == '}')
            {
                ++position;
                break;
            }
            return tl::unexpected(protocol_error("a JSON object is malformed"));
        }
        return Json::object(std::move(object));
    }

    Result<Json> parse_array()
    {
        ++position; // the opening bracket
        Json::Array array;
        skip_whitespace();
        if (peek() == ']')
        {
            ++position;
            return Json::array(std::move(array));
        }
        while (true)
        {
            skip_whitespace();
            auto value = parse_value();
            if (!value)
            {
                return tl::unexpected(value.error());
            }
            array.push_back(std::move(*value));
            skip_whitespace();
            if (peek() == ',')
            {
                ++position;
                continue;
            }
            if (peek() == ']')
            {
                ++position;
                break;
            }
            return tl::unexpected(protocol_error("a JSON array is malformed"));
        }
        return Json::array(std::move(array));
    }
};

void serialize(const Json &value, std::string &out);

void serialize_string(std::string_view value, std::string &out)
{
    out.push_back('"');
    for (const char c : value)
    {
        switch (c)
        {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    char buffer[7];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buffer;
                }
                else
                {
                    out.push_back(c);
                }
                break;
        }
    }
    out.push_back('"');
}

void serialize_number(double value, std::string &out)
{
    // A value with no fraction is written as an integer, so `1280` round-trips as
    // `1280` and not `1280.0`, matching the original text.
    if (std::isfinite(value) && value == std::floor(value) && std::abs(value) < 1e15)
    {
        out += std::to_string(static_cast<std::int64_t>(value));
        return;
    }
    // JSON has no NaN or Infinity, so a non-finite number is written as `null`.
    if (!std::isfinite(value))
    {
        out += "null";
        return;
    }
    char buffer[32];
    const int written = std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    out.append(buffer, static_cast<std::size_t>(written));
}

void serialize(const Json &value, std::string &out)
{
    switch (value.type())
    {
        case JsonType::Null:
            out += "null";
            break;
        case JsonType::Boolean:
            out += value.boolean().value_or(false) ? "true" : "false";
            break;
        case JsonType::Number:
            serialize_number(value.number().value_or(0), out);
            break;
        case JsonType::String:
            serialize_string(value.string().value_or(std::string_view{}), out);
            break;
        case JsonType::Array:
        {
            out.push_back('[');
            bool first = true;
            for (const Json &element : *value.array())
            {
                if (!first)
                {
                    out.push_back(',');
                }
                first = false;
                serialize(element, out);
            }
            out.push_back(']');
            break;
        }
        case JsonType::Object:
        {
            out.push_back('{');
            bool first = true;
            for (const auto &[key, member] : *value.object())
            {
                if (!first)
                {
                    out.push_back(',');
                }
                first = false;
                serialize_string(key, out);
                out.push_back(':');
                serialize(member, out);
            }
            out.push_back('}');
            break;
        }
    }
}

} // namespace

Json::Json(bool value)
    : value_(value)
{
}

Json::Json(int value)
    : value_(static_cast<double>(value))
{
}

Json::Json(std::int64_t value)
    : value_(static_cast<double>(value))
{
}

Json::Json(double value)
    : value_(value)
{
}

Json::Json(std::string value)
    : value_(std::move(value))
{
}

Json::Json(const char *value)
    : value_(std::string(value))
{
}

Json Json::array(Array values)
{
    Json json;
    json.value_ = std::move(values);
    return json;
}

Json Json::object(Object values)
{
    Json json;
    json.value_ = std::move(values);
    return json;
}

JsonType Json::type() const noexcept
{
    switch (value_.index())
    {
        case 0:
            return JsonType::Null;
        case 1:
            return JsonType::Boolean;
        case 2:
            return JsonType::Number;
        case 3:
            return JsonType::String;
        case 4:
            return JsonType::Array;
        default:
            return JsonType::Object;
    }
}

bool Json::is_null() const noexcept
{
    return value_.index() == 0;
}

bool Json::is_boolean() const noexcept
{
    return value_.index() == 1;
}

bool Json::is_number() const noexcept
{
    return value_.index() == 2;
}

bool Json::is_string() const noexcept
{
    return value_.index() == 3;
}

bool Json::is_array() const noexcept
{
    return value_.index() == 4;
}

bool Json::is_object() const noexcept
{
    return value_.index() == 5;
}

std::optional<bool> Json::boolean() const noexcept
{
    if (const bool *value = std::get_if<bool>(&value_))
    {
        return *value;
    }
    return std::nullopt;
}

std::optional<double> Json::number() const noexcept
{
    if (const double *value = std::get_if<double>(&value_))
    {
        return *value;
    }
    return std::nullopt;
}

std::optional<std::string_view> Json::string() const noexcept
{
    if (const std::string *value = std::get_if<std::string>(&value_))
    {
        return *value;
    }
    return std::nullopt;
}

const Json::Array *Json::array() const noexcept
{
    return std::get_if<Array>(&value_);
}

const Json::Object *Json::object() const noexcept
{
    return std::get_if<Object>(&value_);
}

const Json *Json::find(std::string_view key) const noexcept
{
    const Object *object = this->object();
    if (object == nullptr)
    {
        return nullptr;
    }
    const auto found = object->find(key);
    return found == object->end() ? nullptr : &found->second;
}

std::string Json::to_string() const
{
    std::string out;
    serialize(*this, out);
    return out;
}

Result<Json> Json::parse(std::string_view text)
{
    Parser parser{text, 0};
    parser.skip_whitespace();
    auto value = parser.parse_value();
    if (!value)
    {
        return tl::unexpected(value.error());
    }
    parser.skip_whitespace();
    if (!parser.at_end())
    {
        return tl::unexpected(protocol_error("a JSON document has trailing text"));
    }
    return value;
}

} // namespace ioscpp::protocol
