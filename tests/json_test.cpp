#include "ioscpp/protocol/json.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>

using namespace ioscpp;
using namespace ioscpp::protocol;

TEST_CASE("a JSON value round-trips through text", "[json]")
{
    const std::string text = R"({"b":[true,false,null],"n":1280,"s":"hello"})";
    auto parsed = Json::parse(text);
    REQUIRE(parsed.has_value());

    const Json *array = parsed->find("b");
    REQUIRE(array != nullptr);
    REQUIRE(array->array() != nullptr);
    CHECK(array->array()->at(0).boolean() == true);
    CHECK(array->array()->at(1).boolean() == false);
    CHECK(array->array()->at(2).is_null());

    CHECK(parsed->find("n")->number() == 1280);
    CHECK(parsed->find("s")->string() == "hello");

    // An integral number is written without a fraction, so it reads back as one.
    CHECK(parsed->to_string() == R"({"b":[true,false,null],"n":1280,"s":"hello"})");
}

TEST_CASE("a JSON string decodes its escapes", "[json]")
{
    auto parsed = Json::parse(R"("a\"b\\c\n\t\/\u0041\u00e9")");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->string().has_value());

    const std::string expected = "a\"b\\c\n\t/A\xc3\xa9";
    CHECK(*parsed->string() == expected);
}

TEST_CASE("a JSON surrogate pair decodes to one code point", "[json]")
{
    // U+1F600, encoded as the surrogate pair D83D DE00.
    auto parsed = Json::parse(R"("\ud83d\ude00")");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->string().has_value());

    const std::string expected = "\xf0\x9f\x98\x80";
    CHECK(*parsed->string() == expected);
}

TEST_CASE("a malformed JSON document is a protocol error", "[json]")
{
    for (const char *text : {"{", "[1,]", R"({"a" 1})", "nul", "\"unterminated", "1 2", "{} trailing"})
    {
        auto parsed = Json::parse(text);
        CHECK_FALSE(parsed.has_value());
        if (!parsed)
        {
            CHECK(parsed.error().code == ErrorCode::Protocol);
        }
    }
}

TEST_CASE("a JSON accessor does not convert between kinds", "[json]")
{
    const Json value(1280);
    CHECK(value.is_number());
    CHECK_FALSE(value.is_string());
    CHECK_FALSE(value.string().has_value());
    CHECK(value.array() == nullptr);
    CHECK(value.find("mtu") == nullptr);
}
