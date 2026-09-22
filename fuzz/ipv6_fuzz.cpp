// Fuzzes the IPv6 re-framer and the fixed header it decodes. The tunnel hands
// over a raw packet stream with no boundaries, so the framer reads a 16-bit
// payload length and trusts it; a crafted length is the case this is here to catch.

#include <cstddef>
#include <cstdint>
#include <span>

#include "ioscpp/protocol/ipv6.hpp"
#include "ioscpp/testing/mock_transport.hpp"

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

    if (size >= ioscpp::protocol::kIpv6HeaderSize)
    {
        const std::span<const std::byte, ioscpp::protocol::kIpv6HeaderSize> header(bytes.data(),
                                                                                   ioscpp::protocol::kIpv6HeaderSize);
        (void)ioscpp::protocol::Ipv6Header::decode(header);
    }

    ioscpp::testing::MockTransport transport;
    transport.feed(bytes);
    ioscpp::protocol::Ipv6Framer framer(transport);

    // Each packet consumes at least a header, so the finite input bounds the loop.
    const std::size_t limit = size / ioscpp::protocol::kIpv6HeaderSize + 1;
    for (std::size_t i = 0; i < limit; ++i)
    {
        if (!framer.read_packet().has_value())
        {
            break;
        }
    }
    return 0;
}
