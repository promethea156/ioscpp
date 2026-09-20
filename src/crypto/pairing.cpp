#include "ioscpp/crypto/pairing.hpp"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/debug.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/md.h>
#include <mbedtls/pem.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <array>
#include <chrono>
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
#include <thread>
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

/// Wraps a DER structure in a PEM envelope. The device's pairing record carries
/// PEM, matching `pair_record_generate_keys_and_certs`.
Result<std::vector<std::byte>> to_pem(std::span<const std::byte> der, std::string_view header, std::string_view footer)
{
    const std::string header_text(header);
    const std::string footer_text(footer);
    std::array<unsigned char, 4096> buffer{};
    std::size_t size = 0;
    if (mbedtls_pem_write_buffer(header_text.c_str(), footer_text.c_str(),
                                 reinterpret_cast<const unsigned char *>(der.data()), der.size(), buffer.data(),
                                 buffer.size(), &size) != 0)
    {
        return tl::unexpected(crypto_error("the PEM could not be written"));
    }
    return as_bytes(buffer.data(), size);
}

/// Generates an RSA-2048 private key.
Result<mbedtls_pk_context> generate_key_pair()
{
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    if (mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0)
    {
        mbedtls_pk_free(&key);
        return tl::unexpected(crypto_error("the key could not be initialized"));
    }
    if (mbedtls_rsa_gen_key(mbedtls_pk_rsa(key), rng, nullptr, 2048, 65537) != 0)
    {
        mbedtls_pk_free(&key);
        return tl::unexpected(crypto_error("the key could not be generated"));
    }
    return key;
}

/// Writes a private key as DER.
Result<std::vector<std::byte>> write_key(mbedtls_pk_context &key)
{
    std::array<unsigned char, 4096> buffer{};
    const int size = mbedtls_pk_write_key_der(&key, buffer.data(), buffer.size());
    if (size < 0)
    {
        return tl::unexpected(crypto_error("the private key could not be written"));
    }
    return to_pem(as_bytes(buffer.data() + buffer.size() - size, static_cast<std::size_t>(size)),
                  "-----BEGIN RSA PRIVATE KEY-----\n", "-----END RSA PRIVATE KEY-----\n");
}

/// Writes a certificate for `subject_key`, signed by `issuer_key`, as DER.
Result<std::vector<std::byte>> write_certificate(mbedtls_pk_context &subject_key, mbedtls_pk_context &issuer_key,
                                                 bool is_ca, bool has_key_usage)
{
    mbedtls_x509write_cert certificate;
    mbedtls_x509write_crt_init(&certificate);
    mbedtls_x509write_crt_set_version(&certificate, MBEDTLS_X509_CRT_VERSION_3);
    // The pairing chain uses SHA-256, matching `generate_pairing_cert_chain`:
    //   https://github.com/doronz88/pymobiledevice3/blob/master/pymobiledevice3/ca.py
    // The reference uses an empty distinguished name; `CN=ioscpp` is our own.
    mbedtls_x509write_crt_set_md_alg(&certificate, MBEDTLS_MD_SHA256);

    // The reference serial is one (`_SERIAL`). A zero serial is written as a
    // zero-length `INTEGER`, which RFC 5280 forbids and OpenSSL 3.0 and the
    // device reject, so the serial is one here too.
    mbedtls_mpi serial;
    mbedtls_mpi_init(&serial);
    (void)mbedtls_mpi_lset(&serial, 1);
    (void)mbedtls_x509write_crt_set_serial(&certificate, &serial);

    (void)mbedtls_x509write_crt_set_subject_name(&certificate, "CN=ioscpp");
    (void)mbedtls_x509write_crt_set_issuer_name(&certificate, "CN=ioscpp");
    (void)mbedtls_x509write_crt_set_validity(&certificate, "20010101000000", "20491231235959");
    (void)mbedtls_x509write_crt_set_basic_constraints(&certificate, is_ca ? 1 : 0, is_ca ? -1 : 0);
    if (has_key_usage)
    {
        (void)mbedtls_x509write_crt_set_key_usage(&certificate,
                                                  MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_ENCIPHERMENT);
    }
    (void)mbedtls_x509write_crt_set_subject_key_identifier(&certificate);
    (void)mbedtls_x509write_crt_set_authority_key_identifier(&certificate);
    (void)mbedtls_x509write_crt_set_subject_key(&certificate, &subject_key);
    (void)mbedtls_x509write_crt_set_issuer_key(&certificate, &issuer_key);

    std::array<unsigned char, 4096> buffer{};
    const int size = mbedtls_x509write_crt_der(&certificate, buffer.data(), buffer.size(), rng, nullptr);
    mbedtls_mpi_free(&serial);
    mbedtls_x509write_crt_free(&certificate);
    if (size < 0)
    {
        return tl::unexpected(crypto_error("the certificate could not be written"));
    }
    return to_pem(as_bytes(buffer.data() + buffer.size() - size, static_cast<std::size_t>(size)),
                  "-----BEGIN CERTIFICATE-----\n", "-----END CERTIFICATE-----\n");
}

