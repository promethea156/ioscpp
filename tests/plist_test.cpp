#include "ioscpp/protocol/plist.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <string>
#include <vector>

using ioscpp::protocol::Plist;

namespace
{

std::vector<std::byte> bytes(std::string_view text)
{
    return std::vector<std::byte>(reinterpret_cast<const std::byte *>(text.data()),
                                  reinterpret_cast<const std::byte *>(text.data() + text.size()));
}

} // namespace

TEST_CASE("a plist round-trips through XML", "[plist]")
{
    Plist::Dictionary dictionary{
        {"Boolean", Plist(true)}, {"Data", Plist(bytes("hello"))}, {"Integer", Plist(42)},
        {"Real", Plist(1.5)},     {"String", Plist("text")},
    };
    dictionary.emplace("Array", Plist::array({Plist(1), Plist(2), Plist(3)}));

    const Plist original = Plist::dictionary(std::move(dictionary));
    const std::string xml = original.to_xml();

    auto parsed = Plist::parse_xml(xml);
    REQUIRE(parsed.has_value());
    CHECK(parsed->to_xml() == xml);
}

TEST_CASE("a plist round-trips through binary", "[plist]")
{
    Plist::Dictionary dictionary{
        {"Array", Plist::array({Plist(1), Plist("two"), Plist(false)})},
        {"Integer", Plist(123456789)},
        {"String", Plist("text")},
    };

    const Plist original = Plist::dictionary(std::move(dictionary));
    const std::vector<std::byte> binary = original.to_binary();
    REQUIRE(binary.size() > 8);
    CHECK(std::string(reinterpret_cast<const char *>(binary.data()), 8) == "bplist00");

    auto parsed = Plist::parse_binary(binary);
    REQUIRE(parsed.has_value());
    CHECK(parsed->to_xml() == original.to_xml());
}

TEST_CASE("a dictionary keeps its keys sorted", "[plist]")
{
    Plist::Dictionary dictionary{
        {"z", Plist(1)},
        {"a", Plist(2)},
        {"m", Plist(3)},
    };
    const Plist value = Plist::dictionary(std::move(dictionary));

    REQUIRE(value.find("a") != nullptr);
    CHECK(value.find("missing") == nullptr);
}

TEST_CASE("a malformed XML plist is a protocol error", "[plist]")
{
    CHECK_FALSE(Plist::parse_xml("<plist><dict>").has_value());
    CHECK_FALSE(Plist::parse_xml("not xml at all").has_value());
    CHECK_FALSE(Plist::parse(bytes("bplist00")).has_value());
}

TEST_CASE("a date is formatted as ISO 8601", "[plist]")
{
    // 0 seconds since 2001-01-01T00:00:00Z.
    const Plist value = Plist::date(0);
    CHECK(value.to_xml().find("2001-01-01T00:00:00Z") != std::string::npos);
}
