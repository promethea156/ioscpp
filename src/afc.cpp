#include "ioscpp/afc.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ioscpp/error.hpp"
#include "ioscpp/log.hpp"
#include "ioscpp/stream.hpp"

namespace ioscpp
{
namespace
{

Error protocol_error(std::string message)
{
    return Error{ErrorCode::Protocol, std::move(message)};
}

Error device_error(std::string message)
{
    return Error{ErrorCode::Device, std::move(message)};
}

// The AFC operation codes, from `libimobiledevice`'s `src/afc.h`.
constexpr std::uint64_t kOpStatus = 0x01;
constexpr std::uint64_t kOpData = 0x02;
constexpr std::uint64_t kOpReadDir = 0x03;
constexpr std::uint64_t kOpRemovePath = 0x08;
constexpr std::uint64_t kOpMakeDir = 0x09;
constexpr std::uint64_t kOpGetFileInfo = 0x0A;
constexpr std::uint64_t kOpFileOpen = 0x0D;
constexpr std::uint64_t kOpFileOpenRes = 0x0E;
constexpr std::uint64_t kOpFileRead = 0x0F;
constexpr std::uint64_t kOpFileWrite = 0x10;
constexpr std::uint64_t kOpFileClose = 0x14;
constexpr std::uint64_t kOpRenamePath = 0x18;

// The `AFC_FOPEN_*` open modes, from `libimobiledevice`'s `src/afc.h`.
constexpr std::uint64_t kFopenRdonly = 0x01;
constexpr std::uint64_t kFopenWr = 0x04;

constexpr std::size_t kHeaderSize = 40;
// The device caps one message at 65535 bytes, and a `FILE_WRITE`/`FILE_READ`
// packet carries the 40-byte header and an 8-byte handle on top of the chunk, so
// the chunk stays well under that cap.
constexpr std::size_t kChunkSize = 32 * 1024;

constexpr std::array<char, 8> kMagic{'C', 'F', 'A', '6', 'L', 'P', 'A', 'A'};

void put_le64(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value) noexcept
{
    for (std::size_t i = 0; i < 8; ++i)
    {
        bytes[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xff);
    }
}

std::uint64_t get_le64(std::span<const std::byte> bytes, std::size_t offset) noexcept
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i)
    {
        value |= static_cast<std::uint64_t>(bytes[offset + i]) << (8 * i);
    }
    return value;
}

/// Appends `path` and its NUL terminator.
void append_path(std::vector<std::byte> &out, std::string_view path)
{
    out.insert(out.end(), reinterpret_cast<const std::byte *>(path.data()),
               reinterpret_cast<const std::byte *>(path.data() + path.size()));
    out.push_back(std::byte{0});
}

/// The `st_mode` for an `st_ifmt` string, which the device also sends.
std::uint32_t mode_from_ifmt(std::string_view ifmt)
{
    if (ifmt == "S_IFDIR")
    {
        return 0040000u;
    }
    if (ifmt == "S_IFLNK")
    {
        return 0120000u;
    }
    return 0100000u;
}

std::uint64_t to_u64(std::string_view text)
{
    return std::strtoull(std::string(text).c_str(), nullptr, 10);
}

struct Packet
{
    std::uint64_t operation = 0;
    std::vector<std::byte> data;
};

/// Parses alternating key/value NUL-terminated tokens from an AFC `DATA` payload.
template <typename Handler>
void parse_tokens(std::span<const std::byte> data, Handler handler)
{
    std::size_t position = 0;
    auto next_token = [&]() -> std::optional<std::string_view>
    {
        if (position >= data.size())
        {
            return std::nullopt;
        }
        const char *begin = reinterpret_cast<const char *>(data.data() + position);
        const std::size_t remaining = data.size() - position;
        const std::size_t length = strnlen(begin, remaining);
        position += length + 1;
        return std::string_view(begin, length);
    };

    while (std::optional<std::string_view> key = next_token())
    {
        const std::optional<std::string_view> value = next_token();
        if (!value.has_value())
        {
            break;
        }
        handler(*key, *value);
    }
}

} // namespace

