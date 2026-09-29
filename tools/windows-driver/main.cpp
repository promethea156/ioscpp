// Binds a libusb-compatible driver to the attached device, without Zadig.
//
// The step is the one thing the Windows setup cannot do from the library alone:
// Apple's driver owns the mux interface, so a libusb-compatible driver has to be
// bound to it. This is the programmatic form of the step Zadig does by hand, and
// it mirrors `tools/install-windows-driver.ps1`.
//
// Usage:
//   ioscpp_windows_driver                 # bind the attached device
//   ioscpp_windows_driver --list            # report, change nothing
//   ioscpp_windows_driver --force           # rebind even if already libusb0
//   ioscpp_windows_driver --package <dir>
//   ioscpp_windows_driver --no-sign
//   ioscpp_windows_driver --uninstall
//
// An elevated process is needed, because installing a kernel driver does.

#include <iostream>
#include <string>

#include "ioscpp/log.hpp"
#include "ioscpp/usb/windows_driver.hpp"

int main(int argc, char **argv)
{
    using namespace ioscpp;
    using namespace ioscpp::usb;

    DriverOptions options;
    bool list = false;
    bool uninstall = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--list")
        {
            list = true;
        }
        else if (argument == "--uninstall")
        {
            uninstall = true;
        }
        else if (argument == "--force")
        {
            options.force = true;
        }
        else if (argument == "--no-sign")
        {
            options.sign = false;
        }
        else if (argument == "--package" && i + 1 < argc)
        {
            options.package = argv[++i];
        }
        else if (argument == "--config" && i + 1 < argc)
        {
            options.config_value = static_cast<unsigned int>(std::stoul(argv[++i]));
        }
        else
        {
            std::cerr << "usage: ioscpp_windows_driver [--list] [--uninstall] [--force] [--no-sign] "
                         "[--package <dir>] [--config <value>]\n";
            return 2;
        }
    }

    set_logger(
        [](LogLevel, std::string_view message)
        {
            std::cout << message << "\n";
        },
        LogLevel::Info);

    if (uninstall)
    {
        if (Status status = uninstall_driver(); !status)
        {
            std::cerr << "uninstall: " << status.error().message << "\n";
            return 1;
        }
        return 0;
    }

    auto targets = driver_targets();
    if (!targets)
    {
        std::cerr << "targets: " << targets.error().message << "\n";
        return 1;
    }
    if (targets->empty())
    {
        std::cerr << "no Apple device with a mux interface is attached\n";
        return 1;
    }

    for (const DriverTarget &target : *targets)
    {
        std::cout << target.instance_id
                  << "\n  current driver: " << (target.service.empty() ? "<none>" : target.service) << "\n";
    }
    if (list)
    {
        return 0;
    }

    bool failed = false;
    for (const DriverTarget &target : *targets)
    {
        if (Status status = install_driver(target, options); !status)
        {
            std::cerr << target.instance_id << ": " << status.error().message << "\n";
            failed = true;
        }
    }
    return failed ? 1 : 0;
}