/// The certificate chain a `Pair` request carries: a root CA, a host leaf signed
/// by it, and a device leaf built from the device's own public key.
struct Chain
{
    std::vector<std::byte> root_certificate;
    std::vector<std::byte> root_private_key;
    std::vector<std::byte> host_certificate;
    std::vector<std::byte> host_private_key;
    std::vector<std::byte> device_certificate;
};

Result<Chain> generate_chain(std::span<const std::byte> device_public_key)
{
    auto root_key = generate_key_pair();
    if (!root_key)
    {
        return tl::unexpected(root_key.error());
    }
    auto host_key = generate_key_pair();
    if (!host_key)
    {
        mbedtls_pk_free(&*root_key);
        return tl::unexpected(host_key.error());
    }

    mbedtls_pk_context device_key;
    mbedtls_pk_init(&device_key);
    // `mbedtls_pk_parse_public_key` only reads a PEM when the buffer is
    // NUL-terminated, and a plist data value is not.
    std::vector<std::byte> device_pem(device_public_key.begin(), device_public_key.end());
    device_pem.push_back(std::byte{0});
    if (mbedtls_pk_parse_public_key(&device_key, reinterpret_cast<const unsigned char *>(device_pem.data()),
                                    device_pem.size()) != 0)
    {
        mbedtls_pk_free(&device_key);
        mbedtls_pk_free(&*host_key);
        mbedtls_pk_free(&*root_key);
        return tl::unexpected(crypto_error("the device public key could not be parsed"));
    }

    auto root_certificate = write_certificate(*root_key, *root_key, true, false);
    auto host_certificate = write_certificate(*host_key, *root_key, false, true);
    auto device_certificate = write_certificate(device_key, *root_key, false, true);
    auto root_private_key = write_key(*root_key);
    auto host_private_key = write_key(*host_key);

    mbedtls_pk_free(&device_key);
    mbedtls_pk_free(&*host_key);
    mbedtls_pk_free(&*root_key);

    if (!root_certificate || !host_certificate || !device_certificate || !root_private_key || !host_private_key)
    {
        return tl::unexpected(crypto_error("the certificate chain could not be generated"));
    }

    if (std::getenv("IOSCPP_TRACE") != nullptr)
    {
        const std::array<std::pair<const char *, const std::vector<std::byte> *>, 3> certificates{
            std::pair{"root", &*root_certificate}, std::pair{"host", &*host_certificate},
            std::pair{"device", &*device_certificate}};
        for (const auto &[name, bytes] : certificates)
        {
            mbedtls_x509_crt parsed;
            mbedtls_x509_crt_init(&parsed);
            const int rc =
                mbedtls_x509_crt_parse(&parsed, reinterpret_cast<const unsigned char *>(bytes->data()), bytes->size());
            std::fprintf(stderr, "[chain] %s cert parse=%d\n", name, rc);
            mbedtls_x509_crt_free(&parsed);
        }
    }

    Chain chain;
    chain.root_certificate = std::move(*root_certificate);
    chain.root_private_key = std::move(*root_private_key);
    chain.host_certificate = std::move(*host_certificate);
    chain.host_private_key = std::move(*host_private_key);
    chain.device_certificate = std::move(*device_certificate);
    return chain;
}

