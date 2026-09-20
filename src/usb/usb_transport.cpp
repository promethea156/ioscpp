#if defined(_WIN32)
#    define NOMINMAX
#endif

#include "ioscpp/usb/usb_transport.hpp"

#include <libusb.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ioscpp::usb
{
namespace
{

// The mux function is a vendor-specific USB interface. The class/subclass/
// protocol triple is fixed by Apple and is how `usbmuxd` locates it (see its
// `src/usb.h`). Bulk transfers carry the mux frames.
constexpr std::uint8_t kMuxInterfaceClass = 0xFF;
constexpr std::uint8_t kMuxInterfaceSubClass = 0xFE;
constexpr std::uint8_t kMuxInterfaceProtocol = 0x02;
// Apple's vendor id; a device in normal mode presents a product id in this range.
constexpr std::uint16_t kAppleVendorId = 0x05AC;

// One bulk transfer holds at most one mux frame. `usbmuxd` sends up to 3 × 16 KiB
// per transfer, so the read buffer is at least that large.
constexpr std::size_t kReadBufferSize = 3 * 16384;

Error fail(std::string_view what, int code)
{
    return Error{ErrorCode::Transport,
                 std::string(what) + ": " + libusb_strerror(static_cast<enum libusb_error>(code))};
}

// Reads the device's USB `iSerial` descriptor string, which is the serial
// `idevice_id -l` prints and selects on. An empty result means the device has no
// serial descriptor, or that it could not be read.
std::string device_serial(libusb_device *device, const libusb_device_descriptor &descriptor)
{
    if (descriptor.iSerialNumber == 0)
    {
        return {};
    }

    libusb_device_handle *handle = nullptr;
    if (libusb_open(device, &handle) != 0)
    {
        return {};
    }

    std::array<unsigned char, 256> buffer{};
    const int length = libusb_get_string_descriptor_ascii(handle, descriptor.iSerialNumber, buffer.data(),
                                                          static_cast<int>(buffer.size()));
    libusb_close(handle);
    if (length <= 0)
    {
        return {};
    }
    // The `iSerial` descriptor is a fixed-size field whose tail is NUL padded, and
    // some backends return that field's length rather than the string's, so the
    // trailing NULs are trimmed. They matter because a path built from the serial
    // is a C string, and `fopen` stops at the first NUL.
    std::size_t size = static_cast<std::size_t>(length);
    while (size > 0 && buffer[size - 1] == 0)
    {
        --size;
    }
    return std::string(reinterpret_cast<const char *>(buffer.data()), size);
}

// Whether `descriptor` can satisfy `id`. A zero vendor or product id matches any,
// so a selector with only a serial finds its device whatever the model.
bool matches(const libusb_device_descriptor &descriptor, DeviceId id)
{
    if (id.vendor_id != 0 && descriptor.idVendor != id.vendor_id)
    {
        return false;
    }
    if (id.product_id != 0 && descriptor.idProduct != id.product_id)
    {
        return false;
    }
    return true;
}

libusb_device *find_device(libusb_device **devices, ssize_t count, DeviceId id)
{
    for (ssize_t i = 0; i < count; ++i)
    {
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(devices[i], &descriptor) != 0)
        {
            continue;
        }
        if (!matches(descriptor, id))
        {
            continue;
        }
        // Reading the serial opens the device, so it is only done when a serial
        // was asked for; without one the first model match is taken.
        if (id.serial.empty() || device_serial(devices[i], descriptor) == id.serial)
        {
            return devices[i];
        }
    }
    return nullptr;
}

// The mux interface, the configuration that carries it, and its bulk endpoints.
struct MuxInterface
{
    int config_value = -1;
    int number = -1;
    std::uint8_t endpoint_in = 0;
    std::uint8_t endpoint_out = 0;
};

// Locates the mux interface (class 0xFF, subclass 0xFE, protocol 0x02) within one
// configuration. An empty optional means this configuration does not carry it.
std::optional<MuxInterface> find_mux_in_config(const libusb_config_descriptor &config)
{
    MuxInterface found;
    for (std::uint8_t i = 0; i < config.bNumInterfaces && found.number < 0; ++i)
    {
        const libusb_interface &interface = config.interface[i];
        for (int j = 0; j < interface.num_altsetting; ++j)
        {
            const libusb_interface_descriptor &altsetting = interface.altsetting[j];
            if (altsetting.bInterfaceClass != kMuxInterfaceClass ||
                altsetting.bInterfaceSubClass != kMuxInterfaceSubClass ||
                altsetting.bInterfaceProtocol != kMuxInterfaceProtocol)
            {
                continue;
            }
            found.config_value = config.bConfigurationValue;
            found.number = altsetting.bInterfaceNumber;
            for (std::uint8_t k = 0; k < altsetting.bNumEndpoints; ++k)
            {
                const libusb_endpoint_descriptor &endpoint = altsetting.endpoint[k];
                if ((endpoint.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK)
                {
                    continue;
                }
                if ((endpoint.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN)
                {
                    found.endpoint_in = endpoint.bEndpointAddress;
                }
                else
                {
                    found.endpoint_out = endpoint.bEndpointAddress;
                }
            }
            break;
        }
    }

    if (found.number < 0 || found.endpoint_in == 0 || found.endpoint_out == 0)
    {
        return std::optional<MuxInterface>{};
    }
    return found;
}

// Locates the mux interface across every configuration, preferring the active
// one so an already-correct device is left alone. An iOS device in its initial USB
// mode carries the mux interface only in a later configuration, which the host has
// to select before it can claim the interface. An empty optional means no
// configuration carries it, and an error means the descriptors could not be read.
Result<std::optional<MuxInterface>> find_mux_interface(libusb_device *device)
{
    libusb_config_descriptor *active = nullptr;
    if (libusb_get_active_config_descriptor(device, &active) == 0)
    {
        const std::optional<MuxInterface> found = find_mux_in_config(*active);
        libusb_free_config_descriptor(active);
        if (found)
        {
            return found;
        }
    }

    libusb_device_descriptor descriptor{};
    const int rc = libusb_get_device_descriptor(device, &descriptor);
    if (rc != 0)
    {
        return tl::unexpected(fail("libusb_get_device_descriptor", rc));
    }

    // The mux-capable configurations are the later ones, so search backwards.
    for (int i = descriptor.bNumConfigurations - 1; i >= 0; --i)
    {
        libusb_config_descriptor *config = nullptr;
        if (libusb_get_config_descriptor(device, static_cast<std::uint8_t>(i), &config) != 0)
        {
            continue;
        }
        const std::optional<MuxInterface> found = find_mux_in_config(*config);
        libusb_free_config_descriptor(config);
        if (found)
        {
            return found;
        }
    }
    return std::optional<MuxInterface>{};
}

} // namespace

Result<DeviceId> DeviceId::parse(std::string_view text)
{
    constexpr std::string_view kSerialPrefix = "serial:";
    if (text.starts_with(kSerialPrefix))
    {
        if (text.size() == kSerialPrefix.size())
        {
            return tl::unexpected(Error{ErrorCode::InvalidArgument, "serial: needs a serial number"});
        }
        DeviceId id;
        id.serial = std::string(text.substr(kSerialPrefix.size()));
        return id;
    }

    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 == text.size())
    {
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "expected VID:PID or serial:<serial>"});
    }
    try
    {
        DeviceId id;
        id.vendor_id = static_cast<std::uint16_t>(std::stoul(std::string(text.substr(0, colon)), nullptr, 16));
        id.product_id = static_cast<std::uint16_t>(std::stoul(std::string(text.substr(colon + 1)), nullptr, 16));
        return id;
    }
    catch (const std::exception &)
    {
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "VID:PID must be hexadecimal"});
    }
}

