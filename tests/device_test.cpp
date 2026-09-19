// The device integration test. It skips with code 77 when no device is attached,
// so it is safe to run on a host without hardware.
//
// `IOSCPP_TEST_SERIAL` picks one device when several are attached; otherwise the
// first is used. The test asserts the transport opened that exact device, so a
// descriptor walk or claim that reached the wrong one fails here.

#include "ioscpp/device.hpp"

#include <cstdlib>
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
    return ok ? 0 : 1;
}
