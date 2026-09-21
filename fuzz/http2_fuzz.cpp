// Fuzzes the hand-rolled HTTP/2 layer that carries RemoteXPC over the tunnel.
// A frame's 3-byte length decides how much is read next, so a crafted length is
// the case this is here to catch.

#include <cstddef>
#include <cstdint>
#include <span>

#include "ioscpp/protocol/http2.hpp"
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
    ioscpp::testing::MockTransport transport;
    transport.feed(as_bytes(data, size));

    auto http2 = ioscpp::protocol::Http2::connect(transport);
    if (!http2.has_value())
    {
        return 0;
    }

    // Each frame consumes at least its 9-byte header, so the finite input bounds
    // the loop.
    const std::size_t limit = size / 9 + 1;
    for (std::size_t i = 0; i < limit; ++i)
    {
        if (!http2->read().has_value())
        {
            break;
        }
    }
    return 0;
}
