#include "ioscpp/protocol/cdtunnel.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

using namespace ioscpp;
using namespace ioscpp::protocol;

TEST_CASE("a CDTunnel frame round-trips its body", "[cdtunnel]")
{
    const std::string body = R"({"mtu":1280,"type":"clientHandshakeRequest"})";
    const std::vector<std::byte> frame = cdtunnel_encode(body);

    // The magic, then the 16-bit big-endian length, then the body.
    REQUIRE(frame.size() == 8 + 2 + body.size());
    CHECK(std::memcmp(frame.data(), "CDTunnel", 8) == 0);
    CHECK(static_cast<unsigned>(frame[8]) == 0);
    CHECK(static_cast<unsigned>(frame[9]) == body.size());

    auto decoded = cdtunnel_decode(frame);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == body);
}

TEST_CASE("a bad CDTunnel magic is a protocol error", "[cdtunnel]")
{
    std::vector<std::byte> frame = cdtunnel_encode("{}");
    frame[0] = std::byte{'X'};

    auto decoded = cdtunnel_decode(frame);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code == ErrorCode::Protocol);
}

TEST_CASE("a short CDTunnel frame is a protocol error", "[cdtunnel]")
{
    const std::vector<std::byte> frame(4);
    auto decoded = cdtunnel_decode(frame);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code == ErrorCode::Protocol);
}

TEST_CASE("a CDTunnel frame length must match its body", "[cdtunnel]")
{
    std::vector<std::byte> frame = cdtunnel_encode("{}");
    frame.push_back(std::byte{0});

    auto decoded = cdtunnel_decode(frame);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code == ErrorCode::Protocol);
}

TEST_CASE("the handshake request is the reference JSON", "[cdtunnel]")
{
    const CdtunnelRequest request;
    CHECK(request.to_json() == R"({"mtu":1280,"type":"clientHandshakeRequest"})");
}

TEST_CASE("the handshake response parses the RSD endpoint", "[cdtunnel]")
{
    const std::string text = R"({"clientParameters":{"address":"fe80::1","mtu":1500},)"
                             R"("serverAddress":"fe80::2","serverRSDPort":58783,)"
                             R"("type":"serverHandshakeResponse"})";

    auto response = CdtunnelResponse::parse(text);
    REQUIRE(response.has_value());
    CHECK(response->client_address == "fe80::1");
    CHECK(response->client_mtu == 1500);
    CHECK(response->server_address == "fe80::2");
    CHECK(response->server_rsd_port == 58783);
}

TEST_CASE("a handshake response without a field is a protocol error", "[cdtunnel]")
{
    for (const char *text :
         {R"({"serverAddress":"fe80::2","serverRSDPort":1})",
          R"({"clientParameters":{"mtu":1500},"serverAddress":"fe80::2","serverRSDPort":1})",
          R"({"clientParameters":{"address":"fe80::1"},"serverAddress":"fe80::2","serverRSDPort":1})",
          R"({"clientParameters":{"address":"fe80::1","mtu":1500},"serverRSDPort":1})",
          R"({"clientParameters":{"address":"fe80::1","mtu":1500},"serverAddress":"fe80::2"})"})
    {
        auto response = CdtunnelResponse::parse(text);
        CHECK_FALSE(response.has_value());
    }
}
