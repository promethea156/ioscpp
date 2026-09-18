#include "ioscpp/connection.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "ioscpp/protocol/plist.hpp"
#include "ioscpp/protocol/usbmux.hpp"
#include "ioscpp/stream.hpp"

namespace ioscpp
{
namespace
{

Error protocol_error(std::string message)
{
    return Error{ErrorCode::Protocol, std::move(message)};
}

/// Writes a little-endian 32-bit word, for the `usbmuxd` socket protocol.
void put_le32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) noexcept
{
    bytes[offset + 0] = static_cast<std::byte>(value & 0xff);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0xff);
    bytes[offset + 2] = static_cast<std::byte>((value >> 16) & 0xff);
    bytes[offset + 3] = static_cast<std::byte>((value >> 24) & 0xff);
}

std::uint32_t get_le32(std::span<const std::byte> bytes, std::size_t offset) noexcept
{
    return static_cast<std::uint32_t>(bytes[offset + 0]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

/// Reads exactly `buffer.size()` bytes, or fails.
Status read_exact(Transport &transport, std::span<std::byte> buffer)
{
    std::size_t offset = 0;
    while (offset < buffer.size())
    {
        auto read = transport.read(buffer.subspan(offset));
        if (!read)
        {
            return tl::unexpected(read.error());
        }
        if (*read == 0)
        {
            return tl::unexpected(protocol_error("the link closed mid-message"));
        }
        offset += *read;
    }
    return {};
}

/// The `usbmuxd` message types and version that matter here.
constexpr std::uint32_t kMessageResult = 1;
constexpr std::uint32_t kMessagePlist = 8;
constexpr std::uint32_t kPlistVersion = 1;
constexpr std::uint32_t kResultOk = 0;

} // namespace

Connection::Connection(Transport &transport)
    : session_(transport)
{
}

Result<Connection> Connection::open(Transport &transport)
{
    Connection connection(transport);

    if (transport.is_usbmuxd())
    {
        // The daemon speaks a plist handshake, and the socket is already open.
        // `ListDevices` gives the device id that a later `Connect` needs.
        protocol::Plist::Dictionary request{{"MessageType", protocol::Plist("ListDevices")}};
        auto answer = connection.muxd_request(protocol::Plist::dictionary(std::move(request)));
        if (!answer)
        {
            return tl::unexpected(answer.error());
        }

        const protocol::Plist *list = answer->find("DeviceList");
        if (list == nullptr || list->array() == nullptr || list->array()->empty())
        {
            return tl::unexpected(Error{ErrorCode::Transport, "no device is attached to usbmuxd"});
        }
        const protocol::Plist *id = list->array()->front().find("DeviceID");
        if (id == nullptr || !id->integer().has_value())
        {
            return tl::unexpected(protocol_error("the usbmuxd device list has no DeviceID"));
        }

        connection.usbmuxd_ = true;
        connection.device_id_ = static_cast<std::uint32_t>(*id->integer());
        connection.open_transport_ = [&transport]() -> Result<std::unique_ptr<Transport>>
        {
            return transport.reopen();
        };
        return connection;
    }

    // A direct link negotiates a mux version. The first request is written with
    // the v1 header, because the version is not known yet, which is exactly what
    // `usbmuxd` does (`dev->version` is 0 until the device answers).
    const protocol::VersionHeader host_version;
    if (Status status = connection.session_.send(protocol::MuxProtocol::Version, host_version.encode()); !status)
    {
        return tl::unexpected(status.error());
    }

    auto answer = connection.session_.receive();
    if (!answer)
    {
        return tl::unexpected(answer.error());
    }
    if (answer->header.protocol != static_cast<std::uint32_t>(protocol::MuxProtocol::Version) ||
        answer->payload.size() < protocol::VersionHeader::kSize)
    {
        return tl::unexpected(protocol_error("the device did not answer the version request"));
    }

    const protocol::VersionHeader device_version =
        protocol::VersionHeader::decode(std::span<const std::byte, protocol::VersionHeader::kSize>(
            answer->payload.data(), protocol::VersionHeader::kSize));

    if (device_version.major != 1 && device_version.major != 2)
    {
        return tl::unexpected(protocol_error("the device reported an unknown mux version"));
    }

    connection.session_.set_version(device_version.major);
    if (device_version.major >= 2)
    {
        // The v2 setup packet enables the v2 framing and resets the sequences.
        connection.session_.reset_sequences();
        const std::array<std::byte, 1> setup{std::byte{0x07}};
        if (Status status = connection.session_.send(protocol::MuxProtocol::Setup, setup); !status)
        {
            return tl::unexpected(status.error());
        }
        // The device brings up the userspace session asynchronously, so a short
        // pause before the first connection keeps the first frame from racing it.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    connection.negotiated_ = true;
    return connection;
}

Status Connection::send_tcp(const protocol::TcpHeader &header, std::span<const std::byte> payload)
{
    const std::array<std::byte, protocol::kTcpHeaderSize> encoded = header.encode();
    std::vector<std::byte> frame(encoded.begin(), encoded.end());
    frame.insert(frame.end(), payload.begin(), payload.end());
    return session_.send(protocol::MuxProtocol::Tcp, frame);
}

Result<Frame> Connection::receive()
{
    return session_.receive();
}

Result<protocol::Plist> Connection::muxd_request(protocol::Plist request)
{
    const std::string body = request.to_xml();
    const std::size_t length = 16 + body.size();

    std::vector<std::byte> message(length);
    put_le32(message, 0, static_cast<std::uint32_t>(length));
    put_le32(message, 4, kPlistVersion);
    put_le32(message, 8, kMessagePlist);
    put_le32(message, 12, 1); // tag
    std::copy(reinterpret_cast<const std::byte *>(body.data()),
              reinterpret_cast<const std::byte *>(body.data() + body.size()), message.begin() + 16);

    if (Status status = session_.transport().write(message); !status)
    {
        return tl::unexpected(status.error());
    }

    // The answer is a length prefix, a header, and the plist.
    std::array<std::byte, 4> length_bytes{};
    if (Status status = read_exact(session_.transport(), length_bytes); !status)
    {
        return tl::unexpected(status.error());
    }
    const std::uint32_t answer_length = get_le32(length_bytes, 0);
    if (answer_length < 16)
    {
        return tl::unexpected(protocol_error("the usbmuxd answer is too short"));
    }

    std::vector<std::byte> answer(answer_length - 4);
    if (Status status = read_exact(session_.transport(), answer); !status)
    {
        return tl::unexpected(status.error());
    }

    const std::uint32_t message_type = get_le32(answer, 4);
    if (message_type == kMessageResult)
    {
        if (get_le32(answer, 12) != kResultOk)
        {
            return tl::unexpected(Error{ErrorCode::Device, "usbmuxd refused the request"});
        }
        return protocol::Plist();
    }

    return protocol::Plist::parse(std::span<const std::byte>(answer).subspan(12));
}

} // namespace ioscpp
