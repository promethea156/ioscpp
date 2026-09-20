#pragma once

#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ioscpp/byte_stream.hpp"
#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"
#include "ioscpp/protocol/dtx.hpp"
#include "ioscpp/protocol/plist.hpp"

namespace ioscpp
{

/// The global channel, which opens other channels and carries the capability handshake.
inline constexpr std::int32_t kDtxGlobalChannel = 0;

/// The control selector that opens a channel.
inline constexpr std::string_view kDtxRequestChannelSelector = "_requestChannelWithCode:identifier:";

class DtxConnection;

/**
 * @brief A logical DTX channel, one service conversation on a connection.
 *
 * A channel is opened with @ref DtxConnection::request_channel and then speaks one
 * service: a method call archives its selector into the payload, archives each
 * argument into the auxiliary dictionary, and awaits the reply. A channel does not
 * own the connection, which must outlive it.
 */
class IOSCPP_API DtxChannel
{
public:
    DtxChannel() = default;

    /// The channel code assigned when the channel was opened; every message
    /// carries it.
    std::int32_t code() const noexcept;
    /// The service identifier the channel was opened for.
    const std::string &identifier() const noexcept;

    /**
     * @brief Invokes `selector`, archiving each argument, and returns the reply.
     *
     * The selector goes into the payload as an `NSKeyedArchive` string, and each
     * argument goes into the auxiliary dictionary as an `NSKeyedArchive` buffer,
     * which is what a `DTX` method call is.
     */
    Result<protocol::Dtx> method_call(std::string_view selector, std::span<const protocol::Plist> arguments = {});

    /// Invokes `selector` with an already-built auxiliary dictionary.
    Result<protocol::Dtx> method_call_with_auxiliary(std::string_view selector, protocol::Dtx::Auxiliary auxiliary);

private:
    friend class DtxConnection;
    DtxChannel(DtxConnection *connection, std::int32_t code, std::string identifier);

    DtxConnection *connection_ = nullptr;
    std::int32_t code_ = 0;
    std::string identifier_;
};

/**
 * @brief A DTX connection, the request/reply layer over a service stream.
 *
 * A `dvt` service does not exchange plists: it exchanges DTX messages over a byte
 * stream (`docs/04-blockers.md`). The @ref protocol::Dtx codec frames one message,
 * and this reads and writes those frames, assigns the identifier shared by a request
 * and its reply, acks a message that expects a reply, and routes a reply to the
 * caller that awaits it.
 *
 * The first message the device sends is the `_notifyOfPublishedCapabilities:`
 * handshake on the global channel; @ref receive acks it, and @ref request_channel then
 * opens a service channel from the global channel. A connection borrows `stream`, which
 * must outlive it, and is not thread-safe, so concurrent calls must be serialized by
 * the caller.
 */
class IOSCPP_API DtxConnection
{
public:
    /// Borrows `stream`, which must outlive the connection.
    explicit DtxConnection(ByteStream &stream);

    /// Writes `message` as one frame.
    Status send(const protocol::Dtx &message);

    /**
     * @brief Reads one message, acknowledging it when it expects a reply.
     *
     * A message with the `ExpectsReply` flag is answered with an acknowledgement
     * before it is returned, which keeps the device from waiting on one.
     */
    Result<protocol::Dtx> receive();

    /// Sends `request` and reads until its reply, queueing the messages in between.
    Result<protocol::Dtx> send_and_await_reply(protocol::Dtx request);

    /// Opens `identifier` on a fresh channel and returns it.
    Result<DtxChannel> request_channel(std::string_view identifier);

    /// Whether a message received while awaiting a reply is queued.
    bool has_pending() const noexcept;

    /// The next message that arrived while awaiting a reply.
    Result<protocol::Dtx> next_pending();

private:
    friend class DtxChannel;
    Result<protocol::Dtx> invoke(std::int32_t channel_code, std::string_view selector,
                                 protocol::Dtx::Auxiliary auxiliary);

    ByteStream *stream_ = nullptr;
    std::uint32_t next_identifier_ = 1;
    std::int32_t next_channel_code_ = 1;
    std::deque<protocol::Dtx> pending_;
};

} // namespace ioscpp
