#include "ioscpp/crypto/pairing.hpp"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ioscpp/lockdown.hpp"
#include "ioscpp/protocol/plist.hpp"
#include "ioscpp/stream.hpp"

namespace ioscpp::crypto
{
namespace
{

Error crypto_error(std::string message)
{
    return Error{ErrorCode::Crypto, std::move(message)};
}

/// The shared DRBG and entropy, initialized once.
struct Random
{
    mbedtls_entropy_context entropy{};
    mbedtls_ctr_drbg_context drbg{};
    bool ready = false;

    Random()
    {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        const char *personalization = "ioscpp";
        if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                  reinterpret_cast<const unsigned char *>(personalization),
                                  std::strlen(personalization)) == 0)
        {
            ready = true;
        }
    }

    ~Random()
    {
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    }
};

Random &random()
{
    static Random instance;
    return instance;
}

int rng(void *context, unsigned char *output, std::size_t size)
{
    (void)context;
    return mbedtls_ctr_drbg_random(&random().drbg, output, size);
}

std::string random_hex(std::size_t bytes)
{
    static constexpr std::string_view digits = "0123456789ABCDEF";
    std::vector<unsigned char> buffer(bytes);
    (void)mbedtls_ctr_drbg_random(&random().drbg, buffer.data(), buffer.size());
    std::string text;
    text.reserve(bytes * 2);
    for (const unsigned char byte : buffer)
    {
        text.push_back(digits[byte >> 4]);
        text.push_back(digits[byte & 0x0f]);
    }
    return text;
}

std::vector<std::byte> as_bytes(const unsigned char *data, std::size_t size)
{
    return std::vector<std::byte>(reinterpret_cast<const std::byte *>(data),
                                  reinterpret_cast<const std::byte *>(data) + size);
}

/// Generates an RSA-2048 key and a self-signed certificate for it.
Result<std::pair<std::vector<std::byte>, std::vector<std::byte>>> generate_key_and_cert(std::string_view common_name)
{
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    if (mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0)
    {
        mbedtls_pk_free(&key);
        return tl::unexpected(crypto_error("the host key could not be initialized"));
    }
    if (mbedtls_rsa_gen_key(mbedtls_pk_rsa(key), rng, nullptr, 2048, 65537) != 0)
    {
        mbedtls_pk_free(&key);
        return tl::unexpected(crypto_error("the host key could not be generated"));
    }

    mbedtls_x509write_cert certificate;
    mbedtls_x509write_crt_init(&certificate);
    mbedtls_x509write_crt_set_version(&certificate, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&certificate, MBEDTLS_MD_SHA256);

    mbedtls_mpi serial;
    mbedtls_mpi_init(&serial);
    (void)mbedtls_mpi_fill_random(&serial, 16, rng, nullptr);
    (void)mbedtls_x509write_crt_set_serial(&certificate, &serial);

    const std::string name = "CN=" + std::string(common_name);
    (void)mbedtls_x509write_crt_set_subject_name(&certificate, name.c_str());
    (void)mbedtls_x509write_crt_set_issuer_name(&certificate, name.c_str());
    (void)mbedtls_x509write_crt_set_validity(&certificate, "200101000000", "20491231235959");
    (void)mbedtls_x509write_crt_set_basic_constraints(&certificate, 0, -1);
    (void)mbedtls_x509write_crt_set_subject_key(&certificate, &key);
    (void)mbedtls_x509write_crt_set_issuer_key(&certificate, &key);

    std::array<unsigned char, 4096> cert_buffer{};
    const int cert_size = mbedtls_x509write_crt_der(&certificate, cert_buffer.data(), cert_buffer.size(), rng, nullptr);
    if (cert_size < 0)
    {
        mbedtls_mpi_free(&serial);
        mbedtls_x509write_crt_free(&certificate);
        mbedtls_pk_free(&key);
        return tl::unexpected(crypto_error("the host certificate could not be written"));
    }

    std::array<unsigned char, 4096> key_buffer{};
    const int key_size = mbedtls_pk_write_key_der(&key, key_buffer.data(), key_buffer.size());
    if (key_size < 0)
    {
        mbedtls_mpi_free(&serial);
        mbedtls_x509write_crt_free(&certificate);
        mbedtls_pk_free(&key);
        return tl::unexpected(crypto_error("the host key could not be written"));
    }

    // Both writers fill the buffer from the end.
    std::vector<std::byte> certificate_bytes =
        as_bytes(cert_buffer.data() + cert_buffer.size() - cert_size, static_cast<std::size_t>(cert_size));
    std::vector<std::byte> key_bytes =
        as_bytes(key_buffer.data() + key_buffer.size() - key_size, static_cast<std::size_t>(key_size));

    mbedtls_mpi_free(&serial);
    mbedtls_x509write_crt_free(&certificate);
    mbedtls_pk_free(&key);
    return std::make_pair(std::move(key_bytes), std::move(certificate_bytes));
}

