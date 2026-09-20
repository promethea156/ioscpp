#include "ioscpp/protocol/plist.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ioscpp::protocol
{
namespace
{

Error protocol_error(std::string message)
{
    return Error{ErrorCode::Protocol, std::move(message)};
}

// ---------------------------------------------------------------------------
// Base64
// ---------------------------------------------------------------------------

constexpr std::string_view kBase64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(std::span<const std::byte> data)
{
    std::string out;
    // Four output characters per three input bytes, rounded up.
    out.reserve(((data.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= data.size())
    {
        const std::uint32_t triple = (static_cast<std::uint32_t>(data[i]) << 16) |
                                     (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                                     static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(kBase64Alphabet[(triple >> 18) & 0x3f]);
        out.push_back(kBase64Alphabet[(triple >> 12) & 0x3f]);
        out.push_back(kBase64Alphabet[(triple >> 6) & 0x3f]);
        out.push_back(kBase64Alphabet[triple & 0x3f]);
        i += 3;
    }
    if (i + 1 == data.size())
    {
        const std::uint32_t triple = static_cast<std::uint32_t>(data[i]) << 16;
        out.push_back(kBase64Alphabet[(triple >> 18) & 0x3f]);
        out.push_back(kBase64Alphabet[(triple >> 12) & 0x3f]);
        out.push_back('=');
        out.push_back('=');
    }
    else if (i + 2 == data.size())
    {
        const std::uint32_t triple =
            (static_cast<std::uint32_t>(data[i]) << 16) | (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out.push_back(kBase64Alphabet[(triple >> 18) & 0x3f]);
        out.push_back(kBase64Alphabet[(triple >> 12) & 0x3f]);
        out.push_back(kBase64Alphabet[(triple >> 6) & 0x3f]);
        out.push_back('=');
    }
    return out;
}

Result<std::vector<std::byte>> base64_decode(std::string_view text)
{
    std::array<int, 256> table{};
    table.fill(-1);
    for (int i = 0; i < 64; ++i)
    {
        table[static_cast<unsigned char>(kBase64Alphabet[static_cast<std::size_t>(i)])] = i;
    }

    std::vector<std::byte> out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char character : text)
    {
        if (character == '=')
        {
            break;
        }
        if (std::isspace(static_cast<unsigned char>(character)) != 0)
        {
            continue;
        }
        const int value = table[static_cast<unsigned char>(character)];
        if (value < 0)
        {
            return tl::unexpected(protocol_error("the data is not valid base64"));
        }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<std::byte>((buffer >> bits) & 0xff));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// XML escaping and dates
// ---------------------------------------------------------------------------

std::string xml_escape(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char character : text)
    {
        switch (character)
        {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            default:
                out.push_back(character);
                break;
        }
    }
    return out;
}

std::string xml_unescape(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '&' && i + 1 < text.size())
        {
            if (text.substr(i).starts_with("&amp;"))
            {
                out.push_back('&');
                i += 4;
                continue;
            }
            if (text.substr(i).starts_with("&lt;"))
            {
                out.push_back('<');
                i += 3;
                continue;
            }
            if (text.substr(i).starts_with("&gt;"))
            {
                out.push_back('>');
                i += 3;
                continue;
            }
            if (text.substr(i).starts_with("&quot;"))
            {
                out.push_back('"');
                i += 5;
                continue;
            }
            if (text.substr(i).starts_with("&apos;"))
            {
                out.push_back('\'');
                i += 5;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

/// Days from 1970-01-01 to `y-m-d`, from Howard Hinnant's `days_from_civil`.
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept
{
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

/// The civil date for a count of days from 1970-01-01.
void civil_from_days(std::int64_t z, std::int64_t &y, unsigned &m, unsigned &d) noexcept
{
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
}

/// The plist epoch, 2001-01-01T00:00:00Z, in seconds from the Unix epoch.
constexpr std::int64_t kPlistEpochSeconds = 978307200;
constexpr std::int64_t kSecondsPerDay = 86400;

std::string format_date(std::int64_t seconds_since_2001)
{
    const std::int64_t seconds = seconds_since_2001 + kPlistEpochSeconds;
    // Integer division truncates toward zero, so a negative time is shifted down
    // by almost a day to get the floor the civil-date conversion expects.
    const std::int64_t days =
        seconds >= 0 ? seconds / kSecondsPerDay : (seconds - (kSecondsPerDay - 1)) / kSecondsPerDay;
    std::int64_t remainder = seconds - days * kSecondsPerDay;
    if (remainder < 0)
    {
        remainder += kSecondsPerDay;
    }

    std::int64_t year = 0;
    unsigned month = 0;
    unsigned day = 0;
    civil_from_days(days, year, month, day);

    const int hour = static_cast<int>(remainder / 3600);
    const int minute = static_cast<int>((remainder % 3600) / 60);
    const int second = static_cast<int>(remainder % 60);

    std::array<char, 32> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%04lld-%02u-%02uT%02d:%02d:%02dZ", static_cast<long long>(year), month,
                  day, hour, minute, second);
    return std::string(buffer.data());
}

Result<std::int64_t> parse_date(std::string_view text)
{
    if (text.size() < 19)
    {
        return tl::unexpected(protocol_error("the date is too short"));
    }
    auto number = [&](std::size_t offset, std::size_t count) -> std::int64_t
    {
        std::int64_t value = 0;
        for (std::size_t i = 0; i < count; ++i)
        {
            const char character = text[offset + i];
            if (character < '0' || character > '9')
            {
                return -1;
            }
            value = value * 10 + (character - '0');
        }
        return value;
    };

    const std::int64_t year = number(0, 4);
    const std::int64_t month = number(5, 2);
    const std::int64_t day = number(8, 2);
    const std::int64_t hour = number(11, 2);
    const std::int64_t minute = number(14, 2);
    const std::int64_t second = number(17, 2);
    if (year < 0 || month < 0 || day < 0 || hour < 0 || minute < 0 || second < 0)
    {
        return tl::unexpected(protocol_error("the date has a non-digit"));
    }

    const std::int64_t days = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    return days * 86400 + hour * 3600 + minute * 60 + second - kPlistEpochSeconds;
}

// ---------------------------------------------------------------------------
// XML serialization
// ---------------------------------------------------------------------------

void append_xml(const Plist &value, std::string &out)
{
    switch (value.type())
    {
        case PlistType::Null:
            out += "<null/>";
            break;
        case PlistType::Boolean:
            out += value.boolean().value() ? "<true/>" : "<false/>";
            break;
        case PlistType::Integer:
            out += "<integer>";
            out += std::to_string(value.integer().value());
            out += "</integer>";
            break;
        case PlistType::Real:
        {
            std::array<char, 32> buffer{};
            std::snprintf(buffer.data(), buffer.size(), "%.17g", value.real().value());
            out += "<real>";
            out += buffer.data();
            out += "</real>";
            break;
        }
        case PlistType::String:
            out += "<string>";
            out += xml_escape(value.string().value());
            out += "</string>";
            break;
        case PlistType::Data:
            out += "<data>";
            out += base64_encode(value.data().value());
            out += "</data>";
            break;
        case PlistType::Date:
            out += "<date>";
            out += format_date(value.date().value().seconds_since_2001);
            out += "</date>";
            break;
        case PlistType::Array:
            out += "<array>";
            for (const Plist &item : *value.array())
            {
                append_xml(item, out);
            }
            out += "</array>";
            break;
        case PlistType::Dictionary:
            out += "<dict>";
            for (const auto &[key, item] : *value.dictionary())
            {
                out += "<key>";
                out += xml_escape(key);
                out += "</key>";
                append_xml(item, out);
            }
            out += "</dict>";
            break;
        case PlistType::Uid:
            out += "<integer>";
            out += std::to_string(value.uid().value());
            out += "</integer>";
            break;
    }
}

// ---------------------------------------------------------------------------
// XML parsing
// ---------------------------------------------------------------------------

class XmlParser
{
public:
    explicit XmlParser(std::string_view text)
        : text_(text)
    {
    }

    Result<Plist> parse()
    {
        skip_prolog();
        if (!consume("<plist"))
        {
            return tl::unexpected(protocol_error("the plist has no <plist> element"));
        }
        skip_to('>');
        auto value = parse_value();
        if (!value)
        {
            return tl::unexpected(value.error());
        }
        return value;
    }

private:
    void skip_space()
    {
        while (position_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[position_])) != 0)
        {
            ++position_;
        }
    }

    void skip_prolog()
    {
        for (;;)
        {
            skip_space();
            if (text_.substr(position_).starts_with("<?"))
            {
                skip_to('>');
            }
            else if (text_.substr(position_).starts_with("<!--"))
            {
                const std::size_t end = text_.find("-->", position_);
                position_ = end == std::string_view::npos ? text_.size() : end + 3;
            }
            else if (text_.substr(position_).starts_with("<!"))
            {
                skip_to('>');
            }
            else
            {
                return;
            }
        }
    }

    /// Advances past the next `character`, or to the end when it is absent.
    void skip_to(char character)
    {
        const std::size_t end = text_.find(character, position_);
        position_ = end == std::string_view::npos ? text_.size() : end + 1;
    }

    bool consume(std::string_view token)
    {
        skip_space();
        if (text_.substr(position_).starts_with(token))
        {
            position_ += token.size();
            return true;
        }
        return false;
    }

    /// Parses a self-closing or paired element's body and consumes its close tag.
    Result<std::string> read_body(std::string_view name)
    {
        if (consume("/>"))
        {
            return std::string();
        }
        if (!consume(">"))
        {
            return tl::unexpected(protocol_error("a plist element is malformed"));
        }
        const std::size_t start = position_;
        const std::string close = "</" + std::string(name) + ">";
        const std::size_t end = text_.find(close, position_);
        if (end == std::string_view::npos)
        {
            return tl::unexpected(protocol_error("a plist element is not closed"));
        }
        std::string body(text_.substr(start, end - start));
        position_ = end + close.size();
        return body;
    }

    Result<Plist> parse_value()
    {
        skip_space();
        if (position_ >= text_.size() || text_[position_] != '<')
        {
            return tl::unexpected(protocol_error("a plist value does not start with an element"));
        }

        if (consume("<true/>"))
        {
            return Plist(true);
        }
        if (consume("<false/>"))
        {
            return Plist(false);
        }
        if (consume("<null/>"))
        {
            return Plist();
        }

        auto body_of = [&](std::string_view name) -> Result<std::string>
        {
            if (!consume("<" + std::string(name)))
            {
                return tl::unexpected(protocol_error("a plist element has the wrong name"));
            }
            return read_body(name);
        };

        if (text_.substr(position_).starts_with("<integer"))
        {
            auto body = body_of("integer");
            if (!body)
            {
                return tl::unexpected(body.error());
            }
            return Plist(static_cast<std::int64_t>(std::strtoll(body->c_str(), nullptr, 10)));
        }
        if (text_.substr(position_).starts_with("<real"))
        {
            auto body = body_of("real");
            if (!body)
            {
                return tl::unexpected(body.error());
            }
            return Plist(std::strtod(body->c_str(), nullptr));
        }
        if (text_.substr(position_).starts_with("<string"))
        {
            auto body = body_of("string");
            if (!body)
            {
                return tl::unexpected(body.error());
            }
            return Plist(xml_unescape(*body));
        }
        if (text_.substr(position_).starts_with("<data"))
        {
            auto body = body_of("data");
            if (!body)
            {
                return tl::unexpected(body.error());
            }
            // A data body may contain whitespace between base64 lines.
            std::string compact;
            for (const char character : *body)
            {
                if (std::isspace(static_cast<unsigned char>(character)) == 0)
                {
                    compact.push_back(character);
                }
            }
            auto decoded = base64_decode(compact);
            if (!decoded)
            {
                return tl::unexpected(decoded.error());
            }
            return Plist(std::move(*decoded));
        }
        if (text_.substr(position_).starts_with("<date"))
        {
            auto body = body_of("date");
            if (!body)
            {
                return tl::unexpected(body.error());
            }
            auto seconds = parse_date(*body);
            if (!seconds)
            {
                return tl::unexpected(seconds.error());
            }
            return Plist::date(*seconds);
        }
        if (text_.substr(position_).starts_with("<array"))
        {
            if (!consume("<array"))
            {
                return tl::unexpected(protocol_error("the array is malformed"));
            }
            if (!consume(">"))
            {
                return tl::unexpected(protocol_error("the array is malformed"));
            }
            Plist::Array items;
            for (;;)
            {
                skip_space();
                if (consume("</array>"))
                {
                    break;
                }
                auto item = parse_value();
                if (!item)
                {
                    return tl::unexpected(item.error());
                }
                items.push_back(std::move(*item));
            }
            return Plist::array(std::move(items));
        }
        if (text_.substr(position_).starts_with("<dict"))
        {
            if (!consume("<dict"))
            {
                return tl::unexpected(protocol_error("the dictionary is malformed"));
            }
            if (!consume(">"))
            {
                return tl::unexpected(protocol_error("the dictionary is malformed"));
            }
            Plist::Dictionary items;
            for (;;)
            {
                skip_space();
                if (consume("</dict>"))
                {
                    break;
                }
                auto key = body_of("key");
                if (!key)
                {
                    return tl::unexpected(key.error());
                }
                auto item = parse_value();
                if (!item)
                {
                    return tl::unexpected(item.error());
                }
                items.emplace(xml_unescape(*key), std::move(*item));
            }
            return Plist::dictionary(std::move(items));
        }

        return tl::unexpected(protocol_error("the plist element is not known"));
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

// ---------------------------------------------------------------------------
// Binary plist
// ---------------------------------------------------------------------------

// A binary plist object starts with a one-byte marker. For most kinds the high
// nibble is the kind and the low nibble is a count, an exponent of the byte count
// for the fixed-width kinds, or `kMarkerCountFollows` when the real count follows
// as an integer object. The null, false, and true markers are whole bytes with no
// such split. The constants below are the markers this codec reads and writes.
constexpr std::uint8_t kMarkerNull = 0x00;
constexpr std::uint8_t kMarkerFalse = 0x08;
constexpr std::uint8_t kMarkerTrue = 0x09;
constexpr std::uint8_t kMarkerInt = 0x10;
constexpr std::uint8_t kMarkerReal = 0x20;
constexpr std::uint8_t kMarkerDate = 0x30;
constexpr std::uint8_t kMarkerData = 0x40;
constexpr std::uint8_t kMarkerAscii = 0x50;
constexpr std::uint8_t kMarkerUtf16 = 0x60;
constexpr std::uint8_t kMarkerUid = 0x80;
constexpr std::uint8_t kMarkerArray = 0xa0;
constexpr std::uint8_t kMarkerDictionary = 0xd0;
// The low nibble that says the count did not fit and follows as an int object.
constexpr std::uint8_t kMarkerCountFollows = 0x0f;

// The 32-byte trailer that ends a binary plist: six unused bytes, then the offset
// table's entry size, the reference size, the object count, the top object, and the
// offset table's offset, the last three as 8-byte big-endian integers.
constexpr std::size_t kTrailerSize = 32;
constexpr std::size_t kTrailerOffsetSizeIndex = 6;
constexpr std::size_t kTrailerRefSizeIndex = 7;

void put_be(std::vector<std::byte> &out, std::uint64_t value, std::size_t size)
{
    for (std::size_t i = 0; i < size; ++i)
    {
        out.push_back(static_cast<std::byte>((value >> (8 * (size - 1 - i))) & 0xff));
    }
}

/// Emits an integer object with the smallest size that holds `value`.
///
/// The low nibble of an int marker is the base-2 exponent of the byte count, so
/// the sizes are `kMarkerInt` (1 byte), `kMarkerInt | 1` (2), `kMarkerInt | 2`
/// (4), and `kMarkerInt | 3` (8).
void append_int(std::vector<std::byte> &out, std::int64_t value)
{
    if (value >= 0)
    {
        if (value <= 0xff)
        {
            out.push_back(std::byte{kMarkerInt});
            put_be(out, static_cast<std::uint64_t>(value), 1);
        }
        else if (value <= 0xffff)
        {
            out.push_back(std::byte{kMarkerInt | 1});
            put_be(out, static_cast<std::uint64_t>(value), 2);
        }
        else if (value <= 0xffffffffLL)
        {
            out.push_back(std::byte{kMarkerInt | 2});
            put_be(out, static_cast<std::uint64_t>(value), 4);
        }
        else
        {
            out.push_back(std::byte{kMarkerInt | 3});
            put_be(out, static_cast<std::uint64_t>(value), 8);
        }
        return;
    }

    if (value >= -128)
    {
        out.push_back(std::byte{kMarkerInt});
        put_be(out, static_cast<std::uint64_t>(static_cast<std::uint8_t>(value)), 1);
    }
    else if (value >= -32768)
    {
        out.push_back(std::byte{kMarkerInt | 1});
        put_be(out, static_cast<std::uint64_t>(static_cast<std::uint16_t>(value)), 2);
    }
    else if (value >= -2147483648LL)
    {
        out.push_back(std::byte{kMarkerInt | 2});
        put_be(out, static_cast<std::uint64_t>(static_cast<std::uint32_t>(value)), 4);
    }
    else
    {
        out.push_back(std::byte{kMarkerInt | 3});
        put_be(out, static_cast<std::uint64_t>(value), 8);
    }
}

/// Emits a length or count, inline for small values and as an int object otherwise.
void append_count(std::vector<std::byte> &out, std::uint8_t marker, std::uint64_t count)
{
    // A count below the `kMarkerCountFollows` sentinel fits in the low nibble;
    // the sentinel means the count follows as an int object.
    if (count < kMarkerCountFollows)
    {
        out.push_back(static_cast<std::byte>(marker | static_cast<std::uint8_t>(count)));
        return;
    }
    out.push_back(static_cast<std::byte>(marker | kMarkerCountFollows));
    append_int(out, static_cast<std::int64_t>(count));
}

/// Assigns every object a slot, appending the flattened order to `objects`.
///
/// Dictionary keys are objects too, so each key gets a string object of its own,
/// recorded in `key_slots`. The key objects are owned by `owned`, which keeps
/// their addresses stable while `objects` points at them.
void flatten(const Plist &value, std::vector<const Plist *> &objects, std::map<const Plist *, std::size_t> &slots,
             std::map<std::string, std::size_t, std::less<>> &key_slots, std::vector<std::unique_ptr<Plist>> &owned)
{
    if (slots.contains(&value))
    {
        return;
    }
    const std::size_t slot = objects.size();
    objects.push_back(&value);
    slots.emplace(&value, slot);

    if (value.type() == PlistType::Array)
    {
        for (const Plist &item : *value.array())
        {
            flatten(item, objects, slots, key_slots, owned);
        }
    }
    else if (value.type() == PlistType::Dictionary)
    {
        for (const auto &entry : *value.dictionary())
        {
            if (!key_slots.contains(entry.first))
            {
                auto key_object = std::make_unique<Plist>(entry.first);
                key_slots.emplace(entry.first, objects.size());
                flatten(*key_object, objects, slots, key_slots, owned);
                owned.push_back(std::move(key_object));
            }
        }
        for (const auto &entry : *value.dictionary())
        {
            flatten(entry.second, objects, slots, key_slots, owned);
        }
    }
}

void append_object(const Plist &value, const std::map<const Plist *, std::size_t> &slots,
                   const std::map<std::string, std::size_t, std::less<>> &key_slots, std::size_t ref_size,
                   std::vector<std::byte> &out)
{
    switch (value.type())
    {
        case PlistType::Null:
            out.push_back(std::byte{kMarkerNull});
            break;
        case PlistType::Boolean:
            out.push_back(static_cast<std::byte>(value.boolean().value() ? kMarkerTrue : kMarkerFalse));
            break;
        case PlistType::Integer:
            append_int(out, value.integer().value());
            break;
        case PlistType::Real:
        {
            std::uint64_t bits = 0;
            const double real = value.real().value();
            std::memcpy(&bits, &real, sizeof(bits));
            // A real is always 8 bytes, so the low nibble is the size exponent 3.
            out.push_back(std::byte{kMarkerReal | 3});
            put_be(out, bits, 8);
            break;
        }
        case PlistType::Date:
        {
            std::uint64_t bits = 0;
            const double real = static_cast<double>(value.date().value().seconds_since_2001);
            std::memcpy(&bits, &real, sizeof(bits));
            // A date is a real of seconds since 2001, so it is 8 bytes too.
            out.push_back(std::byte{kMarkerDate | 3});
            put_be(out, bits, 8);
            break;
        }
        case PlistType::String:
        {
            const std::string_view text = value.string().value();
            append_count(out, kMarkerAscii, text.size());
            out.insert(out.end(), reinterpret_cast<const std::byte *>(text.data()),
                       reinterpret_cast<const std::byte *>(text.data() + text.size()));
            break;
        }
        case PlistType::Data:
        {
            const std::span<const std::byte> data = value.data().value();
            append_count(out, kMarkerData, data.size());
            out.insert(out.end(), data.begin(), data.end());
            break;
        }
        case PlistType::Array:
        {
            const Plist::Array &items = *value.array();
            append_count(out, kMarkerArray, items.size());
            // `flatten` placed every item in `slots`, so the slot is present.
            for (const Plist &item : items)
            {
                put_be(out, slots.at(&item), ref_size);
            }
            break;
        }
        case PlistType::Dictionary:
        {
            const Plist::Dictionary &items = *value.dictionary();
            append_count(out, kMarkerDictionary, items.size());
            // A dictionary is its keys and then its values, each in key order, and
            // `flatten` placed every one of them, so each slot is present.
            for (const auto &entry : items)
            {
                put_be(out, key_slots.at(entry.first), ref_size);
            }
            for (const auto &entry : items)
            {
                put_be(out, slots.at(&entry.second), ref_size);
            }
            break;
        }
        case PlistType::Uid:
        {
            // The low nibble of a UID marker is the exponent of its byte count.
            const std::uint64_t index = value.uid().value();
            if (index <= 0xff)
            {
                out.push_back(std::byte{kMarkerUid});
                put_be(out, index, 1);
            }
            else if (index <= 0xffff)
            {
                out.push_back(std::byte{kMarkerUid | 1});
                put_be(out, index, 2);
            }
            else if (index <= 0xffffffffULL)
            {
                out.push_back(std::byte{kMarkerUid | 2});
                put_be(out, index, 4);
            }
            else
            {
                out.push_back(std::byte{kMarkerUid | 3});
                put_be(out, index, 8);
            }
            break;
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Plist
// ---------------------------------------------------------------------------

Plist::Plist(bool value)
    : value_(value)
{
}

Plist::Plist(int value)
    : value_(static_cast<std::int64_t>(value))
{
}

Plist::Plist(std::int64_t value)
    : value_(value)
{
}

Plist::Plist(double value)
    : value_(value)
{
}

Plist::Plist(std::string value)
    : value_(std::move(value))
{
}

Plist::Plist(const char *value)
    : value_(std::string(value))
{
}

Plist::Plist(std::string_view value)
    : value_(std::string(value))
{
}

Plist::Plist(std::vector<std::byte> value)
    : value_(std::move(value))
{
}

Plist Plist::date(std::int64_t seconds_since_2001)
{
    Plist plist;
    plist.value_ = Date{seconds_since_2001};
    return plist;
}

Plist Plist::array(Array values)
{
    Plist plist;
    plist.value_ = std::move(values);
    return plist;
}

Plist Plist::dictionary(Dictionary values)
{
    Plist plist;
    plist.value_ = std::move(values);
    return plist;
}

Plist Plist::uid(std::uint64_t value)
{
    Plist plist;
    plist.value_ = value;
    return plist;
}

PlistType Plist::type() const noexcept
{
    switch (value_.index())
    {
        case 0:
            return PlistType::Null;
        case 1:
            return PlistType::Boolean;
        case 2:
            return PlistType::Integer;
        case 3:
            return PlistType::Real;
        case 4:
            return PlistType::String;
        case 5:
            return PlistType::Data;
        case 6:
            return PlistType::Date;
        case 7:
            return PlistType::Array;
        case 8:
            return PlistType::Dictionary;
        default:
            return PlistType::Uid;
    }
}

bool Plist::is_null() const noexcept
{
    return std::holds_alternative<std::monostate>(value_);
}
bool Plist::is_boolean() const noexcept
{
    return std::holds_alternative<bool>(value_);
}
bool Plist::is_integer() const noexcept
{
    return std::holds_alternative<std::int64_t>(value_);
}
bool Plist::is_real() const noexcept
{
    return std::holds_alternative<double>(value_);
}
bool Plist::is_string() const noexcept
{
    return std::holds_alternative<std::string>(value_);
}
bool Plist::is_data() const noexcept
{
    return std::holds_alternative<std::vector<std::byte>>(value_);
}
bool Plist::is_date() const noexcept
{
    return std::holds_alternative<Date>(value_);
}
bool Plist::is_array() const noexcept
{
    return std::holds_alternative<Array>(value_);
}
bool Plist::is_dictionary() const noexcept
{
    return std::holds_alternative<Dictionary>(value_);
}
bool Plist::is_uid() const noexcept
{
    return std::holds_alternative<std::uint64_t>(value_);
}

std::optional<bool> Plist::boolean() const noexcept
{
    const bool *value = std::get_if<bool>(&value_);
    return value == nullptr ? std::nullopt : std::optional<bool>(*value);
}

std::optional<std::int64_t> Plist::integer() const noexcept
{
    const std::int64_t *value = std::get_if<std::int64_t>(&value_);
    return value == nullptr ? std::nullopt : std::optional<std::int64_t>(*value);
}

std::optional<double> Plist::real() const noexcept
{
    const double *value = std::get_if<double>(&value_);
    return value == nullptr ? std::nullopt : std::optional<double>(*value);
}

std::optional<std::string_view> Plist::string() const noexcept
{
    const std::string *value = std::get_if<std::string>(&value_);
    return value == nullptr ? std::nullopt : std::optional<std::string_view>(*value);
}

std::optional<std::span<const std::byte>> Plist::data() const noexcept
{
    const std::vector<std::byte> *value = std::get_if<std::vector<std::byte>>(&value_);
    return value == nullptr ? std::nullopt : std::optional<std::span<const std::byte>>(*value);
}

std::optional<Date> Plist::date() const noexcept
{
    const Date *value = std::get_if<Date>(&value_);
    return value == nullptr ? std::nullopt : std::optional<Date>(*value);
}

const Plist::Array *Plist::array() const noexcept
{
    return std::get_if<Array>(&value_);
}

const Plist::Dictionary *Plist::dictionary() const noexcept
{
    return std::get_if<Dictionary>(&value_);
}

std::optional<std::uint64_t> Plist::uid() const noexcept
{
    const std::uint64_t *value = std::get_if<std::uint64_t>(&value_);
    return value == nullptr ? std::nullopt : std::optional<std::uint64_t>(*value);
}

std::string Plist::string_or(std::string_view fallback) const
{
    const std::optional<std::string_view> value = string();
    return value.has_value() ? std::string(*value) : std::string(fallback);
}

const Plist *Plist::find(std::string_view key) const noexcept
{
    const Dictionary *items = dictionary();
    if (items == nullptr)
    {
        return nullptr;
    }
    const auto it = items->find(key);
    return it == items->end() ? nullptr : &it->second;
}

std::string Plist::to_xml() const
{
    std::string out =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">";
    append_xml(*this, out);
    out += "</plist>\n";
    return out;
}

std::vector<std::byte> Plist::to_binary() const
{
    std::vector<const Plist *> objects;
    std::map<const Plist *, std::size_t> slots;
    std::map<std::string, std::size_t, std::less<>> key_slots;
    std::vector<std::unique_ptr<Plist>> owned;
    flatten(*this, objects, slots, key_slots, owned);

    std::size_t ref_size = 1;
    while (ref_size < 8 && (objects.size() >> (8 * ref_size)) != 0)
    {
        ++ref_size;
    }

    std::vector<std::byte> out;
    constexpr std::array<std::byte, 8> kHeader{std::byte{'b'}, std::byte{'p'}, std::byte{'l'}, std::byte{'i'},
                                               std::byte{'s'}, std::byte{'t'}, std::byte{'0'}, std::byte{'0'}};
    out.insert(out.end(), kHeader.begin(), kHeader.end());

    std::vector<std::uint64_t> offsets;
    offsets.reserve(objects.size());
    for (const Plist *object : objects)
    {
        offsets.push_back(out.size());
        append_object(*object, slots, key_slots, ref_size, out);
    }

    std::size_t offset_size = 1;
    const std::size_t table_offset = out.size();
    while (offset_size < 8 && (table_offset >> (8 * offset_size)) != 0)
    {
        ++offset_size;
    }
    for (const std::uint64_t offset : offsets)
    {
        put_be(out, offset, offset_size);
    }

    for (std::size_t i = 0; i < kTrailerOffsetSizeIndex; ++i)
    {
        out.push_back(std::byte{0});
    }
    out.push_back(static_cast<std::byte>(offset_size));
    out.push_back(static_cast<std::byte>(ref_size));
    put_be(out, objects.size(), 8);
    put_be(out, slots.at(this), 8);
    put_be(out, table_offset, 8);
    return out;
}

Result<Plist> Plist::parse(std::span<const std::byte> bytes)
{
    if (bytes.size() >= 8 && std::memcmp(bytes.data(), "bplist00", 8) == 0)
    {
        return parse_binary(bytes);
    }
    return parse_xml(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
}

Result<Plist> Plist::parse_xml(std::string_view xml)
{
    XmlParser parser(xml);
    return parser.parse();
}

Result<Plist> Plist::parse_binary(std::span<const std::byte> bytes)
{
    if (bytes.size() < 8 + kTrailerSize || std::memcmp(bytes.data(), "bplist00", 8) != 0)
    {
        return tl::unexpected(protocol_error("the binary plist header is missing"));
    }

    const std::span<const std::byte> trailer = bytes.subspan(bytes.size() - kTrailerSize);
    auto read_be = [](std::span<const std::byte> source, std::size_t offset, std::size_t size) -> std::uint64_t
    {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < size; ++i)
        {
            value = (value << 8) | static_cast<std::uint64_t>(source[offset + i]);
        }
        return value;
    };

    const std::size_t offset_size = static_cast<std::size_t>(trailer[kTrailerOffsetSizeIndex]);
    const std::size_t ref_size = static_cast<std::size_t>(trailer[kTrailerRefSizeIndex]);
    // The last three fields are the object count, the top object, and the offset
    // table's offset, each 8 bytes big-endian.
    const std::uint64_t object_count = read_be(trailer, 8, 8);
    const std::uint64_t top_object = read_be(trailer, 16, 8);
    const std::uint64_t table_offset = read_be(trailer, 24, 8);
    if (offset_size == 0 || ref_size == 0 || object_count == 0 ||
        table_offset + object_count * offset_size > bytes.size())
    {
        return tl::unexpected(protocol_error("the binary plist trailer is malformed"));
    }

    std::vector<std::uint64_t> offsets(object_count);
    for (std::size_t i = 0; i < object_count; ++i)
    {
        offsets[i] = read_be(bytes, static_cast<std::size_t>(table_offset) + i * offset_size, offset_size);
    }

    std::vector<std::optional<Plist>> cache(object_count);
    std::vector<bool> active(object_count, false);

    // A recursive member function needs a named type, because a lambda cannot
    // refer to itself.
    struct Decoder
    {
        std::span<const std::byte> bytes;
        const std::vector<std::uint64_t> &offsets;
        std::vector<std::optional<Plist>> &cache;
        std::vector<bool> &active;
        std::size_t ref_size;

        Result<Plist> decode(std::size_t index)
        {
            if (index >= offsets.size())
            {
                return tl::unexpected(protocol_error("a binary plist object reference is out of range"));
            }
            if (cache[index].has_value())
            {
                return *cache[index];
            }
            if (active[index])
            {
                return tl::unexpected(protocol_error("a binary plist object is recursive"));
            }
            active[index] = true;

            auto result = decode_uncached(index);
            active[index] = false;
            if (result)
            {
                cache[index] = *result;
            }
            return result;
        }

        Result<Plist> decode_uncached(std::size_t index)
        {
            std::size_t position = static_cast<std::size_t>(offsets[index]);
            if (position >= bytes.size())
            {
                return tl::unexpected(protocol_error("a binary plist object offset is out of range"));
            }

            const std::uint8_t marker = static_cast<std::uint8_t>(bytes[position++]);
            // The three singletons carry no count or size, so they are matched whole.
            if (marker == kMarkerFalse)
            {
                return Plist(false);
            }
            if (marker == kMarkerTrue)
            {
                return Plist(true);
            }
            if (marker == kMarkerNull)
            {
                return Plist();
            }
            const std::uint8_t kind = marker & 0xf0;
            const std::uint8_t info = marker & 0x0f;

            auto read_uint = [&](std::size_t size) -> std::uint64_t
            {
                std::uint64_t value = 0;
                for (std::size_t i = 0; i < size; ++i)
                {
                    value = (value << 8) | static_cast<std::uint64_t>(bytes[position + i]);
                }
                position += size;
                return value;
            };

            auto read_count = [&]() -> std::uint64_t
            {
                if (info != kMarkerCountFollows)
                {
                    return info;
                }
                // The count follows as an int object, whose low nibble is the
                // base-2 exponent of its byte count.
                const std::uint8_t int_marker = static_cast<std::uint8_t>(bytes[position++]);
                return read_uint(std::size_t{1} << (int_marker & 0x0f));
            };

            // The kind is the marker's high nibble, and each case reads the body.
            switch (kind)
            {
                case kMarkerNull:
                    return Plist();
                case kMarkerInt:
                {
                    const std::size_t size = std::size_t{1} << info;
                    const std::uint64_t raw = read_uint(size);
                    std::int64_t value = 0;
                    // A value whose high bit is set is negative, so sign-extend
                    // it to the full 64-bit width.
                    if (size < 8 && (raw & (std::uint64_t{1} << (size * 8 - 1))) != 0)
                    {
                        value = static_cast<std::int64_t>(raw | (~std::uint64_t{0} << (size * 8)));
                    }
                    else
                    {
                        value = static_cast<std::int64_t>(raw);
                    }
                    return Plist(value);
                }
                case kMarkerReal:
                {
                    const std::size_t size = std::size_t{1} << info;
                    const std::uint64_t raw = read_uint(size);
                    double value = 0;
                    if (size == 8)
                    {
                        std::memcpy(&value, &raw, sizeof(value));
                    }
                    else if (size == 4)
                    {
                        const float small = static_cast<float>(raw);
                        value = small;
                    }
                    return Plist(value);
                }
                case kMarkerDate:
                {
                    const std::uint64_t raw = read_uint(8);
                    double value = 0;
                    std::memcpy(&value, &raw, sizeof(value));
                    return Plist::date(static_cast<std::int64_t>(value));
                }
                case kMarkerData:
                {
                    const std::size_t count = static_cast<std::size_t>(read_count());
                    std::vector<std::byte> data(bytes.begin() + static_cast<std::ptrdiff_t>(position),
                                                bytes.begin() + static_cast<std::ptrdiff_t>(position + count));
                    return Plist(std::move(data));
                }
                case kMarkerAscii:
                {
                    const std::size_t count = static_cast<std::size_t>(read_count());
                    return Plist(std::string(reinterpret_cast<const char *>(bytes.data() + position), count));
                }
                case kMarkerUtf16:
                {
                    const std::size_t count = static_cast<std::size_t>(read_count());
                    std::string text;
                    text.reserve(count);
                    // A UTF-16 object is big-endian pairs. This codec keeps only the
                    // low byte of each pair, so a non-ASCII character is decoded
                    // incorrectly; the plists the device sends here are ASCII.
                    for (std::size_t i = 0; i < count; ++i)
                    {
                        text.push_back(static_cast<char>(bytes[position + i * 2 + 1]));
                    }
                    return Plist(std::move(text));
                }
                case kMarkerUid:
                {
                    const std::size_t size = std::size_t{1} << info;
                    return Plist::uid(read_uint(size));
                }
                case kMarkerArray:
                {
                    const std::size_t count = static_cast<std::size_t>(read_count());
                    Plist::Array items;
                    items.reserve(count);
                    for (std::size_t i = 0; i < count; ++i)
                    {
                        const std::uint64_t reference = read_uint(ref_size);
                        auto item = decode(static_cast<std::size_t>(reference));
                        if (!item)
                        {
                            return tl::unexpected(item.error());
                        }
                        items.push_back(std::move(*item));
                    }
                    return Plist::array(std::move(items));
                }
                case kMarkerDictionary:
                {
                    const std::size_t count = static_cast<std::size_t>(read_count());
                    std::vector<std::uint64_t> keys(count);
                    for (std::size_t i = 0; i < count; ++i)
                    {
                        keys[i] = read_uint(ref_size);
                    }
                    Plist::Dictionary items;
                    for (std::size_t i = 0; i < count; ++i)
                    {
                        auto key = decode(static_cast<std::size_t>(keys[i]));
                        if (!key)
                        {
                            return tl::unexpected(key.error());
                        }
                        auto item = decode(static_cast<std::size_t>(read_uint(ref_size)));
                        if (!item)
                        {
                            return tl::unexpected(item.error());
                        }
                        items.emplace(key->string_or(), std::move(*item));
                    }
                    return Plist::dictionary(std::move(items));
                }
                default:
                    return tl::unexpected(protocol_error("a binary plist object marker is not known"));
            }
        }
    };

    Decoder decoder{bytes, offsets, cache, active, ref_size};
    return decoder.decode(static_cast<std::size_t>(top_object));
}

} // namespace ioscpp::protocol
