// Fuzzes the RemoteXPC codec: the object codec and the payload and wrapper
// that carry it over the CoreDevice tunnel. A wrapper's body length decides how
// much is read, so a crafted length is the case this is here to catch.

#include <cstddef>
#include <cstdint>
#include <span>

#include "ioscpp/protocol/remotexpc.hpp"

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

    (void)ioscpp::protocol::Xpc::parse(bytes);
    (void)ioscpp::protocol::XpcPayload::parse(bytes);
    (void)ioscpp::protocol::XpcWrapper::parse(bytes);
    return 0;
}
