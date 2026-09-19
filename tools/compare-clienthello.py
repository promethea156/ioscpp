"""Captures and compares a TLS ClientHello against a device-free reference.

`ref`   builds the exact context `pymobiledevice3` uses (Python `ssl`, OpenSSL),
        captures the first flight through `ssl.MemoryBIO`, writes the raw record, and
        prints a parsed summary.
`parse` reads a raw ClientHello record and prints the same summary. Point it at the
        file `IOSCPP_TRACE=1 IOSCPP_DUMP=<file> ioscpp_usb_example` writes.
`diff`  reads two raw records and names every difference.

Usage:
    python compare-clienthello.py ref <out.bin> [pair-record]
    python compare-clienthello.py parse <in.bin>
    python compare-clienthello.py diff <reference.bin> <ioscpp.bin>

The reference context mirrors `ServiceConnection.create_ssl_context`:

    ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    minimum_version = TLSv1_2, maximum_version = TLSv1_3
    set_ciphers("ALL:!aNULL:!eNULL:@SECLEVEL=0")
    options |= OP_LEGACY_SERVER_CONNECT
    check_hostname = False, verify_mode = CERT_NONE
    load_cert_chain(host certificate, host key)

The host certificate in the pair record carries an empty `serialNumber`, which
OpenSSL 3 rejects, so the reference falls back to a generated self-signed one. The
client certificate does not change the ClientHello.
"""

import argparse
import datetime
import os
import plistlib
import ssl

EXT_NAMES = {
    0x0000: "server_name",
    0x0001: "max_fragment_length",
    0x0005: "status_request",
    0x000A: "supported_groups",
    0x000B: "ec_point_formats",
    0x000D: "signature_algorithms",
    0x0010: "application_layer_protocol_negotiation",
    0x0012: "signed_certificate_timestamp",
    0x0015: "padding",
    0x0016: "encrypt_then_mac",
    0x0017: "extended_master_secret",
    0x001B: "compress_certificate",
    0x001C: "record_size_limit",
    0x0022: "delegated_credentials",
    0x0023: "session_ticket",
    0x002A: "early_data",
    0x002B: "supported_versions",
    0x002C: "cookie",
    0x002D: "psk_key_exchange_modes",
    0x002F: "certificate_authorities",
    0x0031: "post_handshake_auth",
    0x0032: "signature_algorithms_cert",
    0x0033: "key_share",
}

GROUP_NAMES = {
    0x0017: "secp256r1",
    0x0018: "secp384r1",
    0x0019: "secp521r1",
    0x001A: "brainpoolP256r1",
    0x001B: "brainpoolP384r1",
    0x001C: "brainpoolP512r1",
    0x001D: "x25519",
    0x001E: "x448",
    0x0100: "ffdhe2048",
    0x0101: "ffdhe3072",
    0x0102: "ffdhe4096",
    0x0103: "ffdhe6144",
    0x0104: "ffdhe8192",
    0x11EC: "x25519mlkem768",
}

SIG_NAMES = {
    0x0201: "rsa_pkcs1_sha1",
    0x0203: "ecdsa_sha1",
    0x0401: "rsa_pkcs1_sha256",
    0x0403: "ecdsa_secp256r1_sha256",
    0x0501: "rsa_pkcs1_sha384",
    0x0503: "ecdsa_secp384r1_sha384",
    0x0601: "rsa_pkcs1_sha512",
    0x0603: "ecdsa_secp521r1_sha512",
    0x0804: "rsa_pss_rsae_sha256",
    0x0805: "rsa_pss_rsae_sha384",
    0x0806: "rsa_pss_rsae_sha512",
    0x0807: "ed25519",
    0x0808: "ed448",
    0x0809: "rsa_pss_pss_sha256",
    0x080A: "rsa_pss_pss_sha384",
    0x080B: "rsa_pss_pss_sha512",
}

VERSION_NAMES = {0x0304: "TLS1.3", 0x0303: "TLS1.2", 0x0302: "TLS1.1", 0x0301: "TLS1.0"}


def u16(data):
    return int.from_bytes(data, "big")


