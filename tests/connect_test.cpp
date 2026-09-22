// `connect_with_retry` retries the whole open and connect with a bounded
// exponential backoff, so a device that re-enumerates and needs a fresh transport
// is recovered without the caller hand-rolling the loop. A fake transport and
// connect keep the test device-free: the helper does not know what it connects.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <optional>
#include <string>

#include "ioscpp/device.hpp"

using namespace ioscpp;

namespace
{

/// A stand-in transport, so the helper's retry loop is exercised with no device.
struct FakeTransport
{
    int id = 0;
};

/// A short backoff, so a retry test does not wait the default 250 ms.
constexpr std::chrono::milliseconds kBackoff{1};

} // namespace

TEST_CASE("connect_with_retry retries a failed connect", "[connect]")
{
    std::optional<FakeTransport> transport;
    int opens = 0;
    int connects = 0;

    // The first open succeeds and its connect fails, so the helper resets the
    // transport and opens a fresh one, then the second connect succeeds.
    const auto open = [&opens]() -> Result<FakeTransport>
    {
        ++opens;
        return FakeTransport{opens};
    };
    const auto connect = [&connects](FakeTransport &t) -> Result<std::string>
    {
        ++connects;
        if (connects == 1)
        {
            return tl::unexpected(Error{ErrorCode::Transport, "the link reset"});
        }
        return "connected " + std::to_string(t.id);
    };

    auto connected = connect_with_retry(open, connect, transport, 3, kBackoff);
    REQUIRE(connected.has_value());
    CHECK(*connected == "connected 2");
    CHECK(opens == 2);
    CHECK(connects == 2);
    // The transport that succeeded is the one left in the holder.
    REQUIRE(transport.has_value());
    CHECK(transport->id == 2);
}

TEST_CASE("connect_with_retry retries a failed open", "[connect]")
{
    std::optional<FakeTransport> transport;
    int opens = 0;

    // The first open fails, so the helper retries the open rather than the
    // connect, and the second open succeeds.
    const auto open = [&opens]() -> Result<FakeTransport>
    {
        ++opens;
        if (opens == 1)
        {
            return tl::unexpected(Error{ErrorCode::Transport, "the device was not found"});
        }
        return FakeTransport{opens};
    };
    const auto connect = [](FakeTransport &) -> Result<std::string>
    {
        return "connected";
    };

    auto connected = connect_with_retry(open, connect, transport, 3, kBackoff);
    REQUIRE(connected.has_value());
    CHECK(*connected == "connected");
    CHECK(opens == 2);
}

TEST_CASE("connect_with_retry returns the last error once every attempt fails", "[connect]")
{
    std::optional<FakeTransport> transport;
    int opens = 0;

    const auto open = [&opens]() -> Result<FakeTransport>
    {
        ++opens;
        return FakeTransport{opens};
    };
    const auto connect = [](FakeTransport &) -> Result<std::string>
    {
        return tl::unexpected(Error{ErrorCode::Transport, "the link reset"});
    };

    auto connected = connect_with_retry(open, connect, transport, 3, kBackoff);
    REQUIRE_FALSE(connected.has_value());
    CHECK(connected.error().code == ErrorCode::Transport);
    CHECK(connected.error().message == "the link reset");
    CHECK(opens == 3);
    // A failed attempt leaves no transport behind.
    CHECK_FALSE(transport.has_value());
}
