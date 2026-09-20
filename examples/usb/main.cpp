// Lists every attached iOS device's serial, then connects to the first over USB and
// prints its identity.
//
// Stop `usbmuxd` first, because it claims the same USB interface. On a device
// that has not been trusted by this host, the device shows the *Trust This
// Computer?* prompt and the pairing exchange waits for it, so run this from an
// interactive terminal.

#include <iostream>

#include "ioscpp/crypto/pairing.hpp"
#include "ioscpp/device.hpp"
#include "ioscpp/usb/usb_transport.hpp"

int main()
{
    using namespace ioscpp;

    auto devices = usb::UsbTransport::list();
    if (!devices)
    {
        std::cerr << "list: " << devices.error().message << "\n";
        return 1;
    }
    if (devices->empty())
    {
        std::cout << "no device attached\n";
        return 1;
    }

    for (const usb::DeviceId &id : *devices)
    {
        std::cout << id.serial << "\n";
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

    std::cout << device->udid() << " " << device->product_type() << " " << device->product_version() << "\n";
    return 0;
}
