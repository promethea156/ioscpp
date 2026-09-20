#include "ioscpp/protocol/http2.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
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

/// The frame types this layer speaks.
inline constexpr std::uint8_t kData = 0x0;
inline constexpr std::uint8_t kHeaders = 0x1;
inline constexpr std::uint8_t kRstStream = 0x3;
inline constexpr std::uint8_t kSettings = 0x4;
inline constexpr std::uint8_t kPing = 0x6;
inline constexpr std::uint8_t kGoAway = 0x7;
inline constexpr std::uint8_t kWindowUpdate = 0x8;

/// The `END_STREAM` and `ACK` flags, the ones this layer sets or reads.
inline constexpr std::uint8_t kEndStream = 0x1;
inline constexpr std::uint8_t kAck = 0x1;

/// The two settings this layer reads: the stream limit and the window size.
inline constexpr std::uint16_t kSettingsMaxConcurrentStreams = 0x3;
inline constexpr std::uint16_t kSettingsInitialWindowSize = 0x4;

/// The HTTP/2 default window size, which a peer's window starts at.
inline constexpr std::int32_t kDefaultWindowSize = 65535;

/// The largest frame this layer sends, the HTTP/2 default.
inline constexpr std::size_t kMaxFrameSize = 16384;

std::uint8_t byte_at(std::span<const std::byte> bytes, std::size_t offset)
{
    return std::to_integer<std::uint8_t>(bytes[offset]);
}

/// Appends an HTTP/2 frame: the 3-byte length, the type, the flags, the stream, the payload.
void append_frame(std::vector<std::byte> &out, std::uint8_t type, std::uint8_t flags, std::uint32_t stream,
                  std::span<const std::byte> payload)
{
    const std::size_t length = payload.size();
    out.push_back(static_cast<std::byte>((length >> 16) & 0xff));
    out.push_back(static_cast<std::byte>((length >> 8) & 0xff));
    out.push_back(static_cast<std::byte>(length & 0xff));
    out.push_back(static_cast<std::byte>(type));
    out.push_back(static_cast<std::byte>(flags));
    out.push_back(static_cast<std::byte>((stream >> 24) & 0x7f));
    out.push_back(static_cast<std::byte>((stream >> 16) & 0xff));
    out.push_back(static_cast<std::byte>((stream >> 8) & 0xff));
    out.push_back(static_cast<std::byte>(stream & 0xff));
    out.insert(out.end(), payload.begin(), payload.end());
}

/// Appends a SETTINGS entry: the 16-bit id and the 32-bit value.
void append_setting(std::vector<std::byte> &out, std::uint16_t id, std::uint32_t value)
{
    out.push_back(static_cast<std::byte>((id >> 8) & 0xff));
    out.push_back(static_cast<std::byte>(id & 0xff));
    out.push_back(static_cast<std::byte>((value >> 24) & 0xff));
    out.push_back(static_cast<std::byte>((value >> 16) & 0xff));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xff));
    out.push_back(static_cast<std::byte>(value & 0xff));
}

/// A SETTINGS frame, optionally an acknowledgement.
std::vector<std::byte> settings_frame(bool ack)
{
    std::vector<std::byte> out;
    append_frame(out, kSettings, ack ? kAck : 0, 0, {});
    return out;
}

/// A WINDOW_UPDATE frame: the reserved bit and the 31-bit increment.
std::vector<std::byte> window_update_frame(std::uint32_t stream, std::uint32_t increment)
{
    const std::vector<std::byte> payload = {
        static_cast<std::byte>((increment >> 24) & 0x7f),
        static_cast<std::byte>((increment >> 16) & 0xff),
        static_cast<std::byte>((increment >> 8) & 0xff),
        static_cast<std::byte>(increment & 0xff),
    };
    std::vector<std::byte> out;
    append_frame(out, kWindowUpdate, 0, stream, payload);
    return out;
}

/// An empty HEADERS frame that opens `stream`.
std::vector<std::byte> headers_frame(std::uint32_t stream)
{
    std::vector<std::byte> out;
    append_frame(out, kHeaders, 0, stream, {});
    return out;
}

/// A DATA frame on `stream`.
std::vector<std::byte> data_frame(std::uint32_t stream, std::span<const std::byte> payload)
{
    std::vector<std::byte> out;
    append_frame(out, kData, 0, stream, payload);
    return out;
}

