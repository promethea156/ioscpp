#include "ioscpp/plist_service.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ioscpp/byte_stream.hpp"
#include "ioscpp/error.hpp"
#include "ioscpp/protocol/plist.hpp"

using namespace ioscpp;
using namespace ioscpp::protocol;

namespace
{

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

/// The 4-byte big-endian length prefix of `frame`.
std::uint32_t prefix(std::span<const std::byte> frame)
{
    return (std::to_integer<std::uint32_t>(frame[0]) << 24) | (std::to_integer<std::uint32_t>(frame[1]) << 16) |
           (std::to_integer<std::uint32_t>(frame[2]) << 8) | std::to_integer<std::uint32_t>(frame[3]);
}

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

} // namespace

TEST_CASE("a plist service frames its message with the plist size alone", "[plist_service]")
{
    MemoryByteStream stream;
    PlistService service(stream);

    const Plist message = Plist::dictionary({{"Command", Plist("Install")}});
    const std::string xml = message.to_xml();

    REQUIRE(service.send(message).has_value());

    const std::vector<std::byte> &written = stream.written();
    REQUIRE(written.size() == 4 + xml.size());
    CHECK(prefix(written) == xml.size());
    CHECK(std::string(reinterpret_cast<const char *>(written.data() + 4), xml.size()) == xml);
}

TEST_CASE("a plist service reads a length-prefixed plist", "[plist_service]")
{
    const Plist message = Plist::dictionary({{"Status", Plist("Complete")}});
    MemoryByteStream stream;
    stream.feed(framed(message.to_xml()));

    PlistService service(stream);
    auto received = service.receive();

    REQUIRE(received.has_value());
    REQUIRE(received->find("Status") != nullptr);
    CHECK(received->find("Status")->string_or() == "Complete");
}

TEST_CASE("a plist service rejects an empty plist", "[plist_service]")
{
    const std::vector<std::byte> empty(4);
    MemoryByteStream stream;
    stream.feed(empty);

    PlistService service(stream);
    auto received = service.receive();

    REQUIRE_FALSE(received.has_value());
    CHECK(received.error().code == ErrorCode::Protocol);
}