struct Afc::Impl
{
    explicit Impl(ByteStream &value)
        : stream(&value)
    {
    }

    explicit Impl(Stream value)
        : owned(std::move(value))
        , stream(&*owned)
    {
    }

    /// The mux stream, when the client owns it. The RSD path borrows instead.
    std::optional<Stream> owned;
    ByteStream *stream = nullptr;
    std::uint64_t packet_num = 0;

    Status send(std::uint64_t operation, std::span<const std::byte> extra, std::span<const std::byte> payload)
    {
        std::vector<std::byte> packet(kHeaderSize + extra.size() + payload.size());
        std::memcpy(packet.data(), kMagic.data(), kMagic.size());
        put_le64(packet, 8, packet.size());
        put_le64(packet, 16, kHeaderSize + extra.size());
        put_le64(packet, 24, ++packet_num);
        put_le64(packet, 32, operation);
        std::copy(extra.begin(), extra.end(), packet.begin() + static_cast<std::ptrdiff_t>(kHeaderSize));
        std::copy(payload.begin(), payload.end(),
                  packet.begin() + static_cast<std::ptrdiff_t>(kHeaderSize + extra.size()));
        return stream->write(packet);
    }

    Result<Packet> receive()
    {
        std::array<std::byte, kHeaderSize> header{};
        if (Status status = stream->read_exact(header); !status)
        {
            return tl::unexpected(status.error());
        }
        if (std::memcmp(header.data(), kMagic.data(), kMagic.size()) != 0)
        {
            return tl::unexpected(protocol_error("the AFC magic does not match"));
        }

        // A reply is a single packet, so `entire_length` is the whole payload.
        // `this_length` is short only on a request that splits its header from its
        // payload, such as `FILE_WRITE`, so it is validated and not used here.
        const std::uint64_t entire_length = get_le64(header, 8);
        const std::uint64_t this_length = get_le64(header, 16);
        const std::uint64_t packet_number = get_le64(header, 24);
        if (entire_length < kHeaderSize || this_length < kHeaderSize || this_length > entire_length)
        {
            return tl::unexpected(protocol_error("the AFC packet length is out of range"));
        }
        if (packet_number != packet_num)
        {
            return tl::unexpected(protocol_error("the AFC packet number does not match"));
        }

        Packet packet;
        packet.operation = get_le64(header, 32);
        packet.data.resize(entire_length - kHeaderSize);
        if (Status status = stream->read_exact(packet.data); !status)
        {
            return tl::unexpected(status.error());
        }
        if (is_logging(LogLevel::Debug))
        {
            log(LogLevel::Debug, std::string("[afc recv] op=") + std::to_string(packet.operation) +
                                     " entire=" + std::to_string(entire_length) +
                                     " this=" + std::to_string(this_length) + " num=" + std::to_string(packet_number) +
                                     " payload=" + std::to_string(packet.data.size()));
        }
        return packet;
    }

    Result<Packet> transact(std::uint64_t operation, std::span<const std::byte> extra = {},
                            std::span<const std::byte> payload = {})
    {
        if (Status status = send(operation, extra, payload); !status)
        {
            return tl::unexpected(status.error());
        }
        auto answer = receive();
        if (!answer)
        {
            return tl::unexpected(answer.error());
        }
        if (answer->operation == kOpStatus)
        {
            if (answer->data.size() < 8)
            {
                return tl::unexpected(protocol_error("the AFC status is too short"));
            }
            const std::uint64_t code = get_le64(answer->data, 0);
            if (code != 0)
            {
                return tl::unexpected(device_error("the device reported AFC error " + std::to_string(code)));
            }
        }
        else if (answer->operation != kOpData && answer->operation != kOpFileOpenRes)
        {
            return tl::unexpected(protocol_error("the device sent an unexpected AFC operation"));
        }
        return answer;
    }
};

