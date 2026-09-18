#include "ioscpp/device.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

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
    if (devices->empty())
    {
        std::cout << "no device attached; skipping\n";
        return 77;
    }

    std::cout << "device: " << devices->front().serial << "\n";

    auto transport = ioscpp::usb::UsbTransport::open(devices->front());
    if (!transport)
    {
        std::cerr << "open: " << transport.error().message << "\n";
        return 1;
    }

    // The device may show the trust prompt, so a fresh host pairs here.
    auto pairing = ioscpp::crypto::Pairing::load_for_udid(devices->front().serial);
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

    bool ok = true;
    ok = check(!device->udid().empty(), "the device reported no udid") && ok;
    ok = check(!device->product_type().empty(), "the device reported no product type") && ok;
    return ok ? 0 : 1;
}
