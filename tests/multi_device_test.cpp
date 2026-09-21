// The multi-device integration test. It skips with code 77 when fewer than two
// devices are attached, so it is safe to run on a host without hardware.
//
// It discovers the attached devices and drives each on its own thread: a `Device`,
// a `Stream`, and a `Connection` are not thread-safe, but different devices are
// independent, so one thread per device is how several are driven at once. It
// asserts every device completes a connect and an AFC round trip in the same run,
// so a regression in discovery, one transport per device, or one thread per device
// fails here. `IOSCPP_TEST_SERIAL` still selects one device, so setting it narrows
// this test to one and it skips; it is opted in by attaching two devices.
//
// Each device writes `/PublicStaging/ioscpp_multi_device_test_<serial>.bin`, which
// it also removes, so it leaves the media partition as it found it.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ioscpp/afc.hpp"
#include "ioscpp/crypto/pairing.hpp"
#include "ioscpp/device.hpp"
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

/// A plain check, since this test does not link the Catch2 runner.
bool check(bool condition, const char *what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << "\n";
    }
    return condition;
}

/// The per-transfer timeout, so a silent bulk transfer gives up quickly.
constexpr unsigned int kTransferTimeoutMs = 5000;

/// The transfer budget once paired. The budget is the total a read waits across
/// retries, so a service that never answers fails within it instead of hanging.
constexpr unsigned int kServiceTransferBudgetMs = 60000;

/// Opens `id`, connects, and round-trips one file over AFC, returning whether
/// every step worked. The local file carries the serial, so two devices do not
/// share one.
bool run_tour(const ioscpp::usb::DeviceId &id)
{
    using namespace ioscpp;

    auto opened = usb::UsbTransport::open(id, kTransferTimeoutMs, kServiceTransferBudgetMs);
    if (!opened)
    {
        report(id.serial, "open: " + opened.error().message);
        return false;
    }
    std::optional<usb::UsbTransport> transport = std::move(*opened);

    // The transport reports the serial of the device it opened, so it proves each
    // thread's descriptor walk and claim reached its own device, not another's.
    bool ok = check(!transport->serial().empty(), "the transport reported no serial");
    ok = check(transport->serial() == id.serial, "the transport opened another device") && ok;

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
    ok = check(!device->udid().empty(), "the device reported no udid") && ok;

    auto opened_afc = device->open_afc();
    if (!opened_afc)
    {
        report(id.serial, "afc: " + opened_afc.error().message);
        return false;
    }
    std::optional<Afc> afc = std::move(*opened_afc);

    auto entries = afc->list("/");
    ok = check(entries.has_value(), "listing the media root failed") && ok;

    const std::filesystem::path directory = std::filesystem::temp_directory_path();
    const std::filesystem::path local = directory / ("ioscpp_multi_device_test_" + id.serial + ".bin");
    const std::filesystem::path back = directory / ("ioscpp_multi_device_test_" + id.serial + ".back");
    const std::string remote = "/PublicStaging/ioscpp_multi_device_test_" + id.serial + ".bin";

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
        report(id.serial, "push: " + pushed.error().message);
    }
    else
    {
        auto info = afc->stat(remote);
        ok = check(info.has_value() && info->has_value() && (*info)->size == payload.size(),
                   "the pushed file did not stat back at its full size") &&
             ok;

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
    }

    std::filesystem::remove(local);
    std::filesystem::remove(back);
    return ok;
}

} // namespace

int main()
{
    std::cout << std::unitbuf;

    auto listed = ioscpp::usb::UsbTransport::list();
    if (!listed)
    {
        std::cerr << "list: " << listed.error().message << "\n";
        return 1;
    }

    // `IOSCPP_TEST_SERIAL` still selects one device, so the same variable that
    // narrows the single-device test to one device narrows this one to one as well,
    // and it skips because one is fewer than two.
    const char *wanted = std::getenv("IOSCPP_TEST_SERIAL");
    std::vector<ioscpp::usb::DeviceId> devices;
    for (const ioscpp::usb::DeviceId &id : *listed)
    {
        if (wanted == nullptr || id.serial == wanted)
        {
            devices.push_back(id);
        }
    }
    if (devices.size() < 2)
    {
        std::cout << "fewer than two devices attached (" << devices.size() << "); skipping\n";
        return 77;
    }
    std::cout << devices.size() << " device(s) attached\n";

    // One thread per device: they are independent, so every device is driven at
    // once. Each thread writes its own element of `results`, so no two share one.
    std::vector<std::thread> tours;
    std::vector<char> results(devices.size(), 0);
    for (std::size_t i = 0; i < devices.size(); ++i)
    {
        tours.emplace_back(
            [&, i]
            {
                results[i] = run_tour(devices[i]) ? 1 : 0;
            });
    }
    for (std::thread &tour : tours)
    {
        tour.join();
    }

    bool ok = true;
    for (const char result : results)
    {
        ok = ok && result != 0;
    }
    return ok ? 0 : 1;
}
