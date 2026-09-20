#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"
#include "ioscpp/transport.hpp"

namespace ioscpp::protocol
{

/// The connection preface every HTTP/2 client sends before its first frame.
inline constexpr std::string_view kHttp2Preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

/// The stream the client sends its messages on.
inline constexpr std::uint32_t kHttp2ControlStream = 1;

/// The stream the server sends its replies on.
inline constexpr std::uint32_t kHttp2ReplyStream = 3;

/// The receive window the client grants (16 MiB).
inline constexpr std::uint32_t kHttp2WindowSize = 16 * 1024 * 1024;

/// The connection-level window increment the client grants to reach @ref kHttp2WindowSize.
inline constexpr std::uint32_t kHttp2WindowIncrement = kHttp2WindowSize - 65535;

/// One DATA frame: the stream it arrived on and its payload.
struct Http2Message
{
    std::uint32_t stream = 0;
    std::vector<std::byte> payload;
};

/**
 * @brief A blocking HTTP/2 connection to the device's RSD port.
 *
 * RemoteXPC does not sit on the tunnel's raw byte stream: it runs over HTTP/2, so
 * a `DATA` frame carries each message and the reply travels on a second stream.
 * The frames are simple and the `HEADERS` frames carry no fields, so this layer is
 * hand-rolled over a @ref Transport with no dependency and no HPACK, the way go-ios
 * and pymobiledevice3 do it, and fits the blocking `TcpLink`.
 *
 * `connect` sends the preface, the client `SETTINGS`, and the connection
 * `WINDOW_UPDATE`, then reads the server's `SETTINGS` and acknowledges it.
 * `open` sends an empty `HEADERS` frame that opens a stream, `write` sends a
 * `DATA` frame, and `read` returns the next whole `DATA` frame. A payload larger
 * than the peer's window is split across `DATA` frames as the window allows, so
 * `write` blocks until the peer grants more rather than overrunning it.
 *
 * The connection borrows `transport`, which must outlive it. An `Http2` is not
 * thread-safe, so concurrent calls must be serialized by the caller.
 */
class IOSCPP_API Http2
{
public:
    /// Opens the connection over `transport`, which the caller keeps alive.
    static Result<Http2> connect(Transport &transport);

    ~Http2();
    Http2(Http2 &&) noexcept;
    Http2 &operator=(Http2 &&) noexcept;
    Http2(const Http2 &) = delete;
    Http2 &operator=(const Http2 &) = delete;

    /// Sends an empty `HEADERS` frame that opens `stream`.
    Status open(std::uint32_t stream);

    /// Sends `payload` as one message on `stream`, split across frames by the window.
    Status write(std::uint32_t stream, std::span<const std::byte> payload);

    /// Reads until a whole `DATA` frame arrives, returning its stream and payload.
    Result<Http2Message> read();

    /// Sends a `GOAWAY` frame and releases the connection.
    void close();

private:
    explicit Http2(Transport &transport);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ioscpp::protocol
