#include "ioscpp/protocol/keyed_archive.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

using namespace ioscpp;
using namespace ioscpp::protocol;

namespace
{

std::vector<std::byte> bytes(std::initializer_list<unsigned> values)
{
    std::vector<std::byte> out;
    for (const unsigned value : values)
    {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

} // namespace

TEST_CASE("an NSKeyedArchive encodes the pinned byte vector", "[keyed-archive]")
{
    // A `plistlib` binary plist of the skeleton for the string "hello", with the
    // root as `Uid(1)` and `$objects` as `["$null", "hello"]`.
    const std::vector<std::byte> expected = bytes({
        0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30, 0xd4, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x09, 0x0c, 0x59,
        0x24, 0x61, 0x72, 0x63, 0x68, 0x69, 0x76, 0x65, 0x72, 0x58, 0x24, 0x6f, 0x62, 0x6a, 0x65, 0x63, 0x74, 0x73,
        0x54, 0x24, 0x74, 0x6f, 0x70, 0x58, 0x24, 0x76, 0x65, 0x72, 0x73, 0x69, 0x6f, 0x6e, 0x5f, 0x10, 0x0f, 0x4e,
        0x53, 0x4b, 0x65, 0x79, 0x65, 0x64, 0x41, 0x72, 0x63, 0x68, 0x69, 0x76, 0x65, 0x72, 0xa2, 0x07, 0x08, 0x55,
        0x24, 0x6e, 0x75, 0x6c, 0x6c, 0x55, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0xd1, 0x0a, 0x0b, 0x54, 0x72, 0x6f, 0x6f,
        0x74, 0x80, 0x01, 0x12, 0x00, 0x01, 0x86, 0xa0, 0x08, 0x11, 0x1b, 0x24, 0x29, 0x32, 0x44, 0x47, 0x4d, 0x53,
        0x56, 0x5b, 0x5d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x62,
    });
    CHECK(KeyedArchive::archive(Plist("hello")) == expected);
}

TEST_CASE("an NSKeyedArchive round-trips a string", "[keyed-archive]")
{
    const std::vector<std::byte> encoded = KeyedArchive::archive(Plist("hello"));
    auto value = KeyedArchive::unarchive(encoded);
    REQUIRE(value.has_value());
    CHECK(value->type() == PlistType::String);
    CHECK(value->string_or() == "hello");
}

TEST_CASE("an NSKeyedArchive round-trips a dictionary and an array", "[keyed-archive]")
{
    Plist::Dictionary dictionary;
    dictionary.emplace("name", Plist("ioscpp"));
    dictionary.emplace("count", Plist(std::int64_t{3}));
    dictionary.emplace("items", Plist::array({Plist(std::int64_t{1}), Plist("two")}));

    const std::vector<std::byte> encoded = KeyedArchive::archive(Plist::dictionary(dictionary));
    auto value = KeyedArchive::unarchive(encoded);
    REQUIRE(value.has_value());
    REQUIRE(value->dictionary() != nullptr);
    REQUIRE(value->find("name") != nullptr);
    CHECK(value->find("name")->string_or() == "ioscpp");
    REQUIRE(value->find("count") != nullptr);
    CHECK(value->find("count")->integer() == 3);
    REQUIRE(value->find("items") != nullptr);
    REQUIRE(value->find("items")->array() != nullptr);
    CHECK(value->find("items")->array()->size() == 2);
    CHECK((*value->find("items")->array())[0].integer() == 1);
    CHECK((*value->find("items")->array())[1].string_or() == "two");
}

TEST_CASE("an NSKeyedArchive skeleton is the fixed shape", "[keyed-archive]")
{
    const std::vector<std::byte> encoded = KeyedArchive::archive(Plist("hello"));
    auto skeleton = Plist::parse_binary(encoded);
    REQUIRE(skeleton.has_value());
    REQUIRE(skeleton->dictionary() != nullptr);
    REQUIRE(skeleton->find("$archiver") != nullptr);
    CHECK(skeleton->find("$archiver")->string_or() == "NSKeyedArchiver");
    REQUIRE(skeleton->find("$version") != nullptr);
    CHECK(skeleton->find("$version")->integer() == 100000);
    REQUIRE(skeleton->find("$objects") != nullptr);
    REQUIRE(skeleton->find("$objects")->array() != nullptr);
    CHECK((*skeleton->find("$objects")->array())[0].string_or() == "$null");
    REQUIRE(skeleton->find("$top") != nullptr);
    REQUIRE(skeleton->find("$top")->find("root") != nullptr);
    CHECK(skeleton->find("$top")->find("root")->uid() == 1);
}

TEST_CASE("bytes that are not an NSKeyedArchive are a protocol error", "[keyed-archive]")
{
    const std::vector<std::byte> encoded = Plist::dictionary({}).to_binary();
    auto value = KeyedArchive::unarchive(encoded);
    REQUIRE_FALSE(value.has_value());
    CHECK(value.error().code == ErrorCode::Protocol);
}

TEST_CASE("an NSKeyedArchive with an out-of-range Uid is a protocol error", "[keyed-archive]")
{
    Plist::Dictionary skeleton;
    skeleton.emplace("$version", Plist(std::int64_t{100000}));
    skeleton.emplace("$archiver", Plist("NSKeyedArchiver"));
    Plist::Dictionary top;
    top.emplace("root", Plist::uid(9));
    skeleton.emplace("$top", Plist::dictionary(std::move(top)));
    skeleton.emplace("$objects", Plist::array({Plist("$null")}));

    const std::vector<std::byte> encoded = Plist::dictionary(std::move(skeleton)).to_binary();
    auto value = KeyedArchive::unarchive(encoded);
    REQUIRE_FALSE(value.has_value());
    CHECK(value.error().code == ErrorCode::Protocol);
}
