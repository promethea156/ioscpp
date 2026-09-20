#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ioscpp/byte_stream.hpp"
#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"
#include "ioscpp/stream.hpp"

namespace ioscpp
{

/**
 * @brief A directory entry returned by `Afc::list`.
 *
 * `name` is always set; the metadata fields are the `stat` values the device may
 * append after a name, and they stay zero when it sends the name alone, which is
 * what the media root does. `mode` is a `st_mode`, so the file type lives in its
 * top bits, exactly like `<sys/stat.h>` on the host.
 */
struct IOSCPP_API DirEntry
{
    /// The entry name, without its directory.
    std::string name;
    /// The POSIX mode: file type and permission bits.
    std::uint32_t mode = 0;
    /// The size in bytes.
    std::uint64_t size = 0;
    /// The last modification time, in seconds since the epoch.
    std::int64_t mtime = 0;

    /// Whether the entry is a directory (`S_ISDIR`).
    bool is_directory() const noexcept
    {
        return (mode & 0170000u) == 0040000u;
    }

    /// Whether the entry is a regular file (`S_ISREG`).
    bool is_regular() const noexcept
    {
        return (mode & 0170000u) == 0100000u;
    }
};

/**
 * @brief A file's metadata, as reported by `Afc::stat`.
 *
 * The fields are the metadata the device reports, the same values `stat` would give on
 * the device. `mode` is a `st_mode`, so the file type lives in its top bits, exactly
 * like `<sys/stat.h>` on the host.
 */
struct IOSCPP_API FileStat
{
    /// The POSIX mode: file type and permission bits.
    std::uint32_t mode = 0;
    /// The size in bytes.
    std::uint64_t size = 0;
    /// The last modification time, in seconds since the epoch.
    std::int64_t mtime = 0;

    /// Whether the path is a directory (`S_ISDIR`).
    bool is_directory() const noexcept
    {
        return (mode & 0170000u) == 0040000u;
    }

    /// Whether the path is a regular file (`S_ISREG`).
    bool is_regular() const noexcept
    {
        return (mode & 0170000u) == 0100000u;
    }
};

/**
 * @brief An `AFC` client on a service stream.
 *
 * `AFC` (Apple File Conduit) is the file service. It is reached by asking
 * `lockdownd` to start `com.apple.afc`, and it carries binary `CFA6LPAA` packets
 * rather than plists. The packet layout and every operation are in
 * [`docs/06-afc-protocol.md`](../docs/06-afc-protocol.md).
 *
 * Paths are relative to the service root and are sent as UTF-8 with a terminating
 * NUL. A request the device rejects is an `ErrorCode::Device` error carrying the
 * `AFC` status.
 *
 * An `Afc` is not thread-safe. It shares its stream, so concurrent calls must be
 * serialized by the caller.
 */
class IOSCPP_API Afc
{
public:
    /**
     * @brief Opens an `AFC` client over `stream`, which the caller keeps alive.
     *
     * `stream` must be a started `com.apple.afc` service or, over the RSD
     * tunnel, its `com.apple.afc.shim.remote` counterpart.
     */
    static Result<Afc> start(ByteStream &stream);

    /// Opens an `AFC` client over `stream`, which the client owns.
    static Result<Afc> start(Stream stream);

    ~Afc();
    Afc(Afc &&) noexcept;
    Afc &operator=(Afc &&) noexcept;
    Afc(const Afc &) = delete;
    Afc &operator=(const Afc &) = delete;

    /// Lists the entries of the directory `path`.
    ///
    /// The device answers with one `DATA` packet of entry names, so only `name` is
    /// set unless it also appends the entry's `stat` values.
    Result<std::vector<DirEntry>> list(std::string_view path);

    /// Stats `path`, following symbolic links.
    ///
    /// A missing path is an empty `optional`, not an error.
    Result<std::optional<FileStat>> stat(std::string_view path);

    /**
     * @brief Copies `remote` from the device to `local`.
     *
     * The file is streamed, so it is never held in memory. `local` is created if
     * it does not exist and truncated if it does, and its parent must exist.
     */
    Status pull(std::string_view remote, const std::filesystem::path &local);

    /// Copies `local` to `remote` on the device.
    Status push(const std::filesystem::path &local, std::string_view remote);

    /// Removes `path` from the device.
    Status remove(std::string_view path);

    /// Creates the directory `path` on the device.
    Status make_directory(std::string_view path);

    /// Renames `from` to `to` on the device.
    Status rename(std::string_view from, std::string_view to);

private:
    explicit Afc(ByteStream &stream);
    explicit Afc(Stream stream);

    /// Opens a file and returns its handle, for `pull` and `push`.
    Result<std::uint64_t> open_file(std::string_view path, std::uint64_t mode);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ioscpp
