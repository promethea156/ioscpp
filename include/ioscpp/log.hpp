#pragma once

#include <functional>
#include <string_view>

#include "ioscpp/export.hpp"

namespace ioscpp
{

/**
 * @brief The severity of a log message, from least to most verbose.
 *
 * The levels are ordered: a logger is installed with the most verbose level it
 * writes, and every level at or below it is written too. `Info` is the default,
 * which reports state changes but not the frames themselves, so a caller has to
 * raise the level to `Debug` to follow the protocol.
 */
enum class LogLevel
{
    /// A failure. The operation also returns this as an `Error`.
    Error,
    /// Something unexpected that did not fail, for example a retry.
    Warning,
    /// A state change: a connection, a stream, a session, or a service.
    Info,
    /// A protocol frame sent or received, without its payload.
    Debug,
    /// A payload-derived detail, such as the service a stream is opened for.
    Trace
};

/**
 * @brief Receives a log message.
 *
 * The message is valid only for the duration of the call, so a sink that keeps it
 * copies it. The sink is called from the thread that caused the event, so with one
 * thread per device it may be called from several threads at once and must
 * serialize itself if it shares state. It must not throw, because the library
 * reports failures with `Result` rather than exceptions.
 */
using LogSink = std::function<void(LogLevel level, std::string_view message)>;

/**
 * @brief Installs `sink` as the process-wide log sink and writes every level
 * from `Error` up to and including `level`.
 *
 * Logging is opt-in: nothing is written until this is called, and the library
 * never installs a sink of its own. The sink is process-wide because the objects
 * that log do not take one; installing a second sink replaces the first. `log` is
 * safe to call from several threads at once, but install the sink before the
 * connections are opened so that a sink is not replaced under a message.
 */
IOSCPP_API void set_logger(LogSink sink, LogLevel level = LogLevel::Info);

/// Removes the sink, so nothing is logged. Logging is then off, as it is before
/// @ref set_logger is called. Calling it when no sink is installed is harmless.
IOSCPP_API void clear_logger();

/// Whether a message at `level` would be written, so a caller can avoid building
/// a message the sink would discard.
IOSCPP_API bool is_logging(LogLevel level);

/**
 * @brief Writes `message` at `level` to the installed sink.
 *
 * Does nothing when no sink is installed or when `level` is more verbose than the
 * installed level. The library never passes key material or a payload to this, and a
 * caller should not either.
 */
IOSCPP_API void log(LogLevel level, std::string_view message);

} // namespace ioscpp
