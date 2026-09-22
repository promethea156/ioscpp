// Fuzzes the plist codec, the parser every `lockdownd` and
// `installation_proxy` message goes through. `Plist::parse` chooses the XML or
// the binary form on the leading bytes, so the two form-specific parsers are
// fuzzed directly as well.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "ioscpp/protocol/plist.hpp"

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
    const std::string_view text(reinterpret_cast<const char *>(data), size);

    (void)ioscpp::protocol::Plist::parse(bytes);
    (void)ioscpp::protocol::Plist::parse_binary(bytes);
    (void)ioscpp::protocol::Plist::parse_xml(text);
    return 0;
}
