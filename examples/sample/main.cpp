// A device-free walkthrough of the mux layer.
//
// The `MockTransport` stands in for the device's USB interface: bytes fed into it
// are what the device would send, and bytes written to it are what the library
// would put on the wire. This is the same transport the unit tests use.

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

#include "ioscpp/connection.hpp"
#include "ioscpp/error.hpp"
#include "ioscpp/protocol/usbmux.hpp"
#include "ioscpp/session.hpp"
#include "ioscpp/testing/mock_transport.hpp"

int main()
{
    using namespace ioscpp;
    using namespace ioscpp::protocol;

    testing::MockTransport transport;

    // The device answers the version request with version 1.0, so the library
    // keeps the 8-byte v1 header and sends no setup packet.
    VersionHeader version;
    version.major = 1;
    MuxHeader header;
    header.protocol = static_cast<std::uint32_t>(MuxProtocol::Version);
    header.length = static_cast<std::uint32_t>(kMuxHeaderSizeV1 + VersionHeader::kSize);

    const std::array<std::byte, kMuxHeaderSize> header_bytes = header.encode(0);
    std::vector<std::byte> answer(header_bytes.begin(), header_bytes.begin() + kMuxHeaderSizeV1);
    const auto version_bytes = version.encode();
    answer.insert(answer.end(), version_bytes.begin(), version_bytes.end());
    transport.feed(answer);

    auto connection = Connection::open(transport);
    if (!connection)
    {
        std::cerr << "connect: " << connection.error().message << "\n";
        return 1;
    }

    std::cout << "negotiated mux version " << connection->version() << "\n";
    std::cout << "the library wrote " << transport.written().size() << " bytes\n";
    return 0;
}
