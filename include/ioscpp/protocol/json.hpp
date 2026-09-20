#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"

namespace ioscpp::protocol
{

/// Which of the JSON value kinds a @ref Json holds.
enum class JsonType
{
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object
};

/**
 * @brief A JSON value.
 *
 * The CoreDevice tunnel handshake is JSON rather than a plist, so this is the
 * codec that frame is built on. It is deliberately small: the tunnel's handshake
 * is the only JSON in the library, and its payload uses objects, strings, and
 * numbers alone, though the codec handles every kind below.
 *
 * The value is a variant over the six JSON kinds, and the accessors return an
 * empty `optional` (or a null pointer) when the held kind does not match rather
 * than converting. A number is held as a `double`, and an integral value is
 * serialized without a fraction, so `1280` round-trips as `1280` and not `1280.0`.
 *
 * An object keeps its keys sorted, so a serialized object is deterministic. JSON
 * object order is not significant, so a sorted object is accepted by the device.
 */
class IOSCPP_API Json
{
public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json, std::less<>>;

    /// A null value.
    Json() noexcept = default;
    Json(std::nullptr_t) noexcept
    {
    }

    Json(bool value);
    Json(int value);
    Json(std::int64_t value);
    Json(double value);
    Json(std::string value);
    Json(const char *value);

    static Json array(Array values);
    static Json object(Object values);

    /// The kind of value held.
    JsonType type() const noexcept;

    bool is_null() const noexcept;
    bool is_boolean() const noexcept;
    bool is_number() const noexcept;
    bool is_string() const noexcept;
    bool is_array() const noexcept;
    bool is_object() const noexcept;

    /// The boolean, or nothing when the value is not a boolean.
    std::optional<bool> boolean() const noexcept;
    /// The number, or nothing when the value is not a number.
    std::optional<double> number() const noexcept;
    /// The string, or nothing when the value is not a string.
    std::optional<std::string_view> string() const noexcept;
    /// The array, or nullptr when the value is not an array.
    const Array *array() const noexcept;
    /// The object, or nullptr when the value is not an object.
    const Object *object() const noexcept;

    /// The value for `key` when this is an object, or nullptr.
    const Json *find(std::string_view key) const noexcept;

    /// Serializes to compact JSON text.
    std::string to_string() const;

    /// Parses JSON text. Any trailing whitespace is allowed, trailing text is not.
    static Result<Json> parse(std::string_view text);

private:
    using Value = std::variant<std::monostate, bool, double, std::string, Array, Object>;

    Value value_;
};

} // namespace ioscpp::protocol
