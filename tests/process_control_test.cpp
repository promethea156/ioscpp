#include "ioscpp/process_control.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

#include "ioscpp/byte_stream.hpp"
#include "ioscpp/error.hpp"
#include "ioscpp/protocol/dtx.hpp"
#include "ioscpp/protocol/keyed_archive.hpp"
#include "ioscpp/protocol/plist.hpp"

using namespace ioscpp;
using namespace ioscpp::protocol;

namespace
{

std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t offset)
{
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
    {
        value |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + i])) << (8 * i);
    }
    return value;
}

/// An in-memory `ByteStream` a test feeds and inspects.
class MemoryByteStream : public ByteStream
{
public:
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

    void clear_written()
    {
        written_.clear();
    }

private:
    std::vector<std::byte> incoming_;
    std::size_t read_position_ = 0;
    std::vector<std::byte> written_;
};

/// A message the device sends, with the identifier and conversation index set.
Dtx message(std::uint32_t identifier, std::uint32_t conversation_index, std::int32_t channel_code)
{
    Dtx dtx;
    dtx.identifier = identifier;
    dtx.conversation_index = conversation_index;
    dtx.channel_code = channel_code;
    return dtx;
}

/// A device reply carrying `value` as an archived integer.
Dtx pid_reply(std::uint32_t identifier, std::int64_t value)
{
    Dtx dtx = message(identifier, 1, 1);
    dtx.message_type = DtxMessageType::Object;
    dtx.payload = KeyedArchive::archive(Plist(value));
    return dtx;
}

/// Splits a written buffer into its whole frames.
std::vector<std::vector<std::byte>> frames(std::span<const std::byte> bytes)
{
    std::vector<std::vector<std::byte>> out;
    std::size_t offset = 0;
    while (offset + kDtxHeaderSize <= bytes.size())
    {
        // A frame's size is its header size plus its message length, at offsets
        // 4 and 12 of the DTX message header.
        const std::size_t size = read_u32(bytes, offset + 4) + read_u32(bytes, offset + 12);
        out.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                         bytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
        offset += size;
    }
    return out;
}

/// Feeds the capability handshake and the channel reply a `start` expects.
void feed_open(MemoryByteStream &stream, std::uint32_t handshake_id, std::uint32_t reply_id)
{
    Dtx capabilities = message(handshake_id, 0, kDtxGlobalChannel);
    capabilities.expects_reply = true;
    capabilities.message_type = DtxMessageType::Dispatch;
    capabilities.payload = KeyedArchive::archive(Plist("_notifyOfPublishedCapabilities:"));
    stream.feed(capabilities.encode());
    stream.feed(message(reply_id, 1, kDtxGlobalChannel).encode());
}

} // namespace

TEST_CASE("a process control opens the process-control channel", "[process_control]")
{
    MemoryByteStream stream;
    feed_open(stream, 1, 1);

    auto control = ProcessControl::start(stream);
    REQUIRE(control.has_value());

    const std::vector<std::vector<std::byte>> written = frames(stream.written());
    REQUIRE(written.size() == 2);

    // The channel request is written first, then the handshake that arrived while
    // its reply was awaited is acknowledged.
    auto request = Dtx::parse(written[0]);
    REQUIRE(request.has_value());
    CHECK(request->identifier == 1);
    CHECK(request->channel_code == kDtxGlobalChannel);
    CHECK(request->message_type == DtxMessageType::Dispatch);
    auto selector = KeyedArchive::unarchive(request->payload);
    REQUIRE(selector.has_value());
    CHECK(selector->string_or() == kDtxRequestChannelSelector);
    REQUIRE(request->auxiliary.size() == 2);
    CHECK(request->auxiliary[0].int32() == 1);
    REQUIRE(request->auxiliary[1].buffer().has_value());
    auto identifier = KeyedArchive::unarchive(*request->auxiliary[1].buffer());
    REQUIRE(identifier.has_value());
    CHECK(identifier->string_or() == kProcessControlChannel);

    auto ack = Dtx::parse(written[1]);
    REQUIRE(ack.has_value());
    CHECK(ack->identifier == 1);
    CHECK(ack->conversation_index == 1);
    CHECK(ack->message_type == DtxMessageType::Ok);
}

