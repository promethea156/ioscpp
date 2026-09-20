#include "ioscpp/plist_service.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ioscpp
{
namespace
{

Error protocol_error(std::string message)
{
    return Error{ErrorCode::Protocol, std::move(message)};
}

} // namespace

Status PlistService::send(const protocol::Plist &message)
{
    const std::string body = message.to_xml();
    // The 4-byte big-endian length prefix is the plist size alone, not
    // including the prefix itself. This is `internal_plist_send`:
    //   https://github.com/libimobiledevice/libimobiledevice/blob/master/src/property_list_service.c
    std::vector<std::byte> frame(4 + body.size());
    const std::size_t length = body.size();
    frame[0] = static_cast<std::byte>((length >> 24) & 0xff);
    frame[1] = static_cast<std::byte>((length >> 16) & 0xff);
    frame[2] = static_cast<std::byte>((length >> 8) & 0xff);
    frame[3] = static_cast<std::byte>(length & 0xff);
    std::copy(reinterpret_cast<const std::byte *>(body.data()),
              reinterpret_cast<const std::byte *>(body.data() + body.size()), frame.begin() + 4);
    return stream_->write(frame);
}

Result<protocol::Plist> PlistService::receive()
{
    std::array<std::byte, 4> length_bytes{};
    if (Status status = stream_->read_exact(length_bytes); !status)
    {
        return tl::unexpected(status.error());
    }
    const std::uint32_t length =
        (static_cast<std::uint32_t>(length_bytes[0]) << 24) | (static_cast<std::uint32_t>(length_bytes[1]) << 16) |
        (static_cast<std::uint32_t>(length_bytes[2]) << 8) | static_cast<std::uint32_t>(length_bytes[3]);
    if (length == 0)
    {
        return tl::unexpected(protocol_error("the service sent an empty plist"));
    }
    std::vector<std::byte> body(length);
    if (Status status = stream_->read_exact(body); !status)
    {
        return tl::unexpected(status.error());
    }
    return protocol::Plist::parse(body);
}

} // namespace ioscpp
