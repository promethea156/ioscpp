#pragma once

#include <string>
#include <tl/expected.hpp>

#include "ioscpp/export.hpp"

namespace ioscpp
{

/**
 * @brief What went wrong, so that a caller can branch without matching on a message.
 *
 * The distinction between these is not how severe the problem is but who is being
 * reported on: `InvalidArgument` and `Io` are the caller's or the host's side,
 * `Transport` is the link, and `Protocol` and `Device` are the device's.
 */
enum class ErrorCode
{
    /// The caller passed something the library cannot use: a malformed device
    /// selector or endpoint, a local file that is not a regular file, or a payload
    /// that does not fit the link.
    InvalidArgument,
    /// The link to the device failed: libusb reported an error, no matching device
    /// was found, or a transfer was short.
    Transport,
    /// The device did not answer as the protocol requires: an unexpected frame, an
    /// unexpected end of the stream, a bad magic, or a packet number that does not
    /// match.
    Protocol,
    /// The device refused the request and gave a reason, which is the message. This is
    /// an `AFC` or `lockdownd` error status.
    Device,
    /// The pairing record or key could not be found, read, generated, or used to sign,
    /// or the TLS session failed.
    Crypto,
    /// A local file could not be read or written.
    Io
};

/**
 * @brief A failure, with the reason a human can read.
 *
 * The message does not repeat the library's name, because the type already carries
 * it: a caller prints `error: <message>`.
 */
struct IOSCPP_API Error
{
    ErrorCode code = ErrorCode::Protocol;
    std::string message;
};

/**
 * @brief The outcome of an operation: a value, or the reason there is none.
 *
 * This is `tl::expected` itself rather than a new type, so its whole interface is
 * available: `has_value()`, `operator*`, `operator->`, `value_or`, `and_then`,
 * `map`, and `or_else`. The alias exists so that the public API does not depend
 * on the spelling of the library that provides it, which makes moving to
 * `std::expected` in C++23 a change here alone.
 *
 * @warning `value()` and `error()` assert when they are called on the wrong
 * alternative. The library never calls them without checking, and neither should a
 * caller.
 */
template <typename T>
using Result = tl::expected<T, Error>;

/// The outcome of an operation with nothing to report.
using Status = tl::expected<void, Error>;

} // namespace ioscpp