TEST_CASE("a process control launches an app and returns its pid", "[process_control]")
{
    MemoryByteStream stream;
    feed_open(stream, 1, 1);
    stream.feed(pid_reply(2, 1234).encode());

    auto control = ProcessControl::start(stream);
    REQUIRE(control.has_value());
    stream.clear_written();

    auto pid = control->launch("com.example.app");
    REQUIRE(pid.has_value());
    CHECK(*pid == 1234);

    const std::vector<std::vector<std::byte>> written = frames(stream.written());
    REQUIRE(written.size() == 1);
    auto request = Dtx::parse(written[0]);
    REQUIRE(request.has_value());
    CHECK(request->identifier == 2);
    CHECK(request->channel_code == 1);
    CHECK(request->message_type == DtxMessageType::Dispatch);
    auto selector = KeyedArchive::unarchive(request->payload);
    REQUIRE(selector.has_value());
    CHECK(selector->string_or() ==
          "launchSuspendedProcessWithDevicePath:bundleIdentifier:environment:arguments:options:");

    REQUIRE(request->auxiliary.size() == 5);
    REQUIRE(request->auxiliary[0].buffer().has_value());
    auto path = KeyedArchive::unarchive(*request->auxiliary[0].buffer());
    REQUIRE(path.has_value());
    CHECK(path->string_or() == "/private/");
    REQUIRE(request->auxiliary[1].buffer().has_value());
    auto bundle = KeyedArchive::unarchive(*request->auxiliary[1].buffer());
    REQUIRE(bundle.has_value());
    CHECK(bundle->string_or() == "com.example.app");
}

TEST_CASE("a process control resolves a bundle id to a running process", "[process_control]")
{
    MemoryByteStream stream;
    feed_open(stream, 1, 1);
    stream.feed(pid_reply(2, 0).encode());
    stream.feed(pid_reply(3, 0).encode());

    auto control = ProcessControl::start(stream);
    REQUIRE(control.has_value());
    stream.clear_written();

    auto pid = control->process_identifier("com.example.app");
    REQUIRE(pid.has_value());
    CHECK(*pid == 0);
    CHECK_FALSE(control->is_running("com.example.app").value_or(true));

    // Both calls resolve the bundle id, so each writes one request.
    const std::vector<std::vector<std::byte>> written = frames(stream.written());
    REQUIRE(written.size() == 2);
    auto request = Dtx::parse(written[0]);
    REQUIRE(request.has_value());
    auto selector = KeyedArchive::unarchive(request->payload);
    REQUIRE(selector.has_value());
    CHECK(selector->string_or() == "processIdentifierForBundleIdentifier:");
    REQUIRE(request->auxiliary.size() == 1);
    REQUIRE(request->auxiliary[0].buffer().has_value());
    auto bundle = KeyedArchive::unarchive(*request->auxiliary[0].buffer());
    REQUIRE(bundle.has_value());
    CHECK(bundle->string_or() == "com.example.app");
}

TEST_CASE("a process control kills a process with a signal", "[process_control]")
{
    MemoryByteStream stream;
    feed_open(stream, 1, 1);
    stream.feed(message(2, 1, 1).encode());

    auto control = ProcessControl::start(stream);
    REQUIRE(control.has_value());
    stream.clear_written();

    REQUIRE(control->kill(1234).has_value());

    const std::vector<std::vector<std::byte>> written = frames(stream.written());
    REQUIRE(written.size() == 1);
    auto request = Dtx::parse(written[0]);
    REQUIRE(request.has_value());
    CHECK(request->identifier == 2);
    CHECK(request->channel_code == 1);
    auto selector = KeyedArchive::unarchive(request->payload);
    REQUIRE(selector.has_value());
    CHECK(selector->string_or() == "sendSignal:toPid:");

    REQUIRE(request->auxiliary.size() == 2);
    REQUIRE(request->auxiliary[0].buffer().has_value());
    auto signal = KeyedArchive::unarchive(*request->auxiliary[0].buffer());
    REQUIRE(signal.has_value());
    CHECK(signal->integer() == 9);
    REQUIRE(request->auxiliary[1].buffer().has_value());
    auto pid = KeyedArchive::unarchive(*request->auxiliary[1].buffer());
    REQUIRE(pid.has_value());
    CHECK(pid->integer() == 1234);
}

TEST_CASE("a process control reports a refused launch as a device error", "[process_control]")
{
    MemoryByteStream stream;
    feed_open(stream, 1, 1);
    Dtx error = message(2, 1, 1);
    error.message_type = DtxMessageType::Error;
    error.payload = KeyedArchive::archive(Plist("launch failed"));
    stream.feed(error.encode());

    auto control = ProcessControl::start(stream);
    REQUIRE(control.has_value());

    auto pid = control->launch("com.example.app");
    REQUIRE_FALSE(pid.has_value());
    CHECK(pid.error().code == ErrorCode::Device);
}