Result<bool> UsbTransport::is_present(DeviceId id)
{
    libusb_context *context = nullptr;
    const int initialized = libusb_init(&context);
    if (initialized != 0)
    {
        return tl::unexpected(fail("libusb_init", initialized));
    }

    libusb_device **devices = nullptr;
    const ssize_t count = libusb_get_device_list(context, &devices);
    const bool found = count >= 0 && find_device(devices, count, id) != nullptr;
    if (count >= 0)
    {
        libusb_free_device_list(devices, 1);
    }
    libusb_exit(context);
    return found;
}

Result<std::vector<DeviceId>> UsbTransport::list()
{
    libusb_context *context = nullptr;
    const int initialized = libusb_init(&context);
    if (initialized != 0)
    {
        return tl::unexpected(fail("libusb_init", initialized));
    }

    libusb_device **devices = nullptr;
    const ssize_t count = libusb_get_device_list(context, &devices);
    if (count < 0)
    {
        libusb_exit(context);
        return tl::unexpected(fail("libusb_get_device_list", static_cast<int>(count)));
    }

    std::vector<DeviceId> found;
    for (ssize_t i = 0; i < count; ++i)
    {
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(devices[i], &descriptor) != 0)
        {
            continue;
        }
        // A device without a mux interface is not one of ours, so it is skipped
        // rather than reported and then failing to open.
        const auto mux = find_mux_interface(devices[i]);
        if (!mux || !*mux)
        {
            continue;
        }
        // A device whose serial cannot be read cannot be selected, because the
        // serial is the only unambiguous selector: an empty one would alias the
        // first device in `find_device`. Reading the serial opens the device, so a
        // device whose driver does not let libusb open it lands here, and it is
        // skipped like a device without a mux interface.
        std::string serial = device_serial(devices[i], descriptor);
        if (serial.empty())
        {
            continue;
        }
        DeviceId id;
        id.vendor_id = descriptor.idVendor;
        id.product_id = descriptor.idProduct;
        id.serial = std::move(serial);
        found.push_back(std::move(id));
    }
    libusb_free_device_list(devices, 1);
    libusb_exit(context);
    return found;
}

