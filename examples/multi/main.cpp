// The parallel tour: every feature once against every attached device.
//
// This is the same tour as `examples/demo`, run for every device that is
// attached, one thread per device: a `Device`, a `Stream`, and a `Connection` are
// not thread-safe, but different devices are independent, so one thread per device
// is how several are driven at once.
//
// Read this file next to its output. Every line is prefixed with the device's USB
// serial, so two devices are told apart. Stop `usbmuxd` first, and tap the *Trust
// This Computer?* prompt on each device when it appears.
//
// The file steps ride the mux link, which every iOS version speaks. The app steps
// need iOS 17.4 or later, because the installer and the developer tools moved
// behind the `CoreDevice` tunnel there; on an older device the tour stops after
// the file steps. Each device's install replaces any existing copy of the bundle,
// so it loses that bundle's data.
//
// Usage: ioscpp_multi_example <bundle-id> <app.ipa>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ioscpp/afc.hpp"
#include "ioscpp/app.hpp"
#include "ioscpp/crypto/pairing.hpp"
#include "ioscpp/device.hpp"
#include "ioscpp/rsd.hpp"
#include "ioscpp/usb/usb_transport.hpp"

namespace
{

/// Guards `std::cout`, so two devices' lines do not interleave.
std::mutex g_output;

/// Prints one whole line for `serial`, prefixed so two devices are told apart.
void report(const std::string &serial, const std::string &text)
{
    const std::lock_guard<std::mutex> lock(g_output);
    std::cout << serial << ": " << text << "\n";
}

/// Runs the whole tour against one device, returning whether every step worked.
bool run_tour(const ioscpp::usb::DeviceId &id, const std::string &bundle_id, const std::filesystem::path &ipa)
{
    using namespace ioscpp;

    // Pair if needed, start the lockdown session, and read the identity.
    auto transport = usb::UsbTransport::open(id);
    if (!transport)
    {
        report(id.serial, "open: " + transport.error().message);
        return false;
    }
    auto pairing = crypto::Pairing::load_for_udid(id.serial);
    if (!pairing)
    {
        report(id.serial, "pairing: " + pairing.error().message);
        return false;
    }
    auto device = Device::connect(*transport, *pairing);
    if (!device)
    {
        report(id.serial, "connect: " + device.error().message);
        return false;
    }
    report(id.serial, std::string(device->udid()) + " " + std::string(device->product_type()) + " " +
                          std::string(device->product_version()));

    // Open AFC, list the media root, and round-trip a file through
    // /PublicStaging. The local files carry the serial, so two devices do not
    // share one.
    auto afc = device->open_afc();
    if (!afc)
    {
        report(id.serial, "afc: " + afc.error().message);
        return false;
    }
    if (auto entries = afc->list("/"); entries)
    {
        report(id.serial, "the media root lists " + std::to_string(entries->size()) + " entries");
    }
    const std::filesystem::path directory = std::filesystem::temp_directory_path();
    const std::filesystem::path local = directory / ("ioscpp_multi_" + id.serial + ".bin");
    const std::filesystem::path back = directory / ("ioscpp_multi_" + id.serial + ".back");
    const std::string remote = "/PublicStaging/ioscpp_multi_" + id.serial + ".bin";
    {
        const std::vector<std::byte> payload(200 * 1024, std::byte{0x5a});
        std::ofstream out(local, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }
    if (auto pushed = afc->push(local, remote); !pushed)
    {
        report(id.serial, "push: " + pushed.error().message);
        return false;
    }
    if (auto info = afc->stat(remote); info && info->has_value())
    {
        report(id.serial, "pushed " + remote + " (" + std::to_string((*info)->size) + " bytes)");
    }
    if (auto pulled = afc->pull(remote, back); !pulled)
    {
        report(id.serial, "pull: " + pulled.error().message);
        return false;
    }

    // Open the iOS 17.4+ CoreDevice tunnel and the RSD connection. The app
    // steps need 17.4 or later, so an older device stops after the file steps.
    auto tunnel = device->tunnel();
    if (!tunnel)
    {
        report(id.serial, "the app steps need iOS 17.4 or later; stopping after the file steps");
        std::filesystem::remove(local);
        std::filesystem::remove(back);
        return true;
    }
    auto rsd = Rsd::connect(*tunnel, rsd_uuid(pairing->host_id()));
    if (!rsd)
    {
        report(id.serial, "rsd: " + rsd.error().message);
        return false;
    }
    report(id.serial, "rsd: " + std::to_string(rsd->services().size()) + " services");

    // Install over the RSD shims, launch over DTX, check it runs, close it, and
    // uninstall it.
    auto installed = install(*rsd, ipa);
    if (!installed)
    {
        report(id.serial, "install: " + installed.error().message);
        return false;
    }
    if (!installed->success)
    {
        report(id.serial, "install refused: " + installed->failure_reason());
        return false;
    }
    report(id.serial, "installed " + bundle_id);

    auto launched = launch(*rsd, bundle_id);
    if (!launched)
    {
        report(id.serial, "launch: " + launched.error().message);
        return false;
    }
    report(id.serial, "launched pid " + std::to_string(*launched));
    auto running = is_running(*rsd, bundle_id);
    report(id.serial, running.has_value() && *running ? "running" : "not running");

    // Leave the app on screen for a moment, so a person watching the run can see
    // it launch. Every device's tour waits at once, so the run stays 15s in all.
    report(id.serial, "leaving the app up for 15s");
    std::this_thread::sleep_for(std::chrono::seconds(15));

    if (auto closed = close(*rsd, *launched); !closed)
    {
        report(id.serial, "close: " + closed.error().message);
    }

    auto uninstalled = uninstall(*rsd, bundle_id);
    if (!uninstalled)
    {
        report(id.serial, "uninstall: " + uninstalled.error().message);
        return false;
    }
    if (!uninstalled->success)
    {
        report(id.serial, "uninstall refused: " + uninstalled->failure_reason());
        return false;
    }
    report(id.serial, "uninstalled " + bundle_id);

    std::filesystem::remove(local);
    std::filesystem::remove(back);
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    using namespace ioscpp;

    if (argc < 3)
    {
        std::cerr << "usage: ioscpp_multi_example <bundle-id> <app.ipa>\n";
        return 2;
    }
    const std::string bundle_id = argv[1];
    const std::filesystem::path ipa = argv[2];

    auto devices = usb::UsbTransport::list();
    if (!devices || devices->empty())
    {
        std::cerr << "no device attached\n";
        return 1;
    }
    std::cout << devices->size() << " device(s) attached\n";

    // One thread per device: they are independent, so every tour runs at once.
    // Each result is a distinct element, so the threads do not share one.
    std::vector<std::thread> tours;
    std::vector<char> results(devices->size(), 0);
    for (std::size_t i = 0; i < devices->size(); ++i)
    {
        tours.emplace_back(
            [&, i]
            {
                results[i] = run_tour((*devices)[i], bundle_id, ipa) ? 1 : 0;
            });
    }
    for (std::thread &tour : tours)
    {
        tour.join();
    }

    bool ok = true;
    for (const char result : results)
    {
        ok = result != 0 && ok;
    }
    return ok ? 0 : 1;
}
