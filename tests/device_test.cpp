// The device integration test. It skips with code 77 when no device is attached,
// so it is safe to run on a host without hardware.
//
// `IOSCPP_TEST_SERIAL` picks one device when several are attached; otherwise the
// first is used. The test asserts the transport opened that exact device, so a
// descriptor walk or claim that reached the wrong one fails here.
//
// It writes `/PublicStaging/ioscpp_device_test.bin` on the device, which it also
// removes, so it leaves the media partition as it found it.
//
// The app install/uninstall round trip is opt-in: `IOSCPP_TEST_IPA` and
// `IOSCPP_TEST_BUNDLE` name a disposable app, and the round trip replaces it
// and loses its data. The IPA must be development-signed with the device in its
// provisioning profile; an unsigned IPA is refused with
// `ApplicationVerificationFailed`.
//
// The tunnel step opens the iOS 17.4+ CoreDevice tunnel and checks the RSD
// address, port, and MTU, then opens a userspace TCP link to the RSD port; it is
// skipped on an older device.
//
// The lifecycle step disconnects, re-discovers the device by serial, and
// reconnects on a fresh transport. `IOSCPP_TEST_REPLUG` waits for a physical
// unplug and replug before the reconnect, so the re-enumeration is exercised.

#include "ioscpp/device.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ioscpp/afc.hpp"
#include "ioscpp/app.hpp"
#include "ioscpp/crypto/pairing.hpp"
#include "ioscpp/rsd.hpp"
#include "ioscpp/tcp_link.hpp"
#include "ioscpp/usb/usb_transport.hpp"

namespace
{

/// A plain check, since this test does not link the Catch2 runner.
bool check(bool condition, const char *what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << "\n";
    }
    return condition;
}

/// The host UUID the RSD handshake identifies this peer with, derived from the
/// pairing record's host id. It is stable between runs and processes, because the
/// device re-attaches the tunnel when the UUID changes.
ioscpp::RsdUuid rsd_uuid(std::string_view host_id)
{
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char c : host_id)
    {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001b3ULL;
    }
    ioscpp::RsdUuid uuid{};
    for (std::size_t i = 0; i < uuid.size(); ++i)
    {
        uuid[i] = static_cast<std::byte>((hash >> ((i % 8) * 8)) & 0xff);
    }
    return uuid;
}

/// Whether `version` (for example `18.7.8`) is at least `major.minor`.
bool version_at_least(std::string_view version, int major, int minor)
{
    int parsed_major = 0;
    int parsed_minor = 0;
    std::sscanf(std::string(version).c_str(), "%d.%d", &parsed_major, &parsed_minor);
    return parsed_major > major || (parsed_major == major && parsed_minor >= minor);
}

} // namespace