def parse(raw):
    if raw[0] != 0x16:
        raise ValueError(f"not a handshake record: {raw[0]:#x}")
    record_version = raw[1:3]
    body = raw[5 : 5 + u16(raw[3:5])]
    if body[0] != 0x01:
        raise ValueError(f"not a ClientHello: {body[0]:#x}")
    hello = body[4 : 4 + int.from_bytes(body[1:4], "big")]
    parsed = {
        "record_version": record_version.hex(),
        "client_version": hello[0:2].hex(),
        "length": len(raw),
    }
    offset = 2 + 32
    session_id_length = hello[offset]
    parsed["session_id_length"] = session_id_length
    parsed["session_id"] = hello[offset + 1 : offset + 1 + session_id_length].hex()
    offset += 1 + session_id_length
    cipher_length = u16(hello[offset : offset + 2])
    offset += 2
    parsed["ciphers"] = [u16(hello[offset + i : offset + i + 2]) for i in range(0, cipher_length, 2)]
    offset += cipher_length
    compression_length = hello[offset]
    offset += 1
    parsed["compression"] = hello[offset : offset + compression_length].hex()
    offset += compression_length
    extensions_end = offset + 2 + u16(hello[offset : offset + 2])
    offset += 2
    parsed["extensions"] = []
    while offset < extensions_end:
        ext_type = u16(hello[offset : offset + 2])
        ext_length = u16(hello[offset + 2 : offset + 4])
        parsed["extensions"].append((ext_type, hello[offset + 4 : offset + 4 + ext_length]))
        offset += 4 + ext_length
    return parsed


def describe(name, data):
    if name == "supported_groups":
        count = u16(data[0:2])
        return [GROUP_NAMES.get(u16(data[2 + i : 4 + i]), f"{u16(data[2 + i : 4 + i]):#06x}") for i in range(0, count, 2)]
    if name == "key_share":
        groups = []
        offset = 2
        while offset < 2 + u16(data[0:2]):
            group = u16(data[offset : offset + 2])
            key_length = u16(data[offset + 2 : offset + 4])
            groups.append(f"{GROUP_NAMES.get(group, hex(group))} key_len={key_length}")
            offset += 4 + key_length
        return groups
    if name == "signature_algorithms":
        count = u16(data[0:2])
        return [SIG_NAMES.get(u16(data[2 + i : 4 + i]), f"{u16(data[2 + i : 4 + i]):#06x}") for i in range(0, count, 2)]
    if name == "supported_versions":
        return [VERSION_NAMES.get(u16(data[1 + i : 3 + i]), f"{u16(data[1 + i : 3 + i]):#06x}") for i in range(0, data[0], 2)]
    if name == "psk_key_exchange_modes":
        return list(data[1 : 1 + data[0]])
    if name == "certificate_authorities":
        names = []
        offset = 2
        while offset < 2 + u16(data[0:2]):
            length = u16(data[offset : offset + 2])
            names.append(data[offset + 2 : offset + 2 + length].hex()[:24])
            offset += 2 + length
        return names
    return data.hex()


def extensions(parsed):
    return dict(parsed["extensions"])


def show(parsed):
    print(
        f"record_version={parsed['record_version']} client_version={parsed['client_version']} "
        f"length={parsed['length']}"
    )
    print(f"session_id_length={parsed['session_id_length']} session_id={parsed['session_id']}")
    print(f"ciphers ({len(parsed['ciphers'])}): {' '.join(f'{c:#06x}' for c in parsed['ciphers'])}")
    print(f"compression={parsed['compression']}")
    print(f"extensions ({len(parsed['extensions'])}):")
    for ext_type, data in parsed["extensions"]:
        name = EXT_NAMES.get(ext_type, f"unknown-{ext_type:#06x}")
        print(f"  {ext_type:#06x} {name} len={len(data)}: {describe(name, data)}")


def make_self_signed(path):
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID

    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "ioscpp")])
    now = datetime.datetime.now(datetime.timezone.utc)
    certificate = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(1)
        .not_valid_before(now)
        .not_valid_after(now + datetime.timedelta(days=3650))
        .sign(key, hashes.SHA256())
    )
    with open(path + ".crt", "wb") as handle:
        handle.write(certificate.public_bytes(serialization.Encoding.PEM))
    with open(path + ".key", "wb") as handle:
        handle.write(
            key.private_bytes(
                serialization.Encoding.PEM,
                serialization.PrivateFormat.TraditionalOpenSSL,
                serialization.NoEncryption(),
            )
        )
    return path + ".crt", path + ".key"