/// A GOAWAY frame that closes the connection.
std::vector<std::byte> goaway_frame()
{
    std::vector<std::byte> out;
    append_frame(out, kGoAway, 0, 0, {});
    return out;
}

/// A PING acknowledgement.
std::vector<std::byte> ping_ack_frame()
{
    const std::vector<std::byte> payload(8);
    std::vector<std::byte> out;
    append_frame(out, kPing, kAck, 0, payload);
    return out;
}

} // namespace

struct Http2::Impl
{
    Transport *transport = nullptr;

    /// The bytes read from the transport that are not yet a whole frame.
    std::vector<std::byte> buffer;

    /// The DATA frames received, in order.
    std::deque<Http2Message> received;

    /// The peer's granted connection window.
    std::int32_t connection_window = kDefaultWindowSize;
    /// The peer's granted window for the stream being written.
    std::int32_t stream_window = kDefaultWindowSize;

    bool settings_seen = false;
    bool failed = false;
    Error error;

    /// Reads from the transport until the buffer holds `count` bytes.
    Status fill(std::size_t count);
    /// Reads and handles exactly one frame from the transport.
    Status read_frame();
};

Status Http2::Impl::fill(std::size_t count)
{
    while (buffer.size() < count)
    {
        std::byte chunk[16384];
        auto read = transport->read(chunk);
        if (!read)
        {
            return tl::unexpected(read.error());
        }
        if (*read == 0)
        {
            return tl::unexpected(protocol_error("the HTTP/2 connection ended"));
        }
        buffer.insert(buffer.end(), chunk, chunk + *read);
    }
    return {};
}

Status Http2::Impl::read_frame()
{
    if (auto status = fill(9); !status)
    {
        return status;
    }

    const std::size_t length = (static_cast<std::size_t>(byte_at(buffer, 0)) << 16) |
                               (static_cast<std::size_t>(byte_at(buffer, 1)) << 8) |
                               static_cast<std::size_t>(byte_at(buffer, 2));
    const std::uint8_t type = byte_at(buffer, 3);
    const std::uint8_t flags = byte_at(buffer, 4);
    const std::uint32_t stream = (static_cast<std::uint32_t>(byte_at(buffer, 5)) << 24) |
                                 (static_cast<std::uint32_t>(byte_at(buffer, 6)) << 16) |
                                 (static_cast<std::uint32_t>(byte_at(buffer, 7)) << 8) |
                                 static_cast<std::uint32_t>(byte_at(buffer, 8));

    if (auto status = fill(9 + length); !status)
    {
        return status;
    }
    const std::span<const std::byte> payload(buffer.data() + 9, length);

    switch (type)
    {
        case kData:
            if (length > 0)
            {
                Http2Message message;
                message.stream = stream;
                message.payload.assign(payload.begin(), payload.end());
                received.push_back(std::move(message));
            }
            break;
        case kSettings:
            if ((flags & kAck) == 0)
            {
                // Apply the peer's window size, then acknowledge.
                for (std::size_t offset = 0; offset + 6 <= length; offset += 6)
                {
                    const std::uint16_t id = static_cast<std::uint16_t>(
                        (static_cast<unsigned>(byte_at(payload, offset)) << 8) | byte_at(payload, offset + 1));
                    const std::uint32_t value = (static_cast<std::uint32_t>(byte_at(payload, offset + 2)) << 24) |
                                                (static_cast<std::uint32_t>(byte_at(payload, offset + 3)) << 16) |
                                                (static_cast<std::uint32_t>(byte_at(payload, offset + 4)) << 8) |
                                                static_cast<std::uint32_t>(byte_at(payload, offset + 5));
                    if (id == kSettingsInitialWindowSize)
                    {
                        stream_window = static_cast<std::int32_t>(value);
                    }
                }
                settings_seen = true;
                const std::vector<std::byte> ack = settings_frame(true);
                if (auto status = transport->write(ack); !status)
                {
                    return status;
                }
            }
            break;
        case kWindowUpdate:
        {
            const std::uint32_t increment = (static_cast<std::uint32_t>(byte_at(payload, 0)) << 24) |
                                            (static_cast<std::uint32_t>(byte_at(payload, 1)) << 16) |
                                            (static_cast<std::uint32_t>(byte_at(payload, 2)) << 8) |
                                            static_cast<std::uint32_t>(byte_at(payload, 3));
            if (stream == 0)
            {
                connection_window += static_cast<std::int32_t>(increment);
            }
            else
            {
                stream_window += static_cast<std::int32_t>(increment);
            }
            break;
        }
        case kPing:
            if ((flags & kAck) == 0)
            {
                const std::vector<std::byte> ack = ping_ack_frame();
                if (auto status = transport->write(ack); !status)
                {
                    return status;
                }
            }
            break;
        case kGoAway:
            failed = true;
            error = protocol_error("the peer sent GOAWAY");
            break;
        case kRstStream:
            failed = true;
            error = protocol_error("the peer reset a stream");
            break;
        default:
            break;
    }

    buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(9 + length));
    return {};
}

