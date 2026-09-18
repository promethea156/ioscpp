#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ssl.h>

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{

// Collects the bytes mbedTLS hands to the transport, so a first flight can be
// inspected without a peer.
struct Captured
{
    std::vector<std::byte> bytes;
};

int send_cb(void *ctx, const unsigned char *data, std::size_t length)
{
    auto *captured = static_cast<Captured *>(ctx);
    captured->bytes.insert(captured->bytes.end(), reinterpret_cast<const std::byte *>(data),
                           reinterpret_cast<const std::byte *>(data) + length);
    return static_cast<int>(length);
}

// The peer never answers, so the handshake stops after the first flight.
int recv_cb(void *ctx, unsigned char *data, std::size_t length)
{
    (void)ctx;
    (void)data;
    (void)length;
    return MBEDTLS_ERR_SSL_WANT_READ;
}

struct ClientHello
{
    std::uint16_t record_version = 0;
    std::uint16_t client_version = 0;
    std::vector<std::uint16_t> ciphers;
};

// How the client presents its session to mbedTLS, mirroring what
// `TlsSession::start` did before and after the fix.
enum class Session
{
    // No session at all, which is what `libimobiledevice` does.
    None,
    // A hand-built session that only carries an id, the trap.
    Bare,
    // A session that also carries a version and a suite, the escape.
    Versioned,
};

// Runs a client handshake against a silent peer and parses the ClientHello it
// sent.
ClientHello capture(Session mode)
{
    static mbedtls_entropy_context entropy;
    static mbedtls_ctr_drbg_context drbg;
    static bool seeded = false;
    if (!seeded)
    {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        REQUIRE(mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, nullptr, 0) == 0);
        seeded = true;
    }

    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    REQUIRE(mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) == 0);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    REQUIRE(mbedtls_ssl_setup(&ssl, &conf) == 0);

    if (mode != Session::None)
    {
        mbedtls_ssl_session session;
        mbedtls_ssl_session_init(&session);
        static const char id[] = "0123456789abcdef0123456789abcdef";
        session.MBEDTLS_PRIVATE(id_len) = sizeof(id) - 1;
        std::memcpy(session.MBEDTLS_PRIVATE(id), id, session.MBEDTLS_PRIVATE(id_len));
        if (mode == Session::Versioned)
        {
            session.MBEDTLS_PRIVATE(tls_version) = MBEDTLS_SSL_VERSION_TLS1_3;
            session.MBEDTLS_PRIVATE(ciphersuite) = 0x1301;
        }
        REQUIRE(mbedtls_ssl_set_session(&ssl, &session) == 0);
        mbedtls_ssl_session_free(&session);
    }

    Captured captured;
    mbedtls_ssl_set_bio(&ssl, &captured, send_cb, recv_cb, nullptr);
    const int rc = mbedtls_ssl_handshake(&ssl);
    REQUIRE((rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE));

    ClientHello hello;
    const std::vector<std::byte> &bytes = captured.bytes;
    REQUIRE(bytes.size() > 9);
    REQUIRE(static_cast<std::uint8_t>(bytes[0]) == 0x16);
    hello.record_version =
        static_cast<std::uint16_t>((static_cast<std::uint8_t>(bytes[1]) << 8) | static_cast<std::uint8_t>(bytes[2]));
    std::size_t offset = 5;
    REQUIRE(static_cast<std::uint8_t>(bytes[offset]) == 0x01);
    offset += 4;
    hello.client_version = static_cast<std::uint16_t>((static_cast<std::uint8_t>(bytes[offset]) << 8) |
                                                      static_cast<std::uint8_t>(bytes[offset + 1]));
    offset += 2 + 32;
    const std::size_t session_id_length = static_cast<std::uint8_t>(bytes[offset]);
    offset += 1 + session_id_length;
    const std::size_t cipher_length = static_cast<std::size_t>((static_cast<std::uint8_t>(bytes[offset]) << 8) |
                                                               static_cast<std::uint8_t>(bytes[offset + 1]));
    offset += 2;
    REQUIRE(offset + cipher_length <= bytes.size());
    for (std::size_t i = 0; i + 1 < cipher_length; i += 2)
    {
        hello.ciphers.push_back(static_cast<std::uint16_t>((static_cast<std::uint8_t>(bytes[offset + i]) << 8) |
                                                           static_cast<std::uint8_t>(bytes[offset + i + 1])));
    }

    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    return hello;
}

} // namespace

TEST_CASE("a plain ClientHello advertises TLS 1.2 and its cipher suites")
{
    const ClientHello hello = capture(Session::None);
    REQUIRE(hello.record_version == 0x0303);
    REQUIRE(hello.client_version == 0x0303);
    REQUIRE(hello.ciphers.size() > 1);
}

TEST_CASE("a bare hand-built session poisons the ClientHello")
{
    const ClientHello hello = capture(Session::Bare);
    // `set_session` copies the session into `session_negotiate` and sets
    // `handshake->resume`, and a session whose `tls_version` is still zero
    // writes record version `0x0000` and filters every suite but the SCSV. The
    // ClientHello's own legacy_version is hardcoded to `0x0303`, so only the
    // record header and the suite list give the poison away.
    REQUIRE(hello.record_version == 0x0000);
    REQUIRE(hello.client_version == 0x0303);
    REQUIRE(hello.ciphers.size() == 1);
    REQUIRE(hello.ciphers.front() == 0x00ff);
}

TEST_CASE("a hand-built session with a version does not poison the ClientHello")
{
    const ClientHello hello = capture(Session::Versioned);
    REQUIRE(hello.record_version == 0x0303);
    REQUIRE(hello.client_version == 0x0303);
    REQUIRE(hello.ciphers.size() > 1);
}