Afc::Afc(ByteStream &stream)
    : impl_(std::make_unique<Impl>(stream))
{
}

Afc::Afc(Stream stream)
    : impl_(std::make_unique<Impl>(std::move(stream)))
{
}

Afc::~Afc() = default;
Afc::Afc(Afc &&) noexcept = default;
Afc &Afc::operator=(Afc &&) noexcept = default;

Result<Afc> Afc::start(ByteStream &stream)
{
    return Afc(stream);
}

Result<Afc> Afc::start(Stream stream)
{
    return Afc(std::move(stream));
}

Result<std::vector<DirEntry>> Afc::list(std::string_view path)
{
    std::vector<std::byte> request;
    append_path(request, path);

    // The device answers a `READ_DIR` with one `DATA` packet holding every entry;
    // unlike the file operations it does not send a trailing `STATUS`.
    auto answer = impl_->transact(kOpReadDir, request);
    if (!answer)
    {
        return tl::unexpected(answer.error());
    }
    if (answer->operation != kOpData)
    {
        return tl::unexpected(protocol_error("the device sent an unexpected AFC operation"));
    }

    // The payload is one entry name after another, each optionally followed by
    // alternating stat keys and values. A token that is not a stat key starts the
    // next entry.
    std::vector<DirEntry> entries;
    std::size_t position = 0;
    const std::span<const std::byte> data(answer->data);
    auto next_token = [&]() -> std::optional<std::string_view>
    {
        if (position >= data.size())
        {
            return std::nullopt;
        }
        const char *begin = reinterpret_cast<const char *>(data.data() + position);
        const std::size_t length = strnlen(begin, data.size() - position);
        position += length + 1;
        return std::string_view(begin, length);
    };

    DirEntry entry;
    while (std::optional<std::string_view> token = next_token())
    {
        if (!token->starts_with("st_"))
        {
            if (!entry.name.empty())
            {
                entries.push_back(std::move(entry));
                entry = DirEntry{};
            }
            entry.name = std::string(*token);
            continue;
        }
        const std::optional<std::string_view> value = next_token();
        if (!value.has_value())
        {
            break;
        }
        if (*token == "st_mtime")
        {
            entry.mtime = static_cast<std::int64_t>(to_u64(*value));
        }
        else if (*token == "st_size")
        {
            entry.size = to_u64(*value);
        }
        else if (*token == "st_mode")
        {
            entry.mode = static_cast<std::uint32_t>(to_u64(*value));
        }
        else if (*token == "st_ifmt" && entry.mode == 0)
        {
            entry.mode = mode_from_ifmt(*value);
        }
    }
    if (!entry.name.empty())
    {
        entries.push_back(std::move(entry));
    }
    return entries;
}

Result<std::optional<FileStat>> Afc::stat(std::string_view path)
{
    std::vector<std::byte> request;
    append_path(request, path);

    auto answer = impl_->transact(kOpGetFileInfo, request);
    if (!answer)
    {
        if (answer.error().code == ErrorCode::Device)
        {
            return std::optional<FileStat>{};
        }
        return tl::unexpected(answer.error());
    }

    FileStat info;
    parse_tokens(answer->data,
                 [&](std::string_view key, std::string_view value)
                 {
                     if (key == "st_mtime")
                     {
                         info.mtime = static_cast<std::int64_t>(to_u64(value));
                     }
                     else if (key == "st_size")
                     {
                         info.size = to_u64(value);
                     }
                     else if (key == "st_mode")
                     {
                         info.mode = static_cast<std::uint32_t>(to_u64(value));
                     }
                     else if (key == "st_ifmt" && info.mode == 0)
                     {
                         info.mode = mode_from_ifmt(value);
                     }
                 });
    return std::optional<FileStat>{info};
}