struct UsbTransport::Impl
{
    libusb_context *context = nullptr;
    libusb_device_handle *handle = nullptr;
    int interface_number = -1;
    int config_value = -1;
    std::uint8_t endpoint_in = 0;
    std::uint8_t endpoint_out = 0;
    bool claimed = false;
    std::vector<std::byte> incoming;
    std::size_t incoming_offset = 0;
    std::string serial;
    unsigned int transfer_timeout_ms = kDefaultTransferTimeoutMs;
    unsigned int transfer_budget_ms = kDefaultTransferBudgetMs;

    // Runs one bulk transfer, retrying while it times out having moved nothing.
    //
    // libusb gives up when the peer sends nothing at all for
    // `transfer_timeout_ms`. That silence can be transient, or, during pairing,
    // the user taking their time to tap the trust prompt, so the attempt is
    // repeated until the budget runs out.
    int bulk_transfer(unsigned char endpoint, unsigned char *data, int length, int *transferred)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(transfer_budget_ms);
        while (true)
        {
            const int rc = libusb_bulk_transfer(handle, endpoint, data, length, transferred, transfer_timeout_ms);
            if (rc != LIBUSB_ERROR_TIMEOUT || *transferred > 0)
            {
                return rc;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return rc;
            }
        }
    }

    ~Impl()
    {
        if (handle != nullptr)
        {
            if (claimed)
            {
                libusb_release_interface(handle, interface_number);
            }
            libusb_close(handle);
        }
        if (context != nullptr)
        {
            libusb_exit(context);
        }
    }
};

UsbTransport::UsbTransport()
    : impl_(std::make_unique<Impl>())
{
}

