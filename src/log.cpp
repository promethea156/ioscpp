#include "ioscpp/log.hpp"

#include <mutex>
#include <string_view>
#include <utility>

namespace ioscpp
{
namespace
{

// The sink and the level it is written at. Both are process-wide, because the
// objects that log do not take a logger of their own.
LogSink &current_sink() noexcept
{
    static LogSink value;
    return value;
}

LogLevel &current_level() noexcept
{
    static LogLevel value = LogLevel::Info;
    return value;
}

// The sink and the level are shared by every thread that logs, so they are read
// and written under one mutex. It is a function-local static, which is initialized
// once and thread-safely.
std::mutex &mutex() noexcept
{
    static std::mutex value;
    return value;
}

} // namespace

void set_logger(LogSink sink, LogLevel level)
{
    const std::scoped_lock lock(mutex());
    current_level() = level;
    current_sink() = std::move(sink);
}

void clear_logger()
{
    const std::scoped_lock lock(mutex());
    current_sink() = nullptr;
}

bool is_logging(LogLevel level)
{
    const std::scoped_lock lock(mutex());
    return current_sink() != nullptr && level <= current_level();
}

void log(LogLevel level, std::string_view message)
{
    // The sink is copied under the lock and called without it, so a slow sink
    // holds up only the thread that logged, and a sink that logs again does not
    // deadlock the logger.
    LogSink copy;
    {
        const std::scoped_lock lock(mutex());
        if (current_sink() == nullptr || level > current_level())
        {
            return;
        }
        copy = current_sink();
    }
    copy(level, message);
}

} // namespace ioscpp