Http2::Http2(Transport &transport)
    : impl_(std::make_unique<Impl>())
{
    impl_->transport = &transport;
}

Http2::Http2(Http2 &&) noexcept = default;

Http2 &Http2::operator=(Http2 &&) noexcept = default;

Http2::~Http2()
{
    close();
}

Result<Http2> Http2::connect(Transport &transport)
{
    Http2 http(transport);

    // The connection preface, then the client's SETTINGS and connection WINDOW_UPDATE.
    std::vector<std::byte> preface(kHttp2Preface.size());
    std::memcpy(preface.data(), kHttp2Preface.data(), kHttp2Preface.size());
    if (auto status = transport.write(preface); !status)
    {
        return tl::unexpected(status.error());
    }

    std::vector<std::byte> payload;
    append_setting(payload, kSettingsMaxConcurrentStreams, 100);
    append_setting(payload, kSettingsInitialWindowSize, kHttp2WindowSize);
    std::vector<std::byte> settings;
    append_frame(settings, kSettings, 0, 0, payload);
    if (auto status = transport.write(settings); !status)
    {
        return tl::unexpected(status.error());
    }

    const std::vector<std::byte> update = window_update_frame(0, kHttp2WindowIncrement);
    if (auto status = transport.write(update); !status)
    {
        return tl::unexpected(status.error());
    }

    // The peer's SETTINGS is acknowledged by `read_frame`, so read until it
    // arrives and the window it grants is known.
    while (!http.impl_->settings_seen)
    {
        if (http.impl_->failed)
        {
            return tl::unexpected(http.impl_->error);
        }
        if (auto status = http.impl_->read_frame(); !status)
        {
            return tl::unexpected(status.error());
        }
    }
    return http;
}

Status Http2::open(std::uint32_t stream)
{
    const std::vector<std::byte> frame = headers_frame(stream);
    return impl_->transport->write(frame);
}

Status Http2::write(std::uint32_t stream, std::span<const std::byte> payload)
{
    std::size_t offset = 0;
    while (offset < payload.size())
    {
        const std::int32_t budget =
            std::min({impl_->connection_window, impl_->stream_window, static_cast<std::int32_t>(kMaxFrameSize)});
        if (budget <= 0)
        {
            // The peer's window is exhausted, so wait for it to grant more.
            if (auto status = impl_->read_frame(); !status)
            {
                return status;
            }
            continue;
        }
        const std::size_t count = std::min(static_cast<std::size_t>(budget), payload.size() - offset);
        const std::vector<std::byte> frame = data_frame(stream, payload.subspan(offset, count));
        if (auto status = impl_->transport->write(frame); !status)
        {
            return status;
        }
        offset += count;
        impl_->connection_window -= static_cast<std::int32_t>(count);
        impl_->stream_window -= static_cast<std::int32_t>(count);
    }
    return {};
}

Result<Http2Message> Http2::read()
{
    while (impl_->received.empty())
    {
        if (impl_->failed)
        {
            return tl::unexpected(impl_->error);
        }
        if (auto status = impl_->read_frame(); !status)
        {
            return tl::unexpected(status.error());
        }
    }
    Http2Message message = std::move(impl_->received.front());
    impl_->received.pop_front();
    return message;
}

void Http2::close()
{
    if (impl_ != nullptr && impl_->transport != nullptr)
    {
        const std::vector<std::byte> frame = goaway_frame();
        (void)impl_->transport->write(frame);
        impl_->transport = nullptr;
    }
}

} // namespace ioscpp::protocol