/// Generates a self-signed host key and certificate, used before pairing.
Result<std::pair<std::vector<std::byte>, std::vector<std::byte>>> generate_key_and_cert()
{
    auto key = generate_key_pair();
    if (!key)
    {
        return tl::unexpected(key.error());
    }
    auto certificate = write_certificate(*key, *key, false, true);
    auto private_key = write_key(*key);
    mbedtls_pk_free(&*key);
    if (!certificate || !private_key)
    {
        return tl::unexpected(crypto_error("the host key could not be generated"));
    }
    return std::make_pair(std::move(*private_key), std::move(*certificate));
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
    std::string wifi_mac_address;
    std::string session_id;
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
            if (const protocol::Plist *item = record->find("WiFiMACAddress");
                item != nullptr && item->string().has_value())
            {
                pairing.impl_->wifi_mac_address = std::string(*item->string());
            }
            return pairing;
        }
    }

    // A fresh record: generate the host key, the self-signed certificate, and the
    // identity strings. The host id and the system build id are stable across
    // runs, matching `generate_host_id` and `SYSTEM_BUID`, so a record the device
    // already holds is recognised:
    //   https://github.com/doronz88/pymobiledevice3/blob/master/pymobiledevice3/pair_records.py
    pairing.impl_->host_id = "6BA7B810-9DAD-11D1-80B4-00C04FD430C8";
    pairing.impl_->system_buid = "30142955-444094379208051516";
    auto generated = generate_key_and_cert();
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
    if (!impl_->wifi_mac_address.empty())
    {
        record.emplace("WiFiMACAddress", protocol::Plist(impl_->wifi_mac_address));
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

std::string_view Pairing::session_id() const noexcept
{
    return impl_->session_id;
}

void Pairing::set_session_id(std::string session_id)
{
    impl_->session_id = std::move(session_id);
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
    // A device with a passcode answers the first `Pair` with
    // `PasswordProtected` until it is unlocked, and an untrusted one with
    // `PairingDialogResponsePending` until *Trust* is tapped, so the request is
    // retried for a while. `ExtendedPairingErrors` makes the device send those
    // specific errors instead of a generic one.
    constexpr auto retry_delay = std::chrono::seconds(1);
    constexpr auto budget = std::chrono::seconds(120);

    // The device public key is needed to build the device certificate that the
    // `PairRecord` carries.
    auto device_public_key = lockdown.get_value({}, "DevicePublicKey");
    if (!device_public_key)
    {
        return tl::unexpected(device_public_key.error());
    }
    const protocol::Plist *value = device_public_key->find("Value");
    if (value == nullptr || !value->data().has_value())
    {
        return tl::unexpected(crypto_error("the device did not send its public key"));
    }
    if (std::getenv("IOSCPP_TRACE") != nullptr)
    {
        const std::span<const std::byte> key = *value->data();
        std::fprintf(stderr, "[device key] size=%zu text=%.*s\n", key.size(), static_cast<int>(key.size()),
                     reinterpret_cast<const char *>(key.data()));
    }

    auto chain = generate_chain(*value->data());
    if (!chain)
    {
        return tl::unexpected(chain.error());
    }
    pairing.impl_->root_certificate = std::move(chain->root_certificate);
    pairing.impl_->root_private_key = std::move(chain->root_private_key);
    pairing.impl_->host_certificate = std::move(chain->host_certificate);
    pairing.impl_->host_private_key = std::move(chain->host_private_key);
    pairing.impl_->device_certificate = std::move(chain->device_certificate);

    // The device's Wi-Fi MAC address is part of the record, and the device may
    // refuse a `Pair` whose record omits it.
    if (auto wifi = lockdown.get_value({}, "WiFiAddress"); wifi)
    {
        if (const protocol::Plist *address = wifi->find("Value"); address != nullptr && address->string().has_value())
        {
            pairing.impl_->wifi_mac_address = std::string(*address->string());
        }
    }

    // The device answers a `Pair` request with its escrow bag. The private keys
    // are not sent. The `ProtocolVersion` sits next to `PairRecord`, not inside
    // it, matching `lockdownd_do_pair`:
    //   https://github.com/libimobiledevice/libimobiledevice/blob/master/src/lockdown.c
    protocol::Plist::Dictionary record{
        {"DeviceCertificate", protocol::Plist(pairing.impl_->device_certificate)},
        {"HostCertificate", protocol::Plist(pairing.impl_->host_certificate)},
        {"HostID", protocol::Plist(pairing.impl_->host_id)},
        {"RootCertificate", protocol::Plist(pairing.impl_->root_certificate)},
        {"SystemBUID", protocol::Plist(pairing.impl_->system_buid)},
    };
    if (!pairing.impl_->wifi_mac_address.empty())
    {
        record.emplace("WiFiMACAddress", protocol::Plist(pairing.impl_->wifi_mac_address));
    }

    const auto deadline = std::chrono::steady_clock::now() + budget;
    Result<protocol::Plist> answer = tl::unexpected(Error{ErrorCode::Device, "pairing was not attempted"});
    for (;;)
    {
        protocol::Plist::Dictionary request{
            {"PairRecord", protocol::Plist::dictionary(record)},
            {"PairingOptions", protocol::Plist::dictionary({{"ExtendedPairingErrors", protocol::Plist(true)}})},
            {"ProtocolVersion", protocol::Plist("2")},
            {"Request", protocol::Plist("Pair")},
        };
        answer = lockdown.request(protocol::Plist::dictionary(std::move(request)));
        if (std::getenv("IOSCPP_TRACE") != nullptr)
        {
            std::fprintf(stderr, "[pair] %s\n", answer ? "ok" : answer.error().message.c_str());
        }
        if (answer)
        {
            break;
        }
        const std::string_view message = answer.error().message;
        if ((message != "PasswordProtected" && message != "PairingDialogResponsePending") ||
            std::chrono::steady_clock::now() >= deadline)
        {
            return tl::unexpected(answer.error());
        }
        std::this_thread::sleep_for(retry_delay);
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
    mbedtls_x509_crt root_certificate{};
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
        mbedtls_x509_crt_free(&root_certificate);
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
    if (std::getenv("IOSCPP_TRACE") != nullptr)
    {
        std::fprintf(stderr, "[tls send] len=%zu first=%02x%02x%02x%02x%02x\n", length, data[0], data[1], data[2],
                     data[3], data[4]);
        if (data[0] == 0x16)
        {
            const char *path = std::getenv("IOSCPP_DUMP");
            if (path != nullptr)
            {
                FILE *file = std::fopen(path, "ab");
                if (file != nullptr)
                {
                    std::fwrite(data, 1, length, file);
                    std::fclose(file);
                }
            }
        }
    }
    // The stream write is exact and blocks until it has the bytes or the link
    // breaks, so the whole record is reported as sent.
    const Status status = stream->write(std::span<const std::byte>(reinterpret_cast<const std::byte *>(data), length));
    return status ? static_cast<int>(length) : MBEDTLS_ERR_SSL_WANT_WRITE;
}

int tls_recv(void *context, unsigned char *data, std::size_t length)
{
    auto *stream = static_cast<Stream *>(context);
    const Status read = stream->read_exact(std::span<std::byte>(reinterpret_cast<std::byte *>(data), length));
    // The stream read is exact and blocks until it has the bytes or the link
    // breaks, so a failure is the end of the connection rather than a retry.
    return read ? static_cast<int>(length) : MBEDTLS_ERR_SSL_CONN_EOF;
}

#if defined(MBEDTLS_DEBUG_C)
void tls_debug(void *context, int level, const char *file, int line, const char *message)
{
    (void)context;
    (void)file;
    (void)line;
    std::fprintf(stderr, "[mbedtls %d] %s\n", level, message);
}
#endif

/// Accepts the device certificate whatever its chain says, matching
/// `cert_verify_cb` in `idevice_connection_enable_ssl`:
///   https://github.com/libimobiledevice/libimobiledevice/blob/master/src/idevice.c
int tls_verify(void *context, mbedtls_x509_crt *certificate, int depth, std::uint32_t *flags)
{
    (void)context;
    (void)certificate;
    (void)depth;
    (void)flags;
    return 0;
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
    mbedtls_x509_crt_init(&impl.root_certificate);
    mbedtls_x509_crt_init(&impl.host_certificate);
    mbedtls_pk_init(&impl.host_key);

    if (std::getenv("IOSCPP_TRACE") != nullptr)
    {
        std::fprintf(stderr, "[tls] host cert=%zu key=%zu device cert=%zu root cert=%zu\n",
                     pairing.host_certificate().size(), pairing.host_private_key().size(),
                     pairing.device_certificate().size(), pairing.root_certificate().size());
    }

    // `lockdownd` pairs against the host leaf, which is the certificate the
    // device stored in its pair record, so the host leaf is presented.
    const std::span<const std::byte> identity_certificate = pairing.host_certificate();
    const std::span<const std::byte> identity_key = pairing.host_private_key();
    if (mbedtls_x509_crt_parse(&impl.host_certificate,
                               reinterpret_cast<const unsigned char *>(identity_certificate.data()),
                               identity_certificate.size()) != 0)
    {
        return tl::unexpected(crypto_error("the host certificate could not be parsed"));
    }
    if (mbedtls_pk_parse_key(&impl.host_key, reinterpret_cast<const unsigned char *>(identity_key.data()),
                             identity_key.size(), nullptr, 0, rng, nullptr) != 0)
    {
        return tl::unexpected(crypto_error("the host private key could not be parsed"));
    }
    if (mbedtls_x509_crt_parse(&impl.device_certificate,
                               reinterpret_cast<const unsigned char *>(pairing.device_certificate().data()),
                               pairing.device_certificate().size()) != 0)
    {
        return tl::unexpected(crypto_error("the device certificate could not be parsed"));
    }
    // The device's TLS certificate is signed by the root certificate the pairing
    // chain carries, so the root is the CA to trust, falling back to the device
    // certificate for a record written before the chain existed.
    mbedtls_x509_crt *authority = &impl.device_certificate;
    if (!pairing.root_certificate().empty())
    {
        if (mbedtls_x509_crt_parse(&impl.root_certificate,
                                   reinterpret_cast<const unsigned char *>(pairing.root_certificate().data()),
                                   pairing.root_certificate().size()) != 0)
        {
            return tl::unexpected(crypto_error("the root certificate could not be parsed"));
        }
        authority = &impl.root_certificate;
    }

    if (mbedtls_ssl_config_defaults(&impl.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0)
    {
        return tl::unexpected(crypto_error("the TLS configuration could not be set"));
    }
    // The device certificate is accepted whatever its chain says, matching
    // `cert_verify_cb` in `idevice_connection_enable_ssl`, so the mode is
    // required with a callback that always accepts.
    mbedtls_ssl_conf_authmode(&impl.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_verify(&impl.conf, tls_verify, nullptr);
    mbedtls_ssl_conf_rng(&impl.conf, rng, nullptr);
    mbedtls_ssl_conf_ca_chain(&impl.conf, authority, nullptr);
    if (mbedtls_ssl_conf_own_cert(&impl.conf, &impl.host_certificate, &impl.host_key) != 0)
    {
        return tl::unexpected(crypto_error("the host certificate could not be configured"));
    }
#if defined(MBEDTLS_DEBUG_C)
    if (std::getenv("IOSCPP_TRACE") != nullptr)
    {
        mbedtls_ssl_conf_dbg(&impl.conf, tls_debug, nullptr);
        mbedtls_debug_set_threshold(4);
    }
#endif

    if (mbedtls_ssl_setup(&impl.ssl, &impl.conf) != 0)
    {
        return tl::unexpected(crypto_error("the TLS session could not be created"));
    }
    if (std::getenv("IOSCPP_TRACE") != nullptr)
    {
        std::fprintf(stderr, "[tls] configured version=%s\n", mbedtls_ssl_get_version(&impl.ssl));
    }
    mbedtls_ssl_set_bio(&impl.ssl, &stream, tls_send, tls_recv, nullptr);
    // `libimobiledevice`'s `idevice_connection_enable_ssl` never passes a
    // session here, and a hand-built session is a trap: `set_session` copies it
    // into `session_negotiate` and sets `handshake->resume`, and a session that
    // only carries an id leaves `tls_version` at zero, so the ClientHello is then
    // written with legacy version `0x0000` and no cipher suite but the SCSV.
    //   https://github.com/libimobiledevice/libimobiledevice/blob/master/src/idevice.c

    int result = 0;
    while ((result = mbedtls_ssl_handshake(&impl.ssl)) != 0)
    {
        if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            if (std::getenv("IOSCPP_TRACE") != nullptr)
            {
                std::fprintf(stderr, "[tls] handshake result=%d (-0x%04x)\n", result, -result);
            }
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
