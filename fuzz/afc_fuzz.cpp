// Fuzzes the AFC packet parser, the file service's `CFA6LPAA` codec. The reply
// header carries an `entire_length`, and the `DATA` payload is a run of
// NUL-terminated tokens, so a crafted length or a missing NUL is the case this is
// here to catch. The parser sits above a `ByteStream`, so an in-memory stream
// stands in for the mux link.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "ioscpp/afc.hpp"
#include "ioscpp/byte_stream.hpp"
#include "ioscpp/error.hpp"

namespace
{

constexpr std::array<char, 8> kMagic{'C', 'F', 'A', '6', 'L', 'P', 'A', 'A'};
constexpr std::size_t kHeaderSize = 40;
constexpr std::uint64_t kOpData = 0x02;

std::span<const std::byte> as_bytes(const std::uint8_t *data, std::size_t size)
{
    return {reinterpret_cast<const std::byte *>(data), size};
}

/// An in-memory `ByteStream` that answers with the queued bytes, then fails.
class FuzzByteStream : public ioscpp::ByteStream
{
public:
    explicit FuzzByteStream(std::span<const std::byte> incoming)
        : incoming_(incoming.begin(), incoming.end())
    {
    }

    ioscpp::Status read_exact(std::span<std::byte> buffer) override
    {
        if (position_ + buffer.size() > incoming_.size())
        {
            return tl::unexpected(ioscpp::Error{ioscpp::ErrorCode::Protocol, "the fuzz stream ended early"});
        }
        std::copy_n(incoming_.begin() + static_cast<std::ptrdiff_t>(position_),
                    static_cast<std::ptrdiff_t>(buffer.size()), buffer.begin());
        position_ += buffer.size();
        return {};
    }

    ioscpp::Status write(std::span<const std::byte>) override
    {
        return {};
    }

    void close() override
    {
    }

private:
    std::vector<std::byte> incoming_;
    std::size_t position_ = 0;
};

/// Wraps `payload` in a valid AFC `DATA` packet, with packet number 1.
std::vector<std::byte> afc_data_packet(std::span<const std::byte> payload)
{
    std::vector<std::byte> packet(kHeaderSize + payload.size());
    std::memcpy(packet.data(), kMagic.data(), kMagic.size());
    const auto write = [&](std::size_t offset, std::uint64_t value)
    {
        for (std::size_t i = 0; i < 8; ++i)
        {
            packet[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xff);
        }
    };
    write(8, packet.size());
    write(16, packet.size());
    write(24, 1);
    write(32, kOpData);
    std::copy(payload.begin(), payload.end(), packet.begin() + static_cast<std::ptrdiff_t>(kHeaderSize));
    return packet;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    const std::span<const std::byte> bytes = as_bytes(data, size);
    const std::vector<std::byte> packet = afc_data_packet(bytes);

    // A valid `DATA` reply drives the directory and stat token parsers.
    {
        FuzzByteStream stream(packet);
        auto afc = ioscpp::Afc::start(stream);
        if (afc.has_value())
        {
            (void)afc->list("/");
        }
    }
    {
        FuzzByteStream stream(packet);
        auto afc = ioscpp::Afc::start(stream);
        if (afc.has_value())
        {
            (void)afc->stat("/");
        }
    }

    // The raw bytes drive the reply header parser.
    {
        FuzzByteStream stream(bytes);
        auto afc = ioscpp::Afc::start(stream);
        if (afc.has_value())
        {
            (void)afc->list("/");
        }
    }
    return 0;
}