UsbTransport::UsbTransport(UsbTransport &&) noexcept = default;
UsbTransport &UsbTransport::operator=(UsbTransport &&) noexcept = default;

Result<UsbTransport> UsbTransport::open(DeviceId id, unsigned int transfer_timeout_ms, unsigned int transfer_budget_ms)
{
    UsbTransport transport;
    transport.impl_->transfer_timeout_ms = transfer_timeout_ms;
    transport.impl_->transfer_budget_ms = transfer_budget_ms;

    int rc = libusb_init(&transport.impl_->context);
    if (rc != 0)
    {
        return tl::unexpected(fail("libusb_init", rc));
    }

    libusb_device **devices = nullptr;
    const ssize_t count = libusb_get_device_list(transport.impl_->context, &devices);
    if (count < 0)
    {
        return tl::unexpected(fail("libusb_get_device_list", static_cast<int>(count)));
    }

    libusb_device *match = find_device(devices, count, id);
    if (match == nullptr)
    {
        libusb_free_device_list(devices, 1);
        return tl::unexpected(Error{ErrorCode::Transport, "no USB device matching the given id"});
    }

    const auto mux = find_mux_interface(match);
    if (!mux)
    {
        libusb_free_device_list(devices, 1);
        return tl::unexpected(mux.error());
    }
    if (!*mux)
    {
        libusb_free_device_list(devices, 1);
        return tl::unexpected(Error{ErrorCode::Transport, "the device has no mux USB interface"});
    }
    transport.impl_->interface_number = (*mux)->number;
    transport.impl_->config_value = (*mux)->config_value;
    transport.impl_->endpoint_in = (*mux)->endpoint_in;
    transport.impl_->endpoint_out = (*mux)->endpoint_out;

    libusb_device_descriptor descriptor{};
    if (libusb_get_device_descriptor(match, &descriptor) == 0)
    {
        transport.impl_->serial = device_serial(match, descriptor);
    }

    rc = libusb_open(match, &transport.impl_->handle);
    if (rc != 0)
    {
        libusb_free_device_list(devices, 1);
        return tl::unexpected(fail("libusb_open", rc));
    }

    // A device in its initial USB mode sits in a configuration without the mux
    // interface, so the configuration that carries it has to be selected before
    // the interface can be claimed. This is how `usbmuxd` reaches the device.
    int current_config = 0;
    const bool needs_config = libusb_get_configuration(transport.impl_->handle, &current_config) != 0 ||
                              current_config != transport.impl_->config_value;
    if (needs_config)
    {
#if defined(__linux__)
        // Changing configuration fails while a kernel driver holds an interface
        // of the target configuration, so detach each one first.
        libusb_config_descriptor *config = nullptr;
        if (libusb_get_config_descriptor_by_value(match, static_cast<std::uint8_t>(transport.impl_->config_value),
                                                  &config) == 0)
        {
            for (std::uint8_t i = 0; i < config->bNumInterfaces; ++i)
            {
                const int number = config->interface[i].altsetting[0].bInterfaceNumber;
                if (libusb_kernel_driver_active(transport.impl_->handle, number) == 1)
                {
                    libusb_detach_kernel_driver(transport.impl_->handle, number);
                }
            }
            libusb_free_config_descriptor(config);
        }
#endif
        rc = libusb_set_configuration(transport.impl_->handle, transport.impl_->config_value);
        if (rc != 0)
        {
            libusb_free_device_list(devices, 1);
            return tl::unexpected(fail("libusb_set_configuration", rc));
        }
    }
    libusb_free_device_list(devices, 1);

#if defined(__linux__)
    libusb_set_auto_detach_kernel_driver(transport.impl_->handle, 1);
#endif

    // The mux interface must be claimed before any bulk transfer, and released
    // again on close, so `usbmuxd` and this library do not use it at once.
    rc = libusb_claim_interface(transport.impl_->handle, transport.impl_->interface_number);
    if (rc != 0)
    {
        return tl::unexpected(fail("libusb_claim_interface", rc));
    }
    transport.impl_->claimed = true;

    // A failed transfer can leave a bulk endpoint halted, which makes every later
    // transfer on it fail. Clearing the halt on open recovers from that state.
    libusb_clear_halt(transport.impl_->handle, transport.impl_->endpoint_in);
    libusb_clear_halt(transport.impl_->handle, transport.impl_->endpoint_out);

    return transport;
}

