// Fuzzes the CoreDevice tunnel frame codec: the fixed header, the body, and the
// handshake response JSON. The tunnel's stream is boundary-less and its frames are
// length-prefixed, so a crafted length is the case this is here to catch.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "ioscpp/protocol/cdtunnel.hpp"

namespace
{

std::span<const std::byte> as_bytes(const std::uint8_t *data, std::size_t size)
{
    return {reinterpret_cast<const std::byte *>(data), size};
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    const std::span<const std::byte> bytes = as_bytes(data, size);

    (void)ioscpp::protocol::cdtunnel_decode(bytes);
    (void)ioscpp::protocol::CdtunnelResponse::parse(std::string_view(reinterpret_cast<const char *>(data), size));

    if (size >= ioscpp::protocol::kCdtunnelHeaderSize)
    {
        const std::span<const std::byte, ioscpp::protocol::kCdtunnelHeaderSize> header(
            bytes.data(), ioscpp::protocol::kCdtunnelHeaderSize);
        (void)ioscpp::protocol::cdtunnel_header_length(header);
    }
    return 0;
}
