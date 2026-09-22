#include "ioscpp/house_arrest.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ioscpp/byte_stream.hpp"
#include "ioscpp/error.hpp"
#include "ioscpp/protocol/plist.hpp"

using namespace ioscpp;
using namespace ioscpp::protocol;

namespace
{

constexpr std::array<char, 8> kMagic{'C', 'F', 'A', '6', 'L', 'P', 'A', 'A'};

/// An in-memory `ByteStream` a test feeds and inspects.
class MemoryByteStream : public ByteStream
{
public:
    /// Queues `data` to be returned by subsequent `read_exact` calls.
    void feed(std::span<const std::byte> data)
    {
        incoming_.insert(incoming_.end(), data.begin(), data.end());
    }

    Status read_exact(std::span<std::byte> buffer) override
    {
        if (read_position_ + buffer.size() > incoming_.size())
        {
            return tl::unexpected(Error{ErrorCode::Io, "the stream ended early"});
        }
        std::copy_n(incoming_.begin() + static_cast<std::ptrdiff_t>(read_position_),
                    static_cast<std::ptrdiff_t>(buffer.size()), buffer.begin());
        read_position_ += buffer.size();
        return {};
    }

    Status write(std::span<const std::byte> data) override
    {
        written_.insert(written_.end(), data.begin(), data.end());
        return {};
    }

    void close() override
    {
    }

    const std::vector<std::byte> &written() const noexcept
    {
        return written_;
    }

private:
    std::vector<std::byte> incoming_;
    std::size_t read_position_ = 0;
    std::vector<std::byte> written_;
};

/// Frames `xml` the way a service does: the length, then the bytes.
std::vector<std::byte> framed(std::string_view xml)
{
    std::vector<std::byte> frame(4 + xml.size());
    const std::size_t length = xml.size();
    frame[0] = static_cast<std::byte>((length >> 24) & 0xff);
    frame[1] = static_cast<std::byte>((length >> 16) & 0xff);
    frame[2] = static_cast<std::byte>((length >> 8) & 0xff);
    frame[3] = static_cast<std::byte>(length & 0xff);
    std::copy(reinterpret_cast<const std::byte *>(xml.data()),
              reinterpret_cast<const std::byte *>(xml.data() + xml.size()), frame.begin() + 4);
    return frame;
}

/// The vend answer a device sends for a bundle it accepted.
std::vector<std::byte> vend_success()
{
    return framed(Plist::dictionary({{"Status", Plist("Success")}}).to_xml());
}

/// The first length-prefixed plist in `written`, which is the vend command.
Plist first_plist(const std::vector<std::byte> &written)
{
    REQUIRE(written.size() >= 4);
    const std::uint32_t length =
        (std::to_integer<std::uint32_t>(written[0]) << 24) | (std::to_integer<std::uint32_t>(written[1]) << 16) |
        (std::to_integer<std::uint32_t>(written[2]) << 8) | std::to_integer<std::uint32_t>(written[3]);
    REQUIRE(written.size() >= 4 + length);
    auto parsed = Plist::parse(std::span<const std::byte>(written.data() + 4, length));
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

/// Builds an AFC packet with `operation` and `payload`, with packet number 1.
std::vector<std::byte> afc_packet(std::uint64_t operation, std::span<const std::byte> payload)
{
    std::vector<std::byte> packet(40 + payload.size());
    std::memcpy(packet.data(), kMagic.data(), kMagic.size());
    std::size_t position = 8;
    auto write = [&](std::uint64_t value)
    {
        for (std::size_t i = 0; i < 8; ++i)
        {
            packet[position + i] = static_cast<std::byte>((value >> (8 * i)) & 0xff);
        }
        position += 8;
    };
    write(packet.size());
    write(packet.size());
    write(1);
    write(operation);
    std::copy(payload.begin(), payload.end(), packet.begin() + 40);
    return packet;
}

/// A string with its embedded NULs kept, which `std::string(const char *)` drops.
template <std::size_t N>
std::string bytes_of(const char (&text)[N])
{
    return std::string(text, N - 1);
}

} // namespace

TEST_CASE("house arrest vends a container and reads it with AFC", "[house_arrest]")
{
    MemoryByteStream stream;
    stream.feed(vend_success());

    auto house_arrest = HouseArrest::start(stream, "com.example.app");
    REQUIRE(house_arrest.has_value());

    // The vend command names the app and the whole container.
    const Plist command = first_plist(stream.written());
    REQUIRE(command.find("Command") != nullptr);
    CHECK(command.find("Command")->string_or() == "VendContainer");
    REQUIRE(command.find("Identifier") != nullptr);
    CHECK(command.find("Identifier")->string_or() == "com.example.app");

    // The AFC session is rooted at the container: a READ_DIR answer is the entries.
    const std::string body = bytes_of("Documents\000Library\000");
    std::vector<std::byte> payload(reinterpret_cast<const std::byte *>(body.data()),
                                   reinterpret_cast<const std::byte *>(body.data() + body.size()));
    stream.feed(afc_packet(0x02, payload));

    auto entries = house_arrest->afc().list("/");
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 2);
    CHECK((*entries)[0].name == "Documents");
    CHECK((*entries)[1].name == "Library");
}

TEST_CASE("house arrest vends only Documents when asked", "[house_arrest]")
{
    MemoryByteStream stream;
    stream.feed(vend_success());

    auto house_arrest = HouseArrest::start(stream, "com.example.app", true);
    REQUIRE(house_arrest.has_value());

    const Plist command = first_plist(stream.written());
    REQUIRE(command.find("Command") != nullptr);
    CHECK(command.find("Command")->string_or() == "VendDocuments");
}

TEST_CASE("house arrest reports an uninstalled app as a device error", "[house_arrest]")
{
    MemoryByteStream stream;
    stream.feed(framed(Plist::dictionary({{"Error", Plist("ApplicationLookupFailed")}}).to_xml()));

    auto house_arrest = HouseArrest::start(stream, "com.example.missing");
    REQUIRE_FALSE(house_arrest.has_value());
    CHECK(house_arrest.error().code == ErrorCode::Device);
}