std::filesystem::path default_directory()
{
    if (const char *home = std::getenv("HOME"); home != nullptr)
    {
        return std::filesystem::path(home) / ".ioscpp";
    }
    if (const char *profile = std::getenv("USERPROFILE"); profile != nullptr)
    {
        return std::filesystem::path(profile) / ".ioscpp";
    }
    return std::filesystem::path(".ioscpp");
}

} // namespace

struct Pairing::Impl
{
    std::filesystem::path path;
    std::string host_id;
    std::string system_buid;
    std::string udid;
    std::vector<std::byte> host_certificate;
    std::vector<std::byte> host_private_key;
    std::vector<std::byte> root_certificate;
    std::vector<std::byte> root_private_key;
    std::vector<std::byte> device_certificate;
    std::vector<std::byte> escrow_bag;
    std::vector<std::byte> session_key;
};

Pairing::Pairing()
    : impl_(std::make_unique<Impl>())
{
}

Pairing::~Pairing() = default;
Pairing::Pairing(Pairing &&) noexcept = default;
Pairing &Pairing::operator=(Pairing &&) noexcept = default;

Result<Pairing> Pairing::load(const std::filesystem::path &record_path)
{
    Pairing pairing;
    pairing.impl_->path = record_path;
    pairing.impl_->session_key = as_bytes(reinterpret_cast<const unsigned char *>(random_hex(10).data()), 10);

    if (std::filesystem::exists(record_path))
    {
        std::error_code code;
        const std::uintmax_t size = std::filesystem::file_size(record_path, code);
        if (!code && size > 0)
        {
            std::vector<std::byte> bytes(static_cast<std::size_t>(size));
            FILE *file = std::fopen(record_path.string().c_str(), "rb");
            if (file == nullptr)
            {
                return tl::unexpected(crypto_error("the pairing record could not be read"));
            }
            const std::size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
            std::fclose(file);
            bytes.resize(read);

            auto record = protocol::Plist::parse(bytes);
            if (!record)
            {
                return tl::unexpected(record.error());
            }

            auto copy = [&](std::string_view key, std::vector<std::byte> &out)
            {
                const protocol::Plist *item = record->find(key);
                if (item != nullptr && item->data().has_value())
                {
                    out.assign(item->data()->begin(), item->data()->end());
                }
            };
            if (const protocol::Plist *item = record->find("HostID"); item != nullptr && item->string().has_value())
            {
                pairing.impl_->host_id = std::string(*item->string());
            }
            if (const protocol::Plist *item = record->find("SystemBUID"); item != nullptr && item->string().has_value())
            {
                pairing.impl_->system_buid = std::string(*item->string());
            }
            if (const protocol::Plist *item = record->find("UniqueDeviceID");
                item != nullptr && item->string().has_value())
            {
                pairing.impl_->udid = std::string(*item->string());
            }
            copy("HostCertificate", pairing.impl_->host_certificate);
            copy("HostPrivateKey", pairing.impl_->host_private_key);
            copy("RootCertificate", pairing.impl_->root_certificate);
            copy("RootPrivateKey", pairing.impl_->root_private_key);
            copy("DeviceCertificate", pairing.impl_->device_certificate);
            copy("EscrowBag", pairing.impl_->escrow_bag);
            return pairing;
        }
    }

    // A fresh record: generate the host key, the self-signed certificate, and the
    // identity strings.
    pairing.impl_->host_id = random_hex(8);
    pairing.impl_->system_buid = random_hex(16);
    auto generated = generate_key_and_cert(pairing.impl_->host_id);
    if (!generated)
    {
        return tl::unexpected(generated.error());
    }
    pairing.impl_->host_private_key = std::move(generated->first);
    pairing.impl_->host_certificate = std::move(generated->second);
    return pairing;
}

Result<Pairing> Pairing::load_for_udid(std::string_view udid)
{
    return load(default_directory() / (std::string(udid) + ".plist"));
}

