// Pushes a file to the device and pulls it back, timing both.
//
// Usage: ioscpp_transfer_example <local-file> [<remote-path>]
//
// Stop `usbmuxd` first. The default remote path is
// `/PublicStaging/ioscpp_transfer.bin`, which is a disposable location.

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "ioscpp/crypto/pairing.hpp"
#include "ioscpp/device.hpp"
#include "ioscpp/usb/usb_transport.hpp"

namespace
{

double seconds_since(const std::chrono::steady_clock::time_point &start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

int main(int argc, char **argv)
{
    using namespace ioscpp;

    if (argc < 2)
    {
        std::cerr << "usage: ioscpp_transfer_example <local-file> [<remote-path>]\n";
        return 2;
    }
    const std::filesystem::path local = argv[1];
    const std::string remote = argc > 2 ? argv[2] : "/PublicStaging/ioscpp_transfer.bin";

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
    auto afc = device->open_afc();
    if (!afc)
    {
        std::cerr << "afc: " << afc.error().message << "\n";
        return 1;
    }

    auto start = std::chrono::steady_clock::now();
    if (Status status = afc->push(local, remote); !status)
    {
        std::cerr << "push: " << status.error().message << "\n";
        return 1;
    }
    const double pushed = seconds_since(start);

    start = std::chrono::steady_clock::now();
    if (Status status = afc->pull(remote, local.filename().string() + ".back"); !status)
    {
        std::cerr << "pull: " << status.error().message << "\n";
        return 1;
    }
    const double pulled = seconds_since(start);

    std::cout << "push: " << pushed << "s, pull: " << pulled << "s\n";
    return 0;
}
