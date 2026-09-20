#pragma once

#include <cstddef>
#include <span>

#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"

namespace ioscpp
{

/**
 * @brief A byte stream to a device service.
 *
 * A `ByteStream` is the byte-level view of a service connection: it reads
 * exactly the bytes asked for and writes all of them. A mux @ref Stream and a
 * @ref TcpLink are both byte streams, so a service codec (`AFC`, the
 * `installation_proxy` plist service) rides either one without knowing which
 * link it is on. That is what lets the same code run over the mux link and over
 * the RSD tunnel.
 *
 * A `ByteStream` is not thread-safe, so concurrent calls must be serialized by
 * the caller.
 */
class IOSCPP_API ByteStream
{
public:
    virtual ~ByteStream() = default;

    ByteStream(const ByteStream &) = delete;
    ByteStream &operator=(const ByteStream &) = delete;

    /// Reads exactly `buffer.size()` bytes, or fails.
    virtual Status read_exact(std::span<std::byte> buffer) = 0;

    /// Writes the whole of `data`, or fails.
    virtual Status write(std::span<const std::byte> data) = 0;

    /// Closes the stream, releasing any underlying resources.
    virtual void close() = 0;

protected:
    ByteStream() = default;

    /// A `ByteStream` is move-only, so a concrete stream can be returned by
    /// value from its factory. The move is protected because only a derived
    /// class's own move constructor calls it.
    ByteStream(ByteStream &&) = default;
    ByteStream &operator=(ByteStream &&) = default;
};

} // namespace ioscpp