Status Pairing::save() const
{
    std::error_code code;
    std::filesystem::create_directories(impl_->path.parent_path(), code);

    protocol::Plist::Dictionary record{
        {"DeviceCertificate", protocol::Plist(impl_->device_certificate)},
        {"HostCertificate", protocol::Plist(impl_->host_certificate)},
        {"HostID", protocol::Plist(impl_->host_id)},
        {"HostPrivateKey", protocol::Plist(impl_->host_private_key)},
        {"RootCertificate", protocol::Plist(impl_->root_certificate)},
        {"RootPrivateKey", protocol::Plist(impl_->root_private_key)},
        {"SystemBUID", protocol::Plist(impl_->system_buid)},
        {"UniqueDeviceID", protocol::Plist(impl_->udid)},
    };
    if (!impl_->escrow_bag.empty())
    {
        record.emplace("EscrowBag", protocol::Plist(impl_->escrow_bag));
    }

    const std::string xml = protocol::Plist::dictionary(std::move(record)).to_xml();
    FILE *file = std::fopen(impl_->path.string().c_str(), "wb");
    if (file == nullptr)
    {
        return tl::unexpected(Error{ErrorCode::Io, "the pairing record could not be written"});
    }
    const std::size_t written = std::fwrite(xml.data(), 1, xml.size(), file);
    std::fclose(file);
    if (written != xml.size())
    {
        return tl::unexpected(Error{ErrorCode::Io, "the pairing record was only partly written"});
    }
    return {};
}

bool Pairing::paired() const noexcept
{
    return !impl_->device_certificate.empty();
}

std::string_view Pairing::host_id() const noexcept
{
    return impl_->host_id;
}

std::string_view Pairing::udid() const noexcept
{
    return impl_->udid;
}

void Pairing::set_udid(std::string udid)
{
    impl_->udid = std::move(udid);
}

std::string_view Pairing::system_buid() const noexcept
{
    return impl_->system_buid;
}

std::span<const std::byte> Pairing::host_certificate() const noexcept
{
    return impl_->host_certificate;
}

std::span<const std::byte> Pairing::host_private_key() const noexcept
{
    return impl_->host_private_key;
}

std::span<const std::byte> Pairing::device_certificate() const noexcept
{
    return impl_->device_certificate;
}

std::span<const std::byte> Pairing::root_certificate() const noexcept
{
    return impl_->root_certificate;
}

std::span<const std::byte> Pairing::session_key() const noexcept
{
    return impl_->session_key;
}

Status pair(Lockdown &lockdown, Pairing &pairing)
{
    // The device answers a `Pair` request with its certificate, its root
    // certificate, and an escrow bag. The private keys are not sent.
    protocol::Plist::Dictionary record{
        {"HostCertificate", protocol::Plist(pairing.impl_->host_certificate)},
        {"HostID", protocol::Plist(pairing.impl_->host_id)},
        {"ProtocolVersion", protocol::Plist("2")},
        {"SystemBUID", protocol::Plist(pairing.impl_->system_buid)},
    };

    protocol::Plist::Dictionary request{
        {"PairRecord", protocol::Plist::dictionary(std::move(record))},
        {"Request", protocol::Plist("Pair")},
    };

    auto answer = lockdown.request(protocol::Plist::dictionary(std::move(request)));
    if (!answer)
    {
        return tl::unexpected(answer.error());
    }

    if (const protocol::Plist *error = answer->find("Error"); error != nullptr)
    {
        return tl::unexpected(Error{ErrorCode::Device, error->string_or("pairing failed")});
    }

    auto copy = [&](std::string_view key, std::vector<std::byte> &out)
    {
        const protocol::Plist *item = answer->find(key);
        if (item != nullptr && item->data().has_value())
        {
            out.assign(item->data()->begin(), item->data()->end());
        }
    };
    copy("DeviceCertificate", pairing.impl_->device_certificate);
    copy("RootCertificate", pairing.impl_->root_certificate);
    copy("EscrowBag", pairing.impl_->escrow_bag);

    if (pairing.impl_->device_certificate.empty())
    {
        return tl::unexpected(crypto_error("the device did not send a certificate while pairing"));
    }
    return Status{};
}

// ---------------------------------------------------------------------------
// TLS
// ---------------------------------------------------------------------------

struct TlsSession::Impl
{
    Stream *stream = nullptr;
    mbedtls_ssl_context ssl{};
    mbedtls_ssl_config conf{};
    mbedtls_x509_crt device_certificate{};
    mbedtls_x509_crt host_certificate{};
    mbedtls_pk_context host_key{};
    bool ready = false;

    ~Impl()
    {
        if (ready)
        {
            (void)mbedtls_ssl_close_notify(&ssl);
        }
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_x509_crt_free(&device_certificate);
        mbedtls_x509_crt_free(&host_certificate);
        mbedtls_pk_free(&host_key);
    }
};

TlsSession::TlsSession()
    : impl_(std::make_unique<Impl>())
{
}

