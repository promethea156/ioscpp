// Logging is opt-in and process-wide: a sink is installed with `set_logger`, and
// the library writes protocol events to it. These tests pin down the levels, the
// opt-in behavior, and that no frame payload reaches the sink.

#include "ioscpp/log.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ioscpp/connection.hpp"
#include "ioscpp/protocol/usbmux.hpp"
#include "ioscpp/session.hpp"
#include "ioscpp/stream.hpp"
#include "ioscpp/testing/mock_transport.hpp"

using namespace ioscpp;
using namespace ioscpp::protocol;

namespace
{

// Collects the messages a test's sink receives, and removes the sink when it goes
// out of scope, so one test's logger does not leak into the next.
class LogCapture
{
public:
    explicit LogCapture(LogLevel level = LogLevel::Trace)
    {
        set_logger(
            [this](LogLevel message_level, std::string_view message)
            {
                messages.emplace_back(message_level, std::string(message));
            },
            level);
    }

    ~LogCapture()
    {
        clear_logger();
    }

    LogCapture(const LogCapture &) = delete;
    LogCapture &operator=(const LogCapture &) = delete;

    /// Whether any message contains `needle`.
    bool contains(std::string_view needle) const
    {
        for (const auto &[level, message] : messages)
        {
            if (message.find(needle) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    /// Whether any message contains the byte `needle`.
    bool contains_byte(std::byte needle) const
    {
        for (const auto &[level, message] : messages)
        {
            if (message.find(static_cast<char>(needle)) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    std::vector<std::pair<LogLevel, std::string>> messages;
};

/// Builds a mux frame carrying `protocol` and `payload`.
std::vector<std::byte> frame(std::uint32_t protocol, std::span<const std::byte> payload)
{
    MuxHeader header;
    header.protocol = protocol;
    header.length = static_cast<std::uint32_t>(kMuxHeaderSizeV1 + payload.size());
    const std::array<std::byte, kMuxHeaderSize> header_bytes = header.encode(0);

    std::vector<std::byte> bytes(header_bytes.begin(), header_bytes.begin() + kMuxHeaderSizeV1);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

/// Feeds a version-1.0 handshake answer, so no setup packet is needed.
void feed_version(testing::MockTransport &transport)
{
    VersionHeader version;
    version.major = 1;
    transport.feed(frame(static_cast<std::uint32_t>(MuxProtocol::Version), version.encode()));
}

/// Builds a mux frame carrying a TCP header with `flags`.
std::vector<std::byte> tcp_frame(std::uint16_t source, std::uint16_t destination, std::uint8_t flags)
{
    TcpHeader tcp;
    tcp.source_port = source;
    tcp.destination_port = destination;
    tcp.flags = flags;
    return frame(static_cast<std::uint32_t>(MuxProtocol::Tcp), tcp.encode());
}

/// The local port the first stream is allocated, matching `Connection::next_port_`.
constexpr std::uint16_t kLocalPort = 1;
constexpr std::uint16_t kPort = 62078;

} // namespace

TEST_CASE("logging is off until a sink is installed", "[log]")
{
    clear_logger();

    REQUIRE_FALSE(is_logging(LogLevel::Error));

    // With no sink, `log` does nothing rather than failing.
    log(LogLevel::Error, "nothing");
}

TEST_CASE("a sink receives the levels at or below its level", "[log]")
{
    LogCapture capture(LogLevel::Warning);

    REQUIRE(is_logging(LogLevel::Error));
    REQUIRE(is_logging(LogLevel::Warning));
    REQUIRE_FALSE(is_logging(LogLevel::Info));

    log(LogLevel::Error, "an error");
    log(LogLevel::Warning, "a warning");
    log(LogLevel::Info, "an info");

    REQUIRE(capture.contains("an error"));
    REQUIRE(capture.contains("a warning"));
    REQUIRE_FALSE(capture.contains("an info"));
}

TEST_CASE("a sink sees the frames of the mux handshake", "[log]")
{
    LogCapture capture;
    testing::MockTransport transport;
    feed_version(transport);

    auto connection = Connection::open(transport);
    REQUIRE(connection.has_value());

    REQUIRE(capture.contains("mux send"));
    REQUIRE(capture.contains("mux recv"));
    REQUIRE(capture.contains("mux version 1.0"));
    REQUIRE(capture.contains("connected"));
}

TEST_CASE("a sink sees a stream open and a refusal", "[log]")
{
    LogCapture capture;
    testing::MockTransport transport;
    feed_version(transport);
    auto opened = Connection::open(transport);
    REQUIRE(opened.has_value());
    auto connection = std::make_shared<Connection>(std::move(*opened));

    transport.feed(tcp_frame(kPort, kLocalPort, TcpSyn | TcpAck));
    auto stream = Stream::open(connection, kPort);
    REQUIRE(stream.has_value());
    REQUIRE(capture.contains("opened stream"));

    // The second stream allocates the next local port.
    transport.feed(tcp_frame(kPort, kLocalPort + 1, TcpRst));
    auto refused = Stream::open(connection, kPort);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(capture.contains("refused the port"));
}

TEST_CASE("a sink never sees a frame payload", "[log]")
{
    LogCapture capture;
    testing::MockTransport transport;
    Session session(transport);

    // The payload is a distinctive byte, so the test can tell whether it reached
    // the sink.
    const std::vector<std::byte> payload(32, std::byte{0xAA});
    transport.feed(frame(static_cast<std::uint32_t>(MuxProtocol::Tcp), payload));

    auto received = session.receive();
    REQUIRE(received.has_value());

    REQUIRE(capture.contains("mux recv"));
    REQUIRE_FALSE(capture.contains_byte(std::byte{0xAA}));
}
