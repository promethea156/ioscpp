#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "ioscpp/crypto/pairing.hpp"
#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"
#include "ioscpp/protocol/plist.hpp"
#include "ioscpp/stream.hpp"

namespace ioscpp
{

/**
 * @brief A `lockdownd` client on the device's gatekeeper port.
 *
 * `lockdownd` is the service every other service is reached through: it owns the
 * pairing state, answers device queries, and hands out a port for a named service with
 * `StartService`. It listens on port 62078 and speaks length-prefixed plists.
 *
 * `start` opens a plaintext client over a stream to that port. `start_session` performs
 * the `StartSession` exchange and then wraps every later message in TLS, using the session
 * key from the pairing record; `request` is the plaintext and TLS path in one call.
 *
 * A `Lockdown` is not thread-safe. It shares its stream with the TLS session, so
 * concurrent calls must be serialized by the caller.
 */
class IOSCPP_API Lockdown
{
public:
    /**
     * @brief Opens a `lockdownd` client over `stream`.
     *
     * The stream must already be connected to port 62078.
     */
    static Result<Lockdown> start(Stream stream);

    ~Lockdown();
    Lockdown(Lockdown &&) noexcept;
    Lockdown &operator=(Lockdown &&) noexcept;
    Lockdown(const Lockdown &) = delete;
    Lockdown &operator=(const Lockdown &) = delete;

    /**
     * @brief Sends `request` and returns the device's answer.
     *
     * This is the one place a plist is framed, written, read back, and unframed, so
     * every higher call is built on it. A device error is an `ErrorCode::Device`
     * error carrying the reason.
     */
    Result<protocol::Plist> request(protocol::Plist request);

    /// Sends a `QueryType` request and returns the device's answer.
    Result<protocol::Plist> query(std::string_view type);

    /// Reads `key` from `domain` with a `GetValue` request.
    Result<protocol::Plist> get_value(std::string_view domain, std::string_view key);

    /**
     * @brief Performs the `StartSession` exchange and switches to TLS.
     *
     * The pairing record must be complete. After this, `request` speaks TLS, which
     * `lockdownd` requires before it will start a service.
     */
    Status start_session(crypto::Pairing &pairing);

    /**
     * @brief Starts the service `name` and returns the port it was given.
     *
     * The device allocates a port for the service, which the caller then connects a
     * `Stream` to. A service the device does not know is an `ErrorCode::Device`
     * error.
     */
    Result<std::uint16_t> start_service(std::string_view name);

    /// The underlying stream, for a caller that needs the raw channel.
    Stream &stream() noexcept;

    /**
     * @brief Closes the `lockdownd` session, innermost first.
     *
     * Sends the TLS `close_notify` when the session is encrypted, then resets
     * the stream. The call is idempotent, so a second one is a no-op, and the
     * destructor routes through it, so there is one path.
     */
    void close() noexcept;

private:
    friend Status crypto::pair(Lockdown &lockdown, crypto::Pairing &pairing);

    explicit Lockdown(Stream stream);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ioscpp
