// The guided tour: every feature once against one device.
//
// Read this file next to its output. Each `step` is one thing the library can do,
// and the comment above it says what it does on the device and which protocol it
// speaks. Stop `usbmuxd` first, and tap the *Trust This Computer?* prompt when it
// appears.
//
// The file steps ride the mux link, which every iOS version speaks. The app steps
// need iOS 17.4 or later, because the installer and the developer tools moved
// behind the `CoreDevice` tunnel there; on an older device the tour stops after the
// file steps.
//
// Usage: ioscpp_demo_example <bundle-id> <app.ipa>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "ioscpp/afc.hpp"
#include "ioscpp/app.hpp"
#include "ioscpp/crypto/pairing.hpp"
#include "ioscpp/device.hpp"
#include "ioscpp/house_arrest.hpp"
#include "ioscpp/rsd.hpp"
#include "ioscpp/usb/usb_transport.hpp"

namespace
{

void step(int number, const char *what)
{
    std::cout << "\n[" << number << "] " << what << "\n";
}

} // namespace

int main(int argc, char **argv)
{
    using namespace ioscpp;

    if (argc < 3)
    {
        std::cerr << "usage: ioscpp_demo_example <bundle-id> <app.ipa>\n";
        return 2;
    }
    const std::string bundle_id = argv[1];
    const std::filesystem::path ipa = argv[2];

    // 1. Find the device and open its USB interface.
    step(1, "find a device and open its USB interface");
    auto devices = usb::UsbTransport::list();
    if (!devices || devices->empty())
    {
        std::cerr << "no device attached\n";
        return 1;
    }
    auto transport = usb::UsbTransport::open(devices->front());
    if (!transport)
    {
        std::cerr << "open: " << transport.error().message << "\n";
        return 1;
    }

    // 2. Pair if needed, start the lockdown session, and read the identity.
    step(2, "pair if needed, start the session, and read the identity");
    auto pairing = crypto::Pairing::load_for_udid(devices->front().serial);
    if (!pairing)
    {
        std::cerr << "pairing: " << pairing.error().message << "\n";
        return 1;
    }
    auto device = Device::connect(*transport, *pairing);
    if (!device)
    {
        std::cerr << "connect: " << device.error().message << "\n";
        return 1;
    }
    std::cout << device->udid() << " " << device->product_type() << " " << device->product_version() << "\n";

    // 3. Open AFC and list the media root.
    step(3, "open AFC and list the media root");
    auto afc = device->open_afc();
    if (!afc)
    {
        std::cerr << "afc: " << afc.error().message << "\n";
        return 1;
    }
    if (auto entries = afc->list("/"); entries)
    {
        for (const DirEntry &entry : *entries)
        {
            std::cout << (entry.is_directory() ? "d " : "- ") << entry.name << "\n";
        }
    }

    // 4. Push a file into /PublicStaging, stat it, and pull it back (AFC).
    step(4, "push a file, stat it, and pull it back");
    const std::filesystem::path directory = std::filesystem::temp_directory_path();
    const std::filesystem::path local = directory / "ioscpp_demo.txt";
    const std::filesystem::path back = directory / "ioscpp_demo.back";
    const std::string remote = "/PublicStaging/ioscpp_demo.txt";
    {
        // 200 KiB of 0x5a, big enough to span several AFC chunks.
        const std::vector<std::byte> payload(200 * 1024, std::byte{0x5a});
        std::ofstream out(local, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }
    if (auto pushed = afc->push(local, remote); !pushed)
    {
        std::cerr << "push: " << pushed.error().message << "\n";
        return 1;
    }
    if (auto info = afc->stat(remote); info && info->has_value())
    {
        std::cout << "pushed " << remote << " (" << (*info)->size << " bytes)\n";
    }
    if (auto pulled = afc->pull(remote, back); !pulled)
    {
        std::cerr << "pull: " << pulled.error().message << "\n";
        return 1;
    }

    // 5. Open the iOS 17.4+ CoreDevice tunnel and the RSD connection. The
    // tunnel carries the device's IPv6 packets as data, and the RSD connection
    // lists the services the app steps are reached through.
    step(5, "open the CoreDevice tunnel and the RSD connection");
    auto tunnel = device->tunnel();
    if (!tunnel)
    {
        std::cout << "the app steps need iOS 17.4 or later; stopping after the file steps\n";
        return 0;
    }
    std::cout << "tunnel: " << tunnel->address() << ":" << tunnel->port() << " mtu " << tunnel->mtu() << "\n";
    // The RSD handshake is keyed by the host UUID in the pairing record.
    auto rsd = Rsd::connect(*tunnel, rsd_uuid(pairing->host_id()));
    if (!rsd)
    {
        std::cerr << "rsd: " << rsd.error().message << "\n";
        return 1;
    }
    std::cout << "rsd: " << rsd->services().size() << " services\n";

    // 6. Install the app over the RSD shims: the IPA is staged in
    // /PublicStaging over the AFC shim and then installed over the installer
    // shim. The library reports a refused install as `success == false` with the
    // reason rather than a `Result` error; this demo treats it as fatal.
    step(6, "install the app over the RSD shims");
    auto installed = install(*rsd, ipa);
    if (!installed)
    {
        std::cerr << "install: " << installed.error().message << "\n";
        return 1;
    }
    if (!installed->success)
    {
        std::cerr << "install refused: " << installed->failure_reason() << "\n";
        return 1;
    }
    std::cout << "installed " << bundle_id << "\n";

    // 7. Launch the app over DTX, check it runs, and close it. The
    // process-control service is a DTX channel on the RSD `dtservicehub`, not a
    // plist one: the launch returns the process id, the check resolves the
    // bundle id to it, and the close kills it.
    step(7, "launch the app over DTX, check it runs, and close it");
    auto launched = launch(*rsd, bundle_id);
    if (!launched)
    {
        std::cerr << "launch: " << launched.error().message << "\n";
        return 1;
    }
    std::cout << "launched pid " << *launched << "\n";
    auto running = is_running(*rsd, bundle_id);
    std::cout << (running.has_value() && *running ? "running" : "not running") << "\n";

    // Leave the app on screen for a moment, so a person watching the run can
    // see it launch.
    std::cout << "leaving the app up for 15s\n";
    std::this_thread::sleep_for(std::chrono::seconds(15));

    if (auto closed = close(*rsd, *launched); !closed)
    {
        std::cerr << "close: " << closed.error().message << "\n";
    }

    // 8. Open the app's own container over house_arrest, push a file into its
    // Documents, stat it, and pull it back. On iOS 17.4 and later the service
    // rides the RSD shim; the mux-link service is the fallback.
    step(8, "push and pull a file inside the app's container over house_arrest");
    auto container = HouseArrest::start(*rsd, bundle_id);
    if (!container)
    {
        std::cerr << "house_arrest: " << container.error().message << "\n";
        return 1;
    }
    const std::string in_container = "/Documents/ioscpp_demo.txt";
    {
        const std::vector<std::byte> payload(64 * 1024, std::byte{0x5a});
        std::ofstream out(local, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }
    if (auto pushed = container->afc().push(local, in_container); !pushed)
    {
        std::cerr << "container push: " << pushed.error().message << "\n";
        return 1;
    }
    if (auto info = container->afc().stat(in_container); info && info->has_value())
    {
        std::cout << "pushed " << in_container << " (" << (*info)->size << " bytes)\n";
    }
    if (auto pulled = container->afc().pull(in_container, back); !pulled)
    {
        std::cerr << "container pull: " << pulled.error().message << "\n";
        return 1;
    }
    (void)container->afc().remove(in_container);
    container->close();

    // 9. Uninstall the app over the RSD installer shim.
    step(9, "uninstall the app over the RSD shim");
    auto uninstalled = uninstall(*rsd, bundle_id);
    if (!uninstalled)
    {
        std::cerr << "uninstall: " << uninstalled.error().message << "\n";
        return 1;
    }
    if (!uninstalled->success)
    {
        std::cerr << "uninstall refused: " << uninstalled->failure_reason() << "\n";
        return 1;
    }
    std::cout << "uninstalled " << bundle_id << "\n";

    std::filesystem::remove(local);
    std::filesystem::remove(back);
    return 0;
}
