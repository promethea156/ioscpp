// The guided tour: every feature once against one device.
//
// Read this file next to its output. Each `step` is one thing the library can do,
// and the comment above it says what it does on the device and which protocol it
// speaks. Stop `usbmuxd` first, and tap the *Trust This Computer?* prompt when it
// appears.
//
// Usage: ioscpp_demo_example <bundle-id> <app.ipa>

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#include "ioscpp/afc.hpp"
#include "ioscpp/app.hpp"
#include "ioscpp/crypto/pairing.hpp"
#include "ioscpp/device.hpp"
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

    // 4. Push a file into /PublicStaging, stat it, and pull it back.
    step(4, "push a file, stat it, and pull it back");
    const std::filesystem::path local = "ioscpp_demo.txt";
    {
        FILE *file = std::fopen(local.string().c_str(), "wb");
        if (file != nullptr)
        {
            const char text[] = "hello from ioscpp\n";
            (void)std::fwrite(text, 1, sizeof(text) - 1, file);
            std::fclose(file);
        }
    }
    if (auto pushed = afc->push(local, "/PublicStaging/ioscpp_demo.txt"); !pushed)
    {
        std::cerr << "push: " << pushed.error().message << "\n";
    }
    else
    {
        std::cout << "pushed " << local << "\n";
    }

    // 5. Install the app, then launch it, check it runs, and close it.
    step(5, "install the app");
    auto installed = install(*device, ipa);
    if (!installed)
    {
        std::cerr << "install: " << installed.error().message << "\n";
    }
    else if (!installed->success)
    {
        std::cerr << "install refused: " << installed->failure_reason() << "\n";
    }
    else
    {
        std::cout << "installed " << bundle_id << "\n";
    }

    step(6, "launch the app");
    auto launched = launch(*device, bundle_id);
    if (!launched)
    {
        std::cerr << "launch: " << launched.error().message << "\n";
    }
    else
    {
        std::cout << (launched->success ? "launched" : "launch refused") << "\n";
    }

    step(7, "close the app");
    if (auto closed = close(*device, bundle_id); !closed)
    {
        std::cerr << "close: " << closed.error().message << "\n";
    }
    return 0;
}
