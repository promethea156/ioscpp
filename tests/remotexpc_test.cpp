#include "ioscpp/protocol/remotexpc.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace ioscpp;
using namespace ioscpp::protocol;

namespace
{

/// Decodes a hex string into a byte vector, two characters per byte.
std::vector<std::byte> hex_bytes(std::string_view hex)
{
    std::vector<std::byte> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
    {
        const auto digit = [](char c) -> unsigned
        {
            if (c >= '0' && c <= '9')
            {
                return static_cast<unsigned>(c - '0');
            }
            return static_cast<unsigned>(c - 'a' + 10);
        };
        out.push_back(static_cast<std::byte>((digit(hex[i]) << 4) | digit(hex[i + 1])));
    }
    return out;
}

std::vector<std::byte> bytes_of(std::initializer_list<unsigned> values)
{
    std::vector<std::byte> out;
    for (const unsigned value : values)
    {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

} // namespace

TEST_CASE("an xpc dictionary encodes to the pinned bytes", "[remotexpc]")
{
    // {"a": <uint64 1>}: the dictionary type, the payload length, the entry count,
    // the aligned key, then the value's type and value.
    Xpc::Dictionary dictionary;
    dictionary.emplace("a", Xpc::uint64(1));
    const Xpc value = Xpc::dictionary(std::move(dictionary));

    const std::vector<std::byte> expected = hex_bytes(
        "00f00000"
        "14000000"
        "01000000"
        "61000000"
        "00400000"
        "0100000000000000");
    CHECK(value.to_bytes() == expected);
}

TEST_CASE("an xpc object round-trips through bytes", "[remotexpc]")
{
    Xpc::Uuid uuid{};
    uuid[0] = std::byte{0xab};

    Xpc::Dictionary dictionary;
    dictionary.emplace("array", Xpc::array({Xpc(std::int64_t{1}), Xpc("x")}));
    dictionary.emplace("bool", Xpc(true));
    dictionary.emplace("data", Xpc::data(bytes_of({1, 2, 3})));
    dictionary.emplace("date", Xpc::date(1234567890123456789ULL));
    dictionary.emplace("double", Xpc(3.5));
    dictionary.emplace("int64", Xpc(std::int64_t{-5}));
    dictionary.emplace("null", Xpc());
    dictionary.emplace("string", Xpc("hello"));
    dictionary.emplace("uint64", Xpc::uint64(42));
    dictionary.emplace("uuid", Xpc::uuid(uuid));

    const Xpc value = Xpc::dictionary(std::move(dictionary));
    auto parsed = Xpc::parse(value.to_bytes());
    REQUIRE(parsed.has_value());

    CHECK(parsed->is_dictionary());
    CHECK(parsed->find("bool")->boolean() == true);
    CHECK(parsed->find("int64")->int64() == -5);
    CHECK(parsed->find("uint64")->uint64() == 42);
    CHECK(parsed->find("double")->real() == 3.5);
    CHECK(parsed->find("string")->string() == "hello");
    CHECK(parsed->find("null")->is_null());
    CHECK(parsed->find("uuid")->uuid() == uuid);

    REQUIRE(parsed->find("date") != nullptr);
    CHECK(parsed->find("date")->date()->nanoseconds == 1234567890123456789ULL);

    REQUIRE(parsed->find("array") != nullptr);
    REQUIRE(parsed->find("array")->array() != nullptr);
    CHECK(parsed->find("array")->array()->at(0).int64() == 1);
    CHECK(parsed->find("array")->array()->at(1).string() == "x");

    REQUIRE(parsed->find("data") != nullptr);
    REQUIRE(parsed->find("data")->data().has_value());
    CHECK(parsed->find("data")->data()->size() == 3);

    // The dictionary re-encodes to the same bytes, so the codec is stable.
    CHECK(parsed->to_bytes() == value.to_bytes());
}

TEST_CASE("a wrapper round-trips its payload and header", "[remotexpc]")
{
    const XpcWrapper wrapper = XpcWrapper::request(7, Xpc("hello"), true);

    const std::vector<std::byte> bytes = wrapper.encode();
    CHECK(bytes.size() >= kXpcWrapperHeaderSize);

    // The magic, the flags, the 64-bit body length, and the message id.
    const std::vector<std::byte> header = hex_bytes(
        "920bb029"
        "01010100"
        "1800000000000000"
        "0700000000000000");
    CHECK(std::vector<std::byte>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(header.size())) == header);
    CHECK(bytes.size() == kXpcWrapperHeaderSize + 24);

    auto parsed = XpcWrapper::parse(bytes);
    REQUIRE(parsed.has_value());
    CHECK(parsed->flags == (kXpcFlagAlwaysSet | kXpcFlagDataPresent | kXpcFlagWantingReply));
    CHECK(parsed->message_id == 7);
    REQUIRE(parsed->payload.has_value());
    CHECK(parsed->payload->object.string() == "hello");
}

TEST_CASE("a wrapper without a payload round-trips its flags", "[remotexpc]")
{
    XpcWrapper wrapper;
    wrapper.flags = kXpcFlagAlwaysSet | kXpcFlagReply;
    wrapper.message_id = 3;

    const std::vector<std::byte> bytes = wrapper.encode();
    CHECK(bytes.size() == kXpcWrapperHeaderSize);

    auto parsed = XpcWrapper::parse(bytes);
    REQUIRE(parsed.has_value());
    CHECK(parsed->flags == (kXpcFlagAlwaysSet | kXpcFlagReply));
    CHECK(parsed->message_id == 3);
    CHECK_FALSE(parsed->payload.has_value());
}

TEST_CASE("a bad wrapper magic is a protocol error", "[remotexpc]")
{
    std::vector<std::byte> bytes = XpcWrapper::request(0, Xpc(), true).encode();
    bytes[0] = std::byte{0};

    auto parsed = XpcWrapper::parse(bytes);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == ErrorCode::Protocol);
}

