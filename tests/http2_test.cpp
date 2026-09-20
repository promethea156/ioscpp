#include "ioscpp/protocol/http2.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ioscpp/error.hpp"
#include "ioscpp/testing/mock_transport.hpp"
#include "ioscpp/transport.hpp"

using namespace ioscpp;
using namespace ioscpp::protocol;

namespace
{

constexpr std::uint8_t kData = 0x0;
constexpr std::uint8_t kHeaders = 0x1;
constexpr std::uint8_t kSettings = 0x4;
constexpr std::uint8_t kWindowUpdate = 0x8;
constexpr std::uint8_t kEndHeaders = 0x4;

/// One parsed HTTP/2 frame.
struct Frame
{
    std::uint8_t type = 0;
    std::uint8_t flags = 0;
    std::uint32_t stream = 0;
    std::vector<std::byte> payload;
};

/// Builds an HTTP/2 frame: the 3-byte length, the type, the flags, the stream, the payload.
std::vector<std::byte> frame(std::uint8_t type, std::uint8_t flags, std::uint32_t stream,
                             std::span<const std::byte> payload)
{
    std::vector<std::byte> out(9);
    const std::size_t length = payload.size();
    out[0] = static_cast<std::byte>((length >> 16) & 0xff);
    out[1] = static_cast<std::byte>((length >> 8) & 0xff);
    out[2] = static_cast<std::byte>(length & 0xff);
    out[3] = static_cast<std::byte>(type);
    out[4] = static_cast<std::byte>(flags);
    out[5] = static_cast<std::byte>((stream >> 24) & 0xff);
    out[6] = static_cast<std::byte>((stream >> 16) & 0xff);
    out[7] = static_cast<std::byte>((stream >> 8) & 0xff);
    out[8] = static_cast<std::byte>(stream & 0xff);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

/// Builds a SETTINGS frame from `id`/`value` pairs.
std::vector<std::byte> settings_frame(std::initializer_list<std::pair<std::uint16_t, std::uint32_t>> entries)
{
    std::vector<std::byte> payload;
    for (const auto &[id, value] : entries)
    {
        payload.push_back(static_cast<std::byte>((id >> 8) & 0xff));
        payload.push_back(static_cast<std::byte>(id & 0xff));
        payload.push_back(static_cast<std::byte>((value >> 24) & 0xff));
        payload.push_back(static_cast<std::byte>((value >> 16) & 0xff));
        payload.push_back(static_cast<std::byte>((value >> 8) & 0xff));
        payload.push_back(static_cast<std::byte>(value & 0xff));
    }
    return frame(kSettings, 0, 0, payload);
}

/// Builds a WINDOW_UPDATE frame.
std::vector<std::byte> window_update_frame(std::uint32_t stream, std::uint32_t increment)
{
    const std::vector<std::byte> payload = {
        static_cast<std::byte>((increment >> 24) & 0xff),
        static_cast<std::byte>((increment >> 16) & 0xff),
        static_cast<std::byte>((increment >> 8) & 0xff),
        static_cast<std::byte>(increment & 0xff),
    };
    return frame(kWindowUpdate, 0, stream, payload);
}

/// Builds an empty HEADERS frame that opens `stream`.
std::vector<std::byte> headers_frame(std::uint32_t stream)
{
    return frame(kHeaders, kEndHeaders, stream, {});
}

/// Builds a DATA frame on `stream`.
std::vector<std::byte> data_frame(std::uint32_t stream, std::span<const std::byte> payload)
{
    return frame(kData, 0, stream, payload);
}

/// Parses the frames in `bytes` starting at `offset`, stopping at a partial one.
std::vector<Frame> parse_frames(std::span<const std::byte> bytes, std::size_t offset)
{
    std::vector<Frame> frames;
    while (offset + 9 <= bytes.size())
    {
        const std::size_t length = (std::to_integer<std::size_t>(bytes[offset]) << 16) |
                                   (std::to_integer<std::size_t>(bytes[offset + 1]) << 8) |
                                   std::to_integer<std::size_t>(bytes[offset + 2]);
        Frame parsed;
        parsed.type = std::to_integer<std::uint8_t>(bytes[offset + 3]);
        parsed.flags = std::to_integer<std::uint8_t>(bytes[offset + 4]);
        parsed.stream = (std::to_integer<std::uint32_t>(bytes[offset + 5]) << 24) |
                        (std::to_integer<std::uint32_t>(bytes[offset + 6]) << 16) |
                        (std::to_integer<std::uint32_t>(bytes[offset + 7]) << 8) |
                        std::to_integer<std::uint32_t>(bytes[offset + 8]);
        offset += 9;
        if (offset + length > bytes.size())
        {
            break;
        }
        parsed.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                              bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
        offset += length;
        frames.push_back(std::move(parsed));
    }
    return frames;
}

/// The client's bytes written after the preface, as parsed frames.
std::vector<Frame> client_frames(const testing::MockTransport &transport)
{
    return parse_frames(transport.written(), kHttp2Preface.size());
}

} // namespace

TEST_CASE("connect sends the preface, settings, and window update", "[http2]")
{
    testing::MockTransport transport;
    transport.feed(settings_frame({{0x4, 65535}}));

    auto http2 = Http2::connect(transport);
    REQUIRE(http2.has_value());

    const std::vector<std::byte> &written = transport.written();
    REQUIRE(written.size() >= kHttp2Preface.size());
    const std::string preface(reinterpret_cast<const char *>(written.data()), kHttp2Preface.size());
    CHECK(preface == kHttp2Preface);

    const std::vector<Frame> frames = client_frames(transport);
    REQUIRE(frames.size() >= 2);
    CHECK(frames[0].type == kSettings);
    CHECK(frames[0].stream == 0);
    CHECK(frames[1].type == kWindowUpdate);
    CHECK(frames[1].stream == 0);
}

TEST_CASE("a message round-trips over the control stream", "[http2]")
{
    testing::MockTransport transport;
    transport.feed(settings_frame({{0x4, 65535}}));

    auto http2 = Http2::connect(transport);
    REQUIRE(http2.has_value());

    const std::vector<std::byte> request = {std::byte{1}, std::byte{2}, std::byte{3}};
    const std::vector<std::byte> reply = {std::byte{9}, std::byte{8}};
    transport.feed(data_frame(kHttp2ReplyStream, reply));

    // The handshake opens both the control stream and the reply stream, then the
    // reply arrives on the reply stream.
    REQUIRE(http2->open(kHttp2ControlStream).has_value());
    REQUIRE(http2->open(kHttp2ReplyStream).has_value());
    REQUIRE(http2->write(kHttp2ControlStream, request).has_value());

    auto message = http2->read();
    REQUIRE(message.has_value());
    CHECK(message->stream == kHttp2ReplyStream);
    CHECK(message->payload == reply);

    // The client opened the stream with a HEADERS frame and sent the request in a DATA frame.
    const std::vector<Frame> frames = client_frames(transport);
    bool opened = false;
    bool sent = false;
    for (const Frame &parsed : frames)
    {
        if (parsed.type == kHeaders && parsed.stream == kHttp2ControlStream)
        {
            opened = true;
        }
        if (parsed.type == kData && parsed.stream == kHttp2ControlStream)
        {
            CHECK(parsed.payload == request);
            sent = true;
        }
    }
    CHECK(opened);
    CHECK(sent);
}

TEST_CASE("a payload larger than the window is split across DATA frames", "[http2]")
{
    testing::MockTransport transport;
    // The peer grants a small window, so the payload has to wait for the top-up.
    transport.feed(settings_frame({{0x4, 4096}}));

    auto http2 = Http2::connect(transport);
    REQUIRE(http2.has_value());

    transport.feed(window_update_frame(0, 256 * 1024));
    transport.feed(window_update_frame(kHttp2ControlStream, 256 * 1024));

    std::vector<std::byte> payload(128 * 1024);
    for (std::size_t i = 0; i < payload.size(); ++i)
    {
        payload[i] = static_cast<std::byte>(i & 0xff);
    }

    REQUIRE(http2->open(kHttp2ControlStream).has_value());
    REQUIRE(http2->write(kHttp2ControlStream, payload).has_value());

    // The payload arrives intact, across more than one DATA frame, in order.
    std::vector<std::byte> received;
    std::size_t data_frames = 0;
    for (const Frame &parsed : client_frames(transport))
    {
        if (parsed.type == kData && parsed.stream == kHttp2ControlStream)
        {
            ++data_frames;
            received.insert(received.end(), parsed.payload.begin(), parsed.payload.end());
        }
    }
    CHECK(received == payload);
    CHECK(data_frames > 1);
}
