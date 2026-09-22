#include <iostream>
#include <string>

#include "ioscpp/ioscpp.hpp"

#ifdef IOSCPP_CONSUMER_HAS_USB
#    include "ioscpp/usb/usb_transport.hpp"
#endif

int main()
{
    const auto value = ioscpp::protocol::Plist::parse_xml(R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict><key>ProductVersion</key><string>18.7.8</string></dict></plist>)");
    if (!value)
    {
        std::cerr << "the plist did not parse\n";
        return 1;
    }

    const ioscpp::protocol::Plist *version = value->find("ProductVersion");
    if (version == nullptr || !version->is_string())
    {
        std::cerr << "the plist has no ProductVersion string\n";
        return 1;
    }
    std::cout << "ioscpp " << version->string_or() << "\n";

#ifdef IOSCPP_CONSUMER_HAS_USB
    const auto devices = ioscpp::usb::UsbTransport::list();
    if (!devices)
    {
        std::cerr << "the USB device list failed\n";
        return 1;
    }
    std::cout << "usb devices: " << devices->size() << "\n";
#endif

    return 0;
}
