#pragma once

#include <optional>
#include <utility>

#include "ioscpp/byte_stream.hpp"
#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"
#include "ioscpp/protocol/plist.hpp"
#include "ioscpp/stream.hpp"

namespace ioscpp
{

/**
 * @brief A length-prefixed plist service.
 *
 * `lockdownd`, `installation_proxy`, the process-control service, and the RSD
 * checkin all frame every message the same way: a 4-byte big-endian length, then
 * an XML plist. The length is the plist size alone, not including the prefix
 * itself.
 *
 * A `PlistService` borrows a @ref ByteStream, or owns a @ref Stream when it is
 * constructed from one, so the same service code rides the mux link and the RSD
 * tunnel without knowing which link it is on.
 *
 * A `PlistService` is not thread-safe, so concurrent calls must be serialized by
 * the caller.
 */
class IOSCPP_API PlistService
{
public:
    /// Borrows `stream`, which must outlive the service.
    explicit PlistService(ByteStream &stream)
        : stream_(&stream)
    {
    }

    /// Owns `stream`.
    explicit PlistService(Stream stream)
        : owned_(std::move(stream))
        , stream_(&*owned_)
    {
    }

    /// Writes `message` with its 4-byte big-endian length prefix.
    Status send(const protocol::Plist &message);

    /// Reads one length-prefixed plist.
    Result<protocol::Plist> receive();

private:
    /// The mux stream, when the service owns it. The RSD path borrows instead.
    std::optional<Stream> owned_;
    ByteStream *stream_ = nullptr;
};

} // namespace ioscpp