int main()
{
    // A crash must not swallow the progress already printed, so every write is
    // flushed immediately.
    std::cout << std::unitbuf;

    auto devices = ioscpp::usb::UsbTransport::list();
    if (!devices)
    {
        std::cerr << "list: " << devices.error().message << "\n";
        return 1;
    }

    const char *wanted = std::getenv("IOSCPP_TEST_SERIAL");
    ioscpp::usb::DeviceId selected;
    bool found = false;
    for (const ioscpp::usb::DeviceId &id : *devices)
    {
        if (wanted == nullptr || id.serial == wanted)
        {
            selected = id;
            found = true;
            break;
        }
    }
    if (!found)
    {
        std::cout << "no matching device attached; skipping\n";
        return 77;
    }

    std::cout << "device: " << selected.serial << "\n";

    auto opened = ioscpp::usb::UsbTransport::open(selected);
    if (!opened)
    {
        std::cerr << "open: " << opened.error().message << "\n";
        return 1;
    }
    // The transport is held in an `optional`, not the `Result`, so a reconnect
    // can destroy it and open a fresh one.
    std::optional<ioscpp::usb::UsbTransport> transport = std::move(*opened);

    // The transport reports the serial of the device it opened, so it proves the
    // descriptor walk and the claim reached the selected device.
    bool ok = check(!transport->serial().empty(), "the transport reported no serial");
    ok = check(transport->serial() == selected.serial, "the transport opened another device") && ok;

    // The device may show the trust prompt, so a fresh host pairs here.
    auto pairing = ioscpp::crypto::Pairing::load_for_udid(selected.serial);
    if (!pairing)
    {
        std::cerr << "pairing: " << pairing.error().message << "\n";
        return 1;
    }

    auto connected = ioscpp::Device::connect(*transport, *pairing);
    if (!connected)
    {
        std::cerr << "connect: " << connected.error().message << "\n";
        return 1;
    }
    std::optional<ioscpp::Device> device = std::move(*connected);

    std::cout << "udid: " << device->udid() << "\n";
    std::cout << "product: " << device->product_type() << " " << device->product_version() << "\n";

    ok = check(!device->udid().empty(), "the device reported no udid") && ok;
    ok = check(!device->product_type().empty(), "the device reported no product type") && ok;
    ok = check(!device->product_version().empty(), "the device reported no product version") && ok;

    // `connect` completes the pairing exchange and saves the record, so the
    // in-memory record is paired and a fresh load of it is already paired, which
    // is what keeps the next run from asking for trust again.
    ok = check(pairing->paired(), "the pairing exchange did not complete") && ok;
    auto reloaded = ioscpp::crypto::Pairing::load_for_udid(selected.serial);
    ok = check(reloaded.has_value() && reloaded->paired(), "the saved pairing record was not reusable") && ok;

    // AFC: listing the media root proves the framing and the entry parse, and the
    // push/pull round trip proves the file operations. The payload spans several
    // chunks, so a short read or a missed end-of-file shows up as a size mismatch.
    auto opened_afc = device->open_afc();
    ok = check(opened_afc.has_value(), "the AFC service did not start") && ok;
    if (!opened_afc)
    {
        return 1;
    }
    // The AFC client is held in an `optional`, so the reconnect can destroy it
    // before the connection closes, sending its reset while the link is still up.
    std::optional<ioscpp::Afc> afc = std::move(*opened_afc);

    auto entries = afc->list("/");
    ok = check(entries.has_value(), "listing the media root failed") && ok;
    if (entries)
    {
        ok = check(!entries->empty(), "the media root listed no entry") && ok;
    }

    // A `READ_DIR` answer carries the entry names alone, so the directory check
    // is a `stat` of the root itself.
    auto root = afc->stat("/");
    ok = check(root.has_value() && root->has_value() && (*root)->is_directory(),
               "the media root did not stat as a directory") &&
         ok;

    // A path that does not exist is an empty `optional`, not an error, and a
    // listing of one is a device error rather than a silent empty listing.
    auto missing = afc->stat("/ioscpp-does-not-exist");
    ok = check(missing.has_value() && !missing->has_value(), "stat of a missing path was not empty") && ok;
    auto bad = afc->list("/ioscpp-does-not-exist");
    ok = check(!bad.has_value() && bad.error().code == ioscpp::ErrorCode::Device,
               "listing a missing directory did not report a device error") &&
         ok;

    const std::filesystem::path directory = std::filesystem::temp_directory_path();
    const std::filesystem::path local = directory / "ioscpp_device_test.bin";
    const std::filesystem::path back = directory / "ioscpp_device_test.back";
    const std::string remote = "/PublicStaging/ioscpp_device_test.bin";

    std::vector<std::byte> payload(200 * 1024);
    for (std::size_t i = 0; i < payload.size(); ++i)
    {
        payload[i] = static_cast<std::byte>((i * 7 + (i >> 8)) & 0xff);
    }
    {
        std::ofstream out(local, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }

    ioscpp::Status pushed = afc->push(local, remote);
    ok = check(pushed.has_value(), "push failed") && ok;
    if (!pushed)
    {
        std::cerr << "push: " << pushed.error().message << "\n";
    }

    if (pushed)
    {
        auto info = afc->stat(remote);
        ok = check(info.has_value() && info->has_value() && (*info)->size == payload.size(),
                   "the pushed file did not stat back at its full size") &&
             ok;
    }

    ioscpp::Status pulled = afc->pull(remote, back);
    ok = check(pulled.has_value(), "pull failed") && ok;
    if (pulled)
    {
        std::vector<std::byte> round_trip(payload.size());
        std::ifstream in(back, std::ios::binary);
        in.read(reinterpret_cast<char *>(round_trip.data()), static_cast<std::streamsize>(round_trip.size()));
        ok = check(in.gcount() == static_cast<std::streamsize>(payload.size()),
                   "the pulled file was shorter than the pushed one") &&
             ok;
        ok = check(round_trip == payload, "the pulled bytes did not match the pushed ones") && ok;
    }

    ioscpp::Status removed = afc->remove(remote);
    ok = check(removed.has_value(), "remove failed") && ok;

    // The CoreDevice tunnel needs iOS 17.4 or later, so the step is skipped on
    // anything older. The handshake returns the RSD address, port, and MTU; the
    // tunnel's link and RSD connection are later increments, so the endpoint is
    // not reachable yet.
    if (version_at_least(device->product_version(), 17, 4))
    {
        auto tunnel = device->tunnel();
        ok = check(tunnel.has_value(), "the CoreDevice tunnel handshake failed") && ok;
        if (!tunnel)
        {
            std::cerr << "tunnel: " << tunnel.error().message << "\n";
        }
        else
        {
            std::cout << "tunnel: address=" << tunnel->address() << " port=" << tunnel->port()
                      << " mtu=" << tunnel->mtu() << "\n";
            ok = check(!tunnel->address().empty(), "the tunnel reported no RSD address") && ok;
            ok = check(tunnel->port() != 0, "the tunnel reported no RSD port") && ok;
            ok = check(tunnel->mtu() != 0, "the tunnel reported no MTU") && ok;

            // The link re-frames the tunnel's IPv6 packets and opens a TCP
            // connection to the RSD port; the RemoteXPC handshake over it is
            // the next increment.
            if (ok)
            {
                auto link = ioscpp::TcpLink::open(*tunnel, tunnel->client_address(), tunnel->mtu());
                ok = check(link.has_value(), "the userspace TCP link did not open") && ok;
                if (!link)
                {
                    std::cerr << "link: " << link.error().message << "\n";
                }
                else
                {
                    ioscpp::Status connected = link->connect(tunnel->address(), tunnel->port());
                    ok = check(connected.has_value(), "the RSD port did not accept the connection") && ok;
                    if (!connected)
                    {
                        std::cerr << "link: " << connected.error().message << "\n";
                    }
                    else
                    {
                        std::cout << "link: connected to the RSD port " << tunnel->port() << "\n";
                        link->close();
                    }

                    // The RSD connection: the device handshake, the service
                    // dictionary, and one service reached over it. The lock-down
                    // services answer a plist `RSDCheckin`, so one is reached and
                    // checked in to prove the whole tunnel.
                    auto rsd = ioscpp::Rsd::connect(*tunnel, rsd_uuid(pairing->host_id()));
                    ok = check(rsd.has_value(), "the RSD connection failed") && ok;
                    if (!rsd)
                    {
                        std::cerr << "rsd: " << rsd.error().message << "\n";
                    }
                    else
                    {
                        ok = check(!rsd->services().empty(), "the RSD listed no service") && ok;
                        std::cout << "rsd: " << rsd->services().size() << " services\n";
                        for (const auto &[name, service] : rsd->services())
                        {
                            if (service.uses_remote_xpc)
                            {
                                continue;
                            }
                            auto service_link = rsd->start_service(name);
                            ok = check(service_link.has_value(), "reaching an RSD service failed") && ok;
                            if (service_link)
                            {
                                std::cout << "rsd: reached " << name << " port=" << service.port << "\n";
                                service_link->close();
                            }
                            break;
                        }

                        // The install/uninstall round trip is destructive: it
                        // replaces the bundle and loses its data, so it only runs
                        // when `IOSCPP_TEST_IPA` names a disposable package
                        // and `IOSCPP_TEST_BUNDLE` names its bundle id.
                        const char *ipa = std::getenv("IOSCPP_TEST_IPA");
                        const char *bundle = std::getenv("IOSCPP_TEST_BUNDLE");
                        if (ipa != nullptr && bundle != nullptr)
                        {
                            auto installed = ioscpp::install(*rsd, ipa);
                            if (!installed)
                            {
                                ok = check(false, "installing over the RSD failed") && ok;
                                std::cerr << "install: " << installed.error().message << "\n";
                            }
                            else if (!installed->success)
                            {
                                ok = check(false, "the device refused the install") && ok;
                                std::cerr << "install: refused: " << installed->failure_reason() << "\n";
                            }
                            else
                            {
                                std::cout << "install: installed " << bundle << "\n";

                                // Process control rides the RSD
                                // `dtservicehub` over DTX: launch the app,
                                // prove it runs, and kill it by pid.
                                auto launched = ioscpp::launch(*rsd, bundle);
                                if (!launched)
                                {
                                    ok = check(false, "launching over the RSD failed") && ok;
                                    std::cerr << "launch: " << launched.error().message << "\n";
                                }
                                else
                                {
                                    std::cout << "launch: launched pid " << *launched << "\n";
                                    auto running = ioscpp::is_running(*rsd, bundle);
                                    ok =
                                        check(running.has_value() && *running, "the launched app is not running") && ok;

                                    // Leave the app on screen for a moment, so a
                                    // person watching the run can see it launch.
                                    std::cout << "launch: leaving the app up for 10s\n";
                                    std::this_thread::sleep_for(std::chrono::seconds(10));

                                    if (auto closed = ioscpp::close(*rsd, *launched); !closed)
                                    {
                                        ok = check(false, "closing over the RSD failed") && ok;
                                        std::cerr << "close: " << closed.error().message << "\n";
                                    }
                                    else
                                    {
                                        std::cout << "launch: closed pid " << *launched << "\n";
                                    }
                                }

                                auto uninstalled = ioscpp::uninstall(*rsd, bundle);
                                if (!uninstalled)
                                {
                                    ok = check(false, "uninstalling over the RSD failed") && ok;
                                    std::cerr << "uninstall: " << uninstalled.error().message << "\n";
                                }
                                else
                                {
                                    ok = check(uninstalled->success, "the device refused the uninstall") && ok;
                                    std::cout << "install: uninstalled " << bundle << "\n";
                                }
                            }
                        }

                        rsd->close();
                    }
                }
            }
        }
    }
    else
    {
        std::cout << "tunnel: skipped on iOS " << device->product_version() << " (needs 17.4+)\n";
    }

    // Lifecycle: close innermost first, then re-discover the device by serial
    // and reconnect on a fresh transport. A reset or replug re-enumerates the
    // device, so its USB address changes and the old handle is stale; the serial
    // is what selects it again. `IOSCPP_TEST_REPLUG` waits for a physical
    // unplug and replug before the reconnect.
    afc.reset();

    device->disconnect();
    device->disconnect(); // idempotent: the second call leaves nothing open
    device.reset();
    transport.reset();

    if (std::getenv("IOSCPP_TEST_REPLUG") != nullptr)
    {
        std::cout << "unplug and replug the device, then press Enter\n";
        std::cin.get();
    }

    auto again = ioscpp::usb::UsbTransport::list();
    ok = check(again.has_value(), "re-listing the devices failed") && ok;
    bool present = false;
    if (again)
    {
        for (const ioscpp::usb::DeviceId &id : *again)
        {
            if (id.serial == selected.serial)
            {
                present = true;
            }
        }
    }
    ok = check(present, "the device did not reappear by serial") && ok;

    auto reopened = ioscpp::usb::UsbTransport::open(selected);
    ok = check(reopened.has_value(), "reopening the device failed") && ok;
    if (reopened)
    {
        transport = std::move(*reopened);
        auto reconnected = ioscpp::Device::connect(*transport, *pairing);
        ok = check(reconnected.has_value(), "reconnect failed") && ok;
        if (reconnected)
        {
            device = std::move(*reconnected);
            ok = check(!device->udid().empty(), "the reconnected device reported no udid") && ok;
            std::cout << "reconnected: " << device->product_type() << " " << device->product_version() << "\n";
            device->disconnect();
        }
    }

    std::filesystem::remove(local);
    std::filesystem::remove(back);
    return ok ? 0 : 1;
}
