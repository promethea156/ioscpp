#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ioscpp/export.hpp"
#include "ioscpp/transport.hpp"

namespace ioscpp::testing
{

/**
 * @brief An in-memory Transport for tests and examples.
 *
 * Bytes queued with feed() are returned by read(); bytes passed to write()
 * are accumulated and can be inspected with written().
 */
class IOSCPP_API MockTransport : public Transport
{
public:
    /// Queues `data` to be returned by subsequent read() calls.
    void feed(std::span<const std::byte> data)
    {
        incoming_.insert(incoming_.end(), data.begin(), data.end());
    }

    /// Returns queued bytes, or 0 once the queue is drained (end of stream).
    Result<std::size_t> read(std::span<std::byte> buffer) override
    {
        const std::size_t available = incoming_.size() - read_position_;
        const std::size_t count = std::min(available, buffer.size());
        std::copy_n(incoming_.begin() + static_cast<std::ptrdiff_t>(read_position_), static_cast<std::ptrdiff_t>(count),
                    buffer.begin());
        read_position_ += count;
        return count;
    }

    Status write(std::span<const std::byte> data) override
    {
        written_.insert(written_.end(), data.begin(), data.end());
        return {};
    }

    void close() override
    {
        closed_ = true;
        ++close_count_;
    }

    /// Sets the serial reported by serial(), to stand in for a real transport's.
    void set_serial(std::string serial)
    {
        serial_ = std::move(serial);
    }

    std::string_view serial() const noexcept override
    {
        return serial_;
    }

    /// Bytes accumulated by write() so far.
    const std::vector<std::byte> &written() const noexcept
    {
        return written_;
    }

    /// Whether close() has been called.
    bool closed() const noexcept
    {
        return closed_;
    }

    /// The number of times close() has been called, so a test can tell that a
    /// second close was a no-op.
    std::size_t close_count() const noexcept
    {
        return close_count_;
    }

private:
    std::vector<std::byte> incoming_;
    std::size_t read_position_ = 0;
    std::vector<std::byte> written_;
    bool closed_ = false;
    std::size_t close_count_ = 0;
    std::string serial_;
};

} // namespace ioscpp::testing
