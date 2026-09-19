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
// and loses its data.

#include "ioscpp/device.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "ioscpp/afc.hpp"
#include "ioscpp/app.hpp"
#include "ioscpp/crypto/pairing.hpp"
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

} // namespace

int main()
{
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

    auto transport = ioscpp::usb::UsbTransport::open(selected);
    if (!transport)
    {
        std::cerr << "open: " << transport.error().message << "\n";
        return 1;
    }

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

    auto device = ioscpp::Device::connect(*transport, *pairing);
    if (!device)
    {
        std::cerr << "connect: " << device.error().message << "\n";
        return 1;
    }

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
    auto afc = device->open_afc();
    ok = check(afc.has_value(), "the AFC service did not start") && ok;
    if (!afc)
    {
        return 1;
    }

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

    // Apps: the install/uninstall round trip is opt-in because it replaces the
    // bundle and loses its data. `IOSCPP_TEST_IPA` and `IOSCPP_TEST_BUNDLE` name a
    // disposable app the user has agreed to replace. On iOS 17+ the mux link's
    // `installation_proxy` accepts the connection but does not answer, so this is
    // blocked on the `RSD` tunnel (`docs/04-blockers.md`).
    const char *ipa = std::getenv("IOSCPP_TEST_IPA");
    const char *bundle = std::getenv("IOSCPP_TEST_BUNDLE");
    if (ipa != nullptr && bundle != nullptr)
    {
        std::cout << "app: " << bundle << " from " << ipa << "\n";

        auto installed = ioscpp::install(*device, ipa);
        ok = check(installed.has_value(), "install returned an error") && ok;
        if (!installed)
        {
            std::cerr << "install: " << installed.error().message << "\n";
        }
        else if (!installed->success)
        {
            std::cerr << "install: " << installed->failure_reason() << "\n";
        }
        ok = check(installed.has_value() && installed->success, "the device refused the install") && ok;

        if (installed && installed->success)
        {
            auto uninstalled = ioscpp::uninstall(*device, bundle);
            ok = check(uninstalled.has_value(), "uninstall returned an error") && ok;
            if (!uninstalled)
            {
                std::cerr << "uninstall: " << uninstalled.error().message << "\n";
            }
            else if (!uninstalled->success)
            {
                std::cerr << "uninstall: " << uninstalled->failure_reason() << "\n";
            }
            ok = check(uninstalled.has_value() && uninstalled->success, "the device refused the uninstall") && ok;
        }
    }

    std::filesystem::remove(local);
    std::filesystem::remove(back);
    return ok ? 0 : 1;
}