Result<std::uint64_t> Afc::open_file(std::string_view path, std::uint64_t mode)
{
    // `FILE_OPEN` carries the 8-byte mode first, then the NUL-terminated path.
    std::vector<std::byte> request(8);
    put_le64(request, 0, mode);
    append_path(request, path);

    auto answer = impl_->transact(kOpFileOpen, request);
    if (!answer)
    {
        return tl::unexpected(answer.error());
    }
    if (answer->data.size() < 8)
    {
        return tl::unexpected(protocol_error("the AFC open answer has no handle"));
    }
    return get_le64(answer->data, 0);
}

Status Afc::pull(std::string_view remote, const std::filesystem::path &local)
{
    auto handle = open_file(remote, kFopenRdonly);
    if (!handle)
    {
        return tl::unexpected(handle.error());
    }

    FILE *file = std::fopen(local.string().c_str(), "wb");
    if (file == nullptr)
    {
        return tl::unexpected(Error{ErrorCode::Io, "the local file could not be created"});
    }

    // `FILE_READ` carries the handle and the wanted length, and the device ends the
    // file with a zero-code `STATUS` rather than a zero-length `DATA`.
    std::array<std::byte, 16> request{};
    put_le64(request, 0, *handle);
    Status result;
    for (;;)
    {
        put_le64(request, 8, kChunkSize);
        auto answer = impl_->transact(kOpFileRead, request);
        if (!answer)
        {
            result = tl::unexpected(answer.error());
            break;
        }
        if (answer->operation == kOpStatus || answer->data.empty())
        {
            break;
        }
        if (std::fwrite(answer->data.data(), 1, answer->data.size(), file) != answer->data.size())
        {
            result = tl::unexpected(Error{ErrorCode::Io, "the local file was only partly written"});
            break;
        }
    }
    std::fclose(file);

    std::array<std::byte, 8> handle_bytes{};
    put_le64(handle_bytes, 0, *handle);
    (void)impl_->transact(kOpFileClose, handle_bytes);
    return result;
}

Status Afc::push(const std::filesystem::path &local, std::string_view remote)
{
    FILE *file = std::fopen(local.string().c_str(), "rb");
    if (file == nullptr)
    {
        return tl::unexpected(Error{ErrorCode::Io, "the local file could not be read"});
    }

    auto handle = open_file(remote, kFopenWr);
    if (!handle)
    {
        std::fclose(file);
        return tl::unexpected(handle.error());
    }

    std::vector<std::byte> buffer(kChunkSize);
    std::array<std::byte, 8> handle_bytes{};
    put_le64(handle_bytes, 0, *handle);

    Status result;
    for (;;)
    {
        const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), file);
        if (read == 0)
        {
            break;
        }
        // `FILE_WRITE` carries the handle as its data and the bytes as the
        // payload, so `this_length` covers only the header and the handle.
        auto answer = impl_->transact(kOpFileWrite, handle_bytes, std::span<const std::byte>(buffer).first(read));
        if (!answer)
        {
            result = tl::unexpected(answer.error());
            break;
        }
    }
    std::fclose(file);

    (void)impl_->transact(kOpFileClose, handle_bytes);
    return result;
}

Status Afc::remove(std::string_view path)
{
    std::vector<std::byte> request;
    append_path(request, path);
    auto answer = impl_->transact(kOpRemovePath, request);
    if (!answer)
    {
        return tl::unexpected(answer.error());
    }
    return {};
}

Status Afc::make_directory(std::string_view path)
{
    std::vector<std::byte> request;
    append_path(request, path);
    auto answer = impl_->transact(kOpMakeDir, request);
    if (!answer)
    {
        return tl::unexpected(answer.error());
    }
    return {};
}

Status Afc::rename(std::string_view from, std::string_view to)
{
    std::vector<std::byte> request;
    append_path(request, from);
    append_path(request, to);
    auto answer = impl_->transact(kOpRenamePath, request);
    if (!answer)
    {
        return tl::unexpected(answer.error());
    }
    return {};
}

} // namespace ioscpp