UsbTransport::~UsbTransport() = default;

void UsbTransport::set_transfer_timeout(unsigned int milliseconds) noexcept
{
    impl_->transfer_timeout_ms = milliseconds;
}

unsigned int UsbTransport::transfer_timeout() const noexcept
{
    return impl_->transfer_timeout_ms;
}

void UsbTransport::set_transfer_budget(unsigned int milliseconds) noexcept
{
    impl_->transfer_budget_ms = milliseconds;
}

unsigned int UsbTransport::transfer_budget() const noexcept
{
    return impl_->transfer_budget_ms;
}

Result<std::size_t> UsbTransport::read(std::span<std::byte> buffer)
{
    // One bulk transfer is read at a time, but the caller may ask for fewer
    // bytes than the transfer carries, so the remainder is buffered and handed
    // out by later reads. The session relies on this to read a header and then a
    // payload from two separate transfers.
    if (impl_->incoming_offset >= impl_->incoming.size())
    {
        impl_->incoming.resize(kReadBufferSize);
        int transferred = 0;
        const int rc =
            impl_->bulk_transfer(impl_->endpoint_in, reinterpret_cast<unsigned char *>(impl_->incoming.data()),
                                 static_cast<int>(impl_->incoming.size()), &transferred);
        // A timeout that still delivered bytes is a short read, not a failure.
        if (rc != 0 && !(rc == LIBUSB_ERROR_TIMEOUT && transferred > 0))
        {
            return tl::unexpected(fail("libusb_bulk_transfer (read)", rc));
        }
        impl_->incoming.resize(static_cast<std::size_t>(transferred));
        impl_->incoming_offset = 0;
        if (transferred == 0)
        {
            return std::size_t{0};
        }
    }

    const std::size_t available = impl_->incoming.size() - impl_->incoming_offset;
    const std::size_t count = std::min(available, buffer.size());
    std::copy_n(impl_->incoming.begin() + static_cast<std::ptrdiff_t>(impl_->incoming_offset),
                static_cast<std::ptrdiff_t>(count), buffer.begin());
    impl_->incoming_offset += count;
    return count;
}

Status UsbTransport::write(std::span<const std::byte> data)
{
    if (data.empty())
    {
        return {};
    }

    int transferred = 0;
    const int rc = impl_->bulk_transfer(
        impl_->endpoint_out, const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(data.data())),
        static_cast<int>(data.size()), &transferred);
    if (rc != 0)
    {
        if (rc == LIBUSB_ERROR_TIMEOUT && transferred > 0)
        {
            return tl::unexpected(Error{ErrorCode::Transport, "short USB write: the stream is desynchronized"});
        }
        return tl::unexpected(fail("libusb_bulk_transfer (write)", rc));
    }
    if (static_cast<std::size_t>(transferred) != data.size())
    {
        return tl::unexpected(Error{ErrorCode::Transport, "short USB write"});
    }
    return {};
}

void UsbTransport::close()
{
    if (impl_->handle != nullptr)
    {
        if (impl_->claimed)
        {
            libusb_release_interface(impl_->handle, impl_->interface_number);
            impl_->claimed = false;
        }
        libusb_close(impl_->handle);
        impl_->handle = nullptr;
    }
}

std::string_view UsbTransport::serial() const noexcept
{
    return impl_->serial;
}

} // namespace ioscpp::usb