TlsSession::~TlsSession() = default;
TlsSession::TlsSession(TlsSession &&) noexcept = default;
TlsSession &TlsSession::operator=(TlsSession &&) noexcept = default;

namespace
{

int tls_send(void *context, const unsigned char *data, std::size_t length)
{
    auto *stream = static_cast<Stream *>(context);
    const Status status = stream->write(std::span<const std::byte>(reinterpret_cast<const std::byte *>(data), length));
    return status ? static_cast<int>(length) : MBEDTLS_ERR_SSL_WANT_WRITE;
}

int tls_recv(void *context, unsigned char *data, std::size_t length)
{
    auto *stream = static_cast<Stream *>(context);
    const Status read = stream->read(std::span<std::byte>(reinterpret_cast<std::byte *>(data), length));
    return read ? static_cast<int>(length) : MBEDTLS_ERR_SSL_WANT_READ;
}

} // namespace

Result<TlsSession> TlsSession::start(Stream &stream, const Pairing &pairing)
{
    TlsSession session;
    Impl &impl = *session.impl_;
    impl.stream = &stream;

    mbedtls_ssl_init(&impl.ssl);
    mbedtls_ssl_config_init(&impl.conf);
    mbedtls_x509_crt_init(&impl.device_certificate);
    mbedtls_x509_crt_init(&impl.host_certificate);
    mbedtls_pk_init(&impl.host_key);

    if (mbedtls_x509_crt_parse_der(&impl.host_certificate,
                                   reinterpret_cast<const unsigned char *>(pairing.host_certificate().data()),
                                   pairing.host_certificate().size()) != 0)
    {
        return tl::unexpected(crypto_error("the host certificate could not be parsed"));
    }
    if (mbedtls_pk_parse_key(&impl.host_key, reinterpret_cast<const unsigned char *>(pairing.host_private_key().data()),
                             pairing.host_private_key().size(), nullptr, 0, rng, nullptr) != 0)
    {
        return tl::unexpected(crypto_error("the host private key could not be parsed"));
    }
    if (mbedtls_x509_crt_parse_der(&impl.device_certificate,
                                   reinterpret_cast<const unsigned char *>(pairing.device_certificate().data()),
                                   pairing.device_certificate().size()) != 0)
    {
        return tl::unexpected(crypto_error("the device certificate could not be parsed"));
    }

    if (mbedtls_ssl_config_defaults(&impl.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0)
    {
        return tl::unexpected(crypto_error("the TLS configuration could not be set"));
    }
    mbedtls_ssl_conf_authmode(&impl.conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_rng(&impl.conf, rng, nullptr);
    mbedtls_ssl_conf_ca_chain(&impl.conf, &impl.device_certificate, nullptr);
    if (mbedtls_ssl_conf_own_cert(&impl.conf, &impl.host_certificate, &impl.host_key) != 0)
    {
        return tl::unexpected(crypto_error("the host certificate could not be configured"));
    }

    if (mbedtls_ssl_setup(&impl.ssl, &impl.conf) != 0)
    {
        return tl::unexpected(crypto_error("the TLS session could not be created"));
    }
    mbedtls_ssl_set_bio(&impl.ssl, &stream, tls_send, tls_recv, nullptr);

    int result = 0;
    while ((result = mbedtls_ssl_handshake(&impl.ssl)) != 0)
    {
        if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            return tl::unexpected(crypto_error("the TLS handshake failed"));
        }
    }

    impl.ready = true;
    return session;
}

Status TlsSession::write(std::span<const std::byte> data)
{
    std::size_t offset = 0;
    while (offset < data.size())
    {
        const int written = mbedtls_ssl_write(
            &impl_->ssl, reinterpret_cast<const unsigned char *>(data.data() + offset), data.size() - offset);
        if (written > 0)
        {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written != MBEDTLS_ERR_SSL_WANT_READ && written != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            return tl::unexpected(crypto_error("the TLS write failed"));
        }
    }
    return {};
}

Result<std::size_t> TlsSession::read(std::span<std::byte> buffer)
{
    for (;;)
    {
        const int read = mbedtls_ssl_read(&impl_->ssl, reinterpret_cast<unsigned char *>(buffer.data()), buffer.size());
        if (read > 0)
        {
            return static_cast<std::size_t>(read);
        }
        if (read == 0)
        {
            return std::size_t{0};
        }
        if (read != MBEDTLS_ERR_SSL_WANT_READ && read != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            return tl::unexpected(crypto_error("the TLS read failed"));
        }
    }
}

void TlsSession::close()
{
    if (impl_->ready)
    {
        (void)mbedtls_ssl_close_notify(&impl_->ssl);
        impl_->ready = false;
    }
}

} // namespace ioscpp::crypto
