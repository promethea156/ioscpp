#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"

namespace ioscpp::protocol
{

/// Which of the plist value kinds a @ref Plist holds.
enum class PlistType
{
    Null,
    Boolean,
    Integer,
    Real,
    String,
    Data,
    Date,
    Array,
    Dictionary,
    Uid
};

/// A date, as the whole seconds since 2001-01-01T00:00:00Z, the plist epoch.
struct Date
{
    std::int64_t seconds_since_2001 = 0;

    friend bool operator==(const Date &, const Date &) = default;
};

/**
 * @brief A property list value.
 *
 * Property lists are how every control message on the device is encoded, so this is the
 * foundation the `lockdownd`, `installation_proxy`, and process-control clients are
 * built on. The value is a variant over the nine plist kinds, and the accessors return
 * an empty `optional` (or a null pointer) when the held kind does not match rather than
 * converting.
 *
 * The codec parses both the XML form, which is what `lockdownd` and
 * `installation_proxy` send, and the binary `bplist00` form, which some services use.
 * Serialization writes the XML form, which every device accepts.
 *
 * A dictionary keeps its keys sorted, so a serialized dictionary is deterministic.
 */
class IOSCPP_API Plist
{
public:
    using Array = std::vector<Plist>;
    using Dictionary = std::map<std::string, Plist, std::less<>>;

    /// A null value.
    Plist() noexcept = default;
    Plist(std::nullptr_t) noexcept
    {
    }

    Plist(bool value);
    Plist(int value);
    Plist(std::int64_t value);
    Plist(double value);
    Plist(std::string value);
    Plist(const char *value);
    Plist(std::string_view value);
    /// A `Data` value, a byte blob.
    explicit Plist(std::vector<std::byte> value);

    /// A `Date` value, in seconds since 2001-01-01T00:00:00Z.
    static Plist date(std::int64_t seconds_since_2001);
    static Plist array(Array values);
    static Plist dictionary(Dictionary values);

    /**
     * @brief A `Uid`, an index into a binary plist's object table.
     *
     * A `Uid` only exists in the binary form: it is how an `NSKeyedArchive`
     * refers to its objects, and `0` is the `$null` slot. It has no XML form, so
     * @ref to_xml writes its index as an integer.
     */
    static Plist uid(std::uint64_t value);

    /// The kind of value held.
    PlistType type() const noexcept;

    bool is_null() const noexcept;
    bool is_boolean() const noexcept;
    bool is_integer() const noexcept;
    bool is_real() const noexcept;
    bool is_string() const noexcept;
    bool is_data() const noexcept;
    bool is_date() const noexcept;
    bool is_array() const noexcept;
    bool is_dictionary() const noexcept;
    bool is_uid() const noexcept;

    /// The boolean, or nothing when the value is not a boolean.
    std::optional<bool> boolean() const noexcept;
    /// The integer, or nothing when the value is not an integer.
    std::optional<std::int64_t> integer() const noexcept;
    /// The real, or nothing when the value is not a real.
    std::optional<double> real() const noexcept;
    /// The string, or nothing when the value is not a string.
    std::optional<std::string_view> string() const noexcept;
    /// The data bytes, or nothing when the value is not data.
    std::optional<std::span<const std::byte>> data() const noexcept;
    /// The date, or nothing when the value is not a date.
    std::optional<Date> date() const noexcept;
    /// The array, or nullptr when the value is not an array.
    const Array *array() const noexcept;
    /// The dictionary, or nullptr when the value is not a dictionary.
    const Dictionary *dictionary() const noexcept;
    /// The `Uid` index, or nothing when the value is not a `Uid`.
    std::optional<std::uint64_t> uid() const noexcept;

    /// The string, or `fallback` when the value is not a string.
    std::string string_or(std::string_view fallback = {}) const;

    /// The value for `key` when this is a dictionary, or nullptr.
    const Plist *find(std::string_view key) const noexcept;

    /// Serializes to the XML form, which every device accepts.
    std::string to_xml() const;

    /// Serializes to the binary `bplist00` form.
    std::vector<std::byte> to_binary() const;

    /// Parses either form, choosing on the leading bytes.
    static Result<Plist> parse(std::span<const std::byte> bytes);

    /// Parses the XML form.
    static Result<Plist> parse_xml(std::string_view xml);

    /// Parses the binary `bplist00` form.
    static Result<Plist> parse_binary(std::span<const std::byte> bytes);

private:
    using Value = std::variant<std::monostate, bool, std::int64_t, double, std::string, std::vector<std::byte>, Date,
                               Array, Dictionary, std::uint64_t>;

    Value value_;
};

} // namespace ioscpp::protocol