TEST_CASE("a bad payload magic or version is a protocol error", "[remotexpc]")
{
    std::vector<std::byte> bytes = XpcWrapper::request(0, Xpc("x"), true).encode();

    std::vector<std::byte> bad_magic = bytes;
    bad_magic[kXpcWrapperHeaderSize] = std::byte{0};
    auto magic = XpcWrapper::parse(bad_magic);
    REQUIRE_FALSE(magic.has_value());
    CHECK(magic.error().code == ErrorCode::Protocol);

    std::vector<std::byte> bad_version = bytes;
    bad_version[kXpcWrapperHeaderSize + 4] = std::byte{0};
    auto version = XpcWrapper::parse(bad_version);
    REQUIRE_FALSE(version.has_value());
    CHECK(version.error().code == ErrorCode::Protocol);
}

TEST_CASE("an xpc object with an unknown type is a protocol error", "[remotexpc]")
{
    const std::vector<std::byte> bytes = hex_bytes("ff100000");

    auto parsed = Xpc::parse(bytes);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == ErrorCode::Protocol);
}

TEST_CASE("a truncated xpc object is a protocol error", "[remotexpc]")
{
    std::vector<std::byte> bytes = Xpc("hello").to_bytes();
    bytes.pop_back();

    auto parsed = Xpc::parse(bytes);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == ErrorCode::Protocol);
}

TEST_CASE("an xpc accessor does not convert between kinds", "[remotexpc]")
{
    const Xpc value = Xpc::uint64(1280);
    CHECK(value.is_uint64());
    CHECK_FALSE(value.is_int64());
    CHECK_FALSE(value.is_string());
    CHECK(value.uint64().has_value());
    CHECK_FALSE(value.int64().has_value());
    CHECK_FALSE(value.string().has_value());
    CHECK(value.array() == nullptr);
    CHECK(value.find("mtu") == nullptr);
}
