#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ioscpp/byte_stream.hpp"
#include "ioscpp/connection.hpp"
#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"

namespace ioscpp
{

/**
 * @brief A byte stream to a service on the device.
 *
 * There are two kinds of stream, matching the two ways into a @ref Connection.
 * On a direct USB link, a stream is a mux TCP connection to one port: `open`
 * performs the SYN / SYN|ACK / ACK exchange and leaves the connection
 * established, `write` sends `PSH|ACK` frames, and `read` collects the device's
 * data frames and acknowledges each with an `ACK`. This is what `usbmuxd` does on
 * the host side:
 *
 *   https://github.com/libimobiledevice/usbmuxd/blob/master/src/device.c
 *
 * Over a socket to a running `usbmuxd`, there is no mux framing: `open` sends a
 * `Connect` plist for the port and then the stream carries the port's raw bytes.
 *
 * A `Stream` is not thread-safe. It shares its connection's session and frame
 * routing, and it buffers a partial frame across reads, so concurrent calls on one
 * stream must be serialized by the caller.
 */
class IOSCPP_API Stream : public ByteStream
{
public:
    /**
     * @brief Opens a stream to `port` on the device.
     *
     * Opening can fail, and a constructor cannot report that, so this is a named
     * factory and the constructor is private.
     */
    static Result<Stream> open(std::shared_ptr<Connection> connection, std::uint16_t port);

    ~Stream();

    Stream(const Stream &) = delete;
    Stream &operator=(const Stream &) = delete;

    /// A stream is returned by value, so it moves. The moved-from stream is marked
    /// closed, so its destructor does not send a second reset.
    Stream(Stream &&other) noexcept;
    Stream &operator=(Stream &&other) noexcept;

    /// Writes data to the device.
    Status write(std::span<const std::byte> data) override;

    /**
     * @brief Reads exactly `buffer.size()` bytes from the device.
     *
     * A frame may be larger or smaller than `buffer`, so the remainder is
     * buffered and handed out by later reads. An error is returned if the device
     * resets the connection first.
     */
    Status read_exact(std::span<std::byte> buffer) override;

    /// Reads all output until the device resets the connection.
    Result<std::vector<std::byte>> read_all();

    /**
     * @brief Resets the port, releasing it on the device.
     *
     * The reset is sent once; a second call, or one after the stream has already
     * been moved from, is a no-op. This is the stream half of an ordered
     * teardown, and the destructor routes through it, so there is one path.
     */
    void close() noexcept override;

    /// The remote port this stream is connected to.
    std::uint16_t port() const noexcept
    {
        return remote_port_;
    }

    /// The diagnostic the device sent with its reset, if it reset the port. The
    /// device puts a human-readable reason in the RST payload, the same string
    /// `usbmuxd` logs as `RST reason`, so it is kept for the error message.
    std::string_view reset_reason() const noexcept
    {
        return reset_reason_;
    }

private:
    friend class Connection;

    Stream(std::shared_ptr<Connection> connection, std::uint16_t local_port, std::uint16_t remote_port);

    // Sends a reset if it has not been sent. The destructor and the move
    // assignment both need it.
    void close_now() noexcept;

    // Receives frames until one carries data for this stream, which is appended to
    // the buffer. Returns false when the device reset the connection instead.
    Result<bool> receive_more();

    // Records the diagnostic the device attached to an RST, if any, for a later
    // error message. A payload is a NUL-terminated string that may carry a
    // trailing newline.
    void remember_reset_reason(std::span<const std::byte> payload);

    std::shared_ptr<Connection> connection_;
    std::uint16_t local_port_;
    std::uint16_t remote_port_;
    std::uint32_t tx_seq_ = 1;
    std::uint32_t tx_ack_ = 1;
    bool closed_ = false;
    std::string reset_reason_;

    // The `usbmuxd` path: the stream owns its socket and speaks raw bytes.
    bool usbmuxd_ = false;
    std::unique_ptr<Transport> owned_transport_;

    std::vector<std::byte> incoming_;
    std::size_t incoming_offset_ = 0;
};

} // namespace ioscpp
