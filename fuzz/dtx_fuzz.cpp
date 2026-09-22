// Fuzzes the DTX codec, the message format a `com.apple.dvt.*` service speaks.
// The header carries the auxiliary and payload sizes, so a crafted size is the case
// this is here to catch.

#include <cstddef>
#include <cstdint>
#include <span>

#include "ioscpp/protocol/dtx.hpp"

namespace
{

std::span<const std::byte> as_bytes(const std::uint8_t *data, std::size_t size)
{
    return {reinterpret_cast<const std::byte *>(data), size};
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    (void)ioscpp::protocol::Dtx::parse(as_bytes(data, size));
    return 0;
}