def default_pair_record():
    home = os.environ.get("USERPROFILE") or os.environ.get("HOME") or "."
    root = os.path.join(home, ".ioscpp")
    entries = sorted(os.listdir(root)) if os.path.isdir(root) else []
    if len(entries) == 1:
        return os.path.join(root, entries[0])
    raise SystemExit("pass the pair record path, or keep exactly one in ~/.ioscpp")


def reference(out_path, pair_record):
    with open(pair_record, "rb") as handle:
        record = plistlib.load(handle)
    certificate = out_path + ".cert.pem"
    key = out_path + ".key.pem"
    with open(certificate, "wb") as handle:
        handle.write(record["HostCertificate"].rstrip(b"\x00"))
    with open(key, "wb") as handle:
        handle.write(record["HostPrivateKey"].rstrip(b"\x00"))

    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.maximum_version = ssl.TLSVersion.TLSv1_3
    if ssl.OPENSSL_VERSION.lower().startswith("openssl"):
        context.set_ciphers("ALL:!aNULL:!eNULL:@SECLEVEL=0")
    else:
        context.set_ciphers("ALL:!aNULL:!eNULL")
    context.options |= 0x4
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    try:
        context.load_cert_chain(certificate, key)
        print("identity: pairing record host certificate")
    except ssl.SSLError as error:
        print(f"identity: pairing record host certificate rejected by OpenSSL ({error}); using a generated one")
        certificate, key = make_self_signed(out_path)
        context.load_cert_chain(certificate, key)

    incoming = ssl.MemoryBIO()
    outgoing = ssl.MemoryBIO()
    session = context.wrap_bio(incoming, outgoing, server_side=False)
    try:
        session.do_handshake()
    except ssl.SSLWantReadError:
        pass
    raw = outgoing.read()
    with open(out_path, "wb") as handle:
        handle.write(raw)
    print(f"wrote {len(raw)} bytes to {out_path}")
    show(parse(raw))


def diff(reference_path, ioscpp_path):
    with open(reference_path, "rb") as handle:
        reference = parse(handle.read())
    with open(ioscpp_path, "rb") as handle:
        ioscpp = parse(handle.read())

    print(f"record_version  reference={reference['record_version']} ioscpp={ioscpp['record_version']}")
    print(f"client_version  reference={reference['client_version']} ioscpp={ioscpp['client_version']}")
    print(f"cipher count    reference={len(reference['ciphers'])} ioscpp={len(ioscpp['ciphers'])}")
    print(f"compression     reference={reference['compression']} ioscpp={ioscpp['compression']}")
    reference_types = {ext_type for ext_type, _ in reference["extensions"]}
    ioscpp_types = {ext_type for ext_type, _ in ioscpp["extensions"]}
    print("extensions only in reference:", [EXT_NAMES.get(t, hex(t)) for t in sorted(reference_types - ioscpp_types)])
    print("extensions only in ioscpp  :", [EXT_NAMES.get(t, hex(t)) for t in sorted(ioscpp_types - reference_types)])
    for ext_type in sorted(reference_types & ioscpp_types):
        name = EXT_NAMES.get(ext_type, hex(ext_type))
        reference_data = extensions(reference)[ext_type]
        ioscpp_data = extensions(ioscpp)[ext_type]
        # A key share carries a fresh public key each run, so only its group
        # and length are comparable.
        if name == "key_share":
            reference_data = describe(name, reference_data)
            ioscpp_data = describe(name, ioscpp_data)
        if reference_data != ioscpp_data:
            print(f"{name}: DIFFERS")
            print(f"  reference: {describe(name, reference_data) if isinstance(reference_data, bytes) else reference_data}")
            print(f"  ioscpp   : {describe(name, ioscpp_data) if isinstance(ioscpp_data, bytes) else ioscpp_data}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    subparsers = parser.add_subparsers(dest="mode", required=True)
    ref = subparsers.add_parser("ref")
    ref.add_argument("out")
    ref.add_argument("pair_record", nargs="?")
    parse_command = subparsers.add_parser("parse")
    parse_command.add_argument("input")
    diff_command = subparsers.add_parser("diff")
    diff_command.add_argument("reference")
    diff_command.add_argument("ioscpp")
    arguments = parser.parse_args()

    if arguments.mode == "ref":
        reference(arguments.out, arguments.pair_record or default_pair_record())
    elif arguments.mode == "parse":
        with open(arguments.input, "rb") as handle:
            show(parse(handle.read()))
    else:
        diff(arguments.reference, arguments.ioscpp)


if __name__ == "__main__":
    main()
