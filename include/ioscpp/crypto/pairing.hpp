#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"

namespace ioscpp
{
class Lockdown;
class Stream;
} // namespace ioscpp

namespace ioscpp::crypto
{

/**
 * @brief The host's pairing record with a device.
 *
 * A pairing record holds the host's RSA key pair and certificate (self-signed before
 * pairing, signed by the root CA after), the device's certificate, and the device's
 * root certificate. `lockdownd` refuses to start a session, and so any service, until
 * it has accepted this record.
 *
 * The record is persisted as a plist. The default path is
 * `~/.ioscpp/<serial>.plist`, mirroring how `libimobiledevice` stores its records.
 *
 * `load` loads the record when the file exists and generates a fresh key pair and
 * certificate when it does not; `pair` completes the record against the device. After a
 * successful `pair`, `save` writes it, so the next run reuses it and the device does
 * not ask for trust again.
 *
 * A `Pairing` is not thread-safe: `pair` and the `set_*` calls replace its state
 * with no synchronization, so concurrent calls must be serialized by the caller.
 */
class IOSCPP_API Pairing
{
public:
    ~Pairing();
    Pairing(Pairing &&) noexcept;
    Pairing &operator=(Pairing &&) noexcept;
    Pairing(const Pairing &) = delete;
    Pairing &operator=(const Pairing &) = delete;

    /// Loads the record at `record_path`, generating it when absent.
    static Result<Pairing> load(const std::filesystem::path &record_path);

    /// Loads the record for the device with this USB serial, which is its udid without
    /// the dash, from the default directory, generating it when absent.
    static Result<Pairing> load_for_udid(std::string_view udid);

    /// Writes the record to the path it was loaded from.
    Status save() const;

    /// Whether the device side of the record is present, so pairing is complete.
    bool paired() const noexcept;

    /// The host id, a fixed UUID that identifies this host to the device.
    std::string_view host_id() const noexcept;

    /// The device's unique id, once known.
    std::string_view udid() const noexcept;

    /// Sets the device's unique id, which the pairing exchange learns from the device.
    void set_udid(std::string udid);

    /// The session id the device handed out with `StartSession`.
    std::string_view session_id() const noexcept;

    /// Sets the session id the device handed out with `StartSession`.
    void set_session_id(std::string session_id);

    /// The system build id, a fixed UUID shared by every record on this host.
    std::string_view system_buid() const noexcept;

    /// The host's PEM-encoded certificate.
    std::span<const std::byte> host_certificate() const noexcept;

    /// The host's PEM-encoded private key.
    std::span<const std::byte> host_private_key() const noexcept;

    /// The device's PEM-encoded certificate.
    std::span<const std::byte> device_certificate() const noexcept;

    /// The device's PEM-encoded root certificate.
    std::span<const std::byte> root_certificate() const noexcept;

    /// The 20-byte AES session key derived by the pairing exchange.
    std::span<const std::byte> session_key() const noexcept;

private:
    friend Status pair(Lockdown &lockdown, Pairing &pairing);
    friend class TlsSession;

    Pairing();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief Completes the pairing exchange with `lockdown`.
 *
 * Sends a `Pair` request carrying the host certificate, the host id, and the system
 * build id, and stores the device's certificate, its root certificate, its escrow bag,
 * its Wi-Fi MAC address, and the host and root certificate/key pair in `pairing`. The
 * device may show the *Trust This Computer?* prompt, so this blocks until it is answered.
 *
 * The record must be saved afterwards, or the exchange is repeated on the next run.
 */
Status pair(Lockdown &lockdown, Pairing &pairing);

/**
 * @brief The TLS session that `lockdownd` wraps every later message in.
 *
 * After `StartSession`, `lockdownd` speaks TLS, using the host certificate and its
 * private key from pairing. `start` runs the handshake over `stream`, and `read` and
 * `write` move plaintext through it.
 */
class IOSCPP_API TlsSession
{
public:
    ~TlsSession();
    TlsSession(TlsSession &&) noexcept;
    TlsSession &operator=(TlsSession &&) noexcept;
    TlsSession(const TlsSession &) = delete;
    TlsSession &operator=(const TlsSession &) = delete;

    /// Runs the TLS handshake over `stream` as the client.
    static Result<TlsSession> start(Stream &stream, const Pairing &pairing);

    /// Writes plaintext to the TLS session.
    Status write(std::span<const std::byte> data);
    /// Reads up to `buffer.size()` bytes of plaintext.
    Result<std::size_t> read(std::span<std::byte> buffer);
    /// Sends `close_notify` and frees the session. A second call is a no-op.
    void close();

private:
    TlsSession();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ioscpp::crypto
