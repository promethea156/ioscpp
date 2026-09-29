#include "ioscpp/usb/windows_driver.hpp"

#if defined(_WIN32)

#    ifndef NOMINMAX
#        define NOMINMAX
#    endif

#    include <windows.h>

// `initguid.h` defines the storage of the `DEFINE_GUID` device interface
// class, which `usbiodef.h` otherwise only declares; it is included before
// `usbiodef.h` so that the storage exists.
#    include <cfgmgr32.h>
#    include <devguid.h>
#    include <initguid.h>
#    include <newdev.h>
#    include <setupapi.h>
#    include <usbiodef.h>
#    include <wincrypt.h>

#    include <algorithm>
#    include <array>
#    include <cstddef>
#    include <cstdint>
#    include <filesystem>
#    include <functional>
#    include <optional>
#    include <string>
#    include <string_view>
#    include <system_error>
#    include <vector>

#    include "ioscpp/log.hpp"

namespace ioscpp::usb
{
namespace
{

Error fail(std::string_view what, unsigned long code)
{
    return Error{ErrorCode::Io, std::string(what) + ": " + std::system_category().message(static_cast<int>(code))};
}

std::wstring widen(std::string_view text)
{
    if (text.empty())
    {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

std::string narrow(std::wstring_view text)
{
    if (text.empty())
    {
        return {};
    }
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(), size, nullptr, nullptr);
    return narrow;
}

// `CreateProcessW` splits a command line on spaces, so a path that may contain
// one, such as an SDK tool under `Program Files (x86)`, is quoted.
std::wstring quote(const std::filesystem::path &path)
{
    return L"\"" + path.wstring() + L"\"";
}

std::string after_colon(const std::string &line)
{
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos)
    {
        return {};
    }
    std::size_t start = colon + 1;
    while (start < line.size() && line[start] == ' ')
    {
        ++start;
    }
    std::size_t end = line.size();
    while (end > start && (line[end - 1] == '\r' || line[end - 1] == ' '))
    {
        --end;
    }
    return line.substr(start, end - start);
}

struct RunResult
{
    int code = 0;
    std::string output;
};

// Runs `command`, logging its output, and returns its exit code and output.
// `CreateProcessW` takes a writable buffer, so the command line is copied.
Result<RunResult> run(std::wstring command)
{
    SECURITY_ATTRIBUTES attributes = {};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE read = nullptr;
    HANDLE write = nullptr;
    if (CreatePipe(&read, &write, &attributes, 0) == 0)
    {
        return tl::unexpected(fail("CreatePipe", GetLastError()));
    }
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write;
    startup.hStdError = write;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process = {};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process) == 0)
    {
        const unsigned long code = GetLastError();
        CloseHandle(read);
        CloseHandle(write);
        return tl::unexpected(fail("CreateProcess", code));
    }
    CloseHandle(write);

    RunResult result;
    std::array<char, 4096> buffer{};
    DWORD count = 0;
    while (ReadFile(read, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) != 0 && count > 0)
    {
        result.output.append(buffer.data(), count);
    }
    CloseHandle(read);

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    result.code = static_cast<int>(code);

    if (is_logging(LogLevel::Info))
    {
        std::size_t start = 0;
        while (start < result.output.size())
        {
            const std::size_t end = result.output.find('\n', start);
            const std::size_t length = (end == std::string::npos ? result.output.size() : end) - start;
            if (length > 0)
            {
                log(LogLevel::Info, result.output.substr(start, length));
            }
            start = (end == std::string::npos ? result.output.size() : end + 1);
        }
    }
    return result;
}

std::wstring host_arch()
{
    const std::wstring architecture = []
    {
        std::array<wchar_t, 32> value{};
        const DWORD length =
            GetEnvironmentVariableW(L"PROCESSOR_ARCHITECTURE", value.data(), static_cast<DWORD>(value.size()));
        return std::wstring(value.data(), length);
    }();
    if (architecture == L"AMD64")
    {
        return L"x64";
    }
    if (architecture == L"ARM64")
    {
        return L"arm64";
    }
    return L"x86";
}

// Finds a tool on `PATH`, then in the newest Windows Kits install, which is where
// `makecat` and `signtool` ship and where they are usually not on `PATH`.
std::optional<std::filesystem::path> find_tool(std::wstring name)
{
    std::array<wchar_t, MAX_PATH> on_path{};
    const DWORD length =
        SearchPathW(nullptr, name.c_str(), nullptr, static_cast<DWORD>(on_path.size()), on_path.data(), nullptr);
    if (length > 0 && length < on_path.size())
    {
        return std::filesystem::path(on_path.data());
    }

    std::array<wchar_t, MAX_PATH> program_files = {};
    if (GetEnvironmentVariableW(L"ProgramFiles(x86)", program_files.data(), static_cast<DWORD>(program_files.size())) ==
        0)
    {
        return std::nullopt;
    }
    const std::filesystem::path kits = std::filesystem::path(program_files.data()) / L"Windows Kits" / L"10" / L"bin";
    std::error_code error;
    if (!std::filesystem::exists(kits, error))
    {
        return std::nullopt;
    }

    std::vector<std::filesystem::path> versions;
    for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(kits, error))
    {
        if (entry.is_directory())
        {
            versions.push_back(entry.path());
        }
    }
    std::sort(versions.begin(), versions.end(), std::greater<>{});
    for (const std::filesystem::path &version : versions)
    {
        for (const std::wstring &sub : {host_arch(), std::wstring(L"x86")})
        {
            const std::filesystem::path candidate = version / sub / name;
            if (std::filesystem::exists(candidate, error))
            {
                return candidate;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::wstring> read_property(HDEVINFO set, SP_DEVINFO_DATA &data, DWORD key)
{
    DWORD type = 0;
    DWORD size = 0;
    SetupDiGetDeviceRegistryPropertyW(set, &data, key, &type, nullptr, 0, &size);
    if (size == 0)
    {
        return std::nullopt;
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (SetupDiGetDeviceRegistryPropertyW(set, &data, key, &type, reinterpret_cast<PBYTE>(value.data()), size,
                                          nullptr) == 0)
    {
        return std::nullopt;
    }
    // A `REG_SZ` or `REG_MULTI_SZ` value is NUL-terminated, so the tail is
    // dropped; a multi-string's inner NULs stay and are split by the caller.
    std::size_t length = value.size();
    while (length > 0 && value[length - 1] == L'\0')
    {
        --length;
    }
    value.resize(length);
    return value;
}

// Reads a `REG_MULTI_SZ` property, whose strings are NUL-separated.
std::vector<std::string> read_multi_string(HDEVINFO set, SP_DEVINFO_DATA &data, DWORD key)
{
    std::vector<std::string> strings;
    const std::optional<std::wstring> value = read_property(set, data, key);
    if (!value)
    {
        return strings;
    }
    std::size_t start = 0;
    while (start <= value->size())
    {
        std::size_t end = value->find(L'\0', start);
        if (end == std::wstring::npos)
        {
            end = value->size();
        }
        if (end > start)
        {
            strings.push_back(narrow(std::wstring_view(*value).substr(start, end - start)));
        }
        start = end + 1;
    }
    return strings;
}

bool is_apple(const std::string &hardware_id)
{
    return hardware_id.starts_with("USB\\VID_05AC&PID_");
}

bool is_interface(const std::string &hardware_id)
{
    return hardware_id.ends_with("&MI_01");
}

// A device in configuration 1 carries only the PTP interface, so the mux
// interface (`&MI_01`) exists only once a later configuration is selected, and
// the composite node is the target until then.
void add_target(HDEVINFO set, SP_DEVINFO_DATA &data, std::vector<DriverTarget> &targets)
{
    std::vector<wchar_t> instance(MAX_DEVICE_ID_LEN);
    if (SetupDiGetDeviceInstanceIdW(set, &data, instance.data(), static_cast<DWORD>(instance.size()), nullptr) == 0)
    {
        return;
    }

    const std::vector<std::string> ids = read_multi_string(set, data, SPDRP_HARDWAREID);
    const auto id = std::find_if(ids.begin(), ids.end(),
                                 [](const std::string &candidate)
                                 {
                                     return is_apple(candidate) && is_interface(candidate);
                                 });
    const auto fallback = std::find_if(ids.begin(), ids.end(), is_apple);

    DriverTarget target;
    if (id != ids.end())
    {
        target.hardware_id = *id;
    }
    else if (fallback != ids.end())
    {
        target.hardware_id = *fallback;
    }
    else
    {
        return;
    }
    target.instance_id = narrow(instance.data());
    if (const std::optional<std::wstring> service = read_property(set, data, SPDRP_SERVICE))
    {
        target.service = narrow(*service);
    }
    targets.push_back(std::move(target));
}

std::wstring hardware_id_of(const DriverTarget &target)
{
    // The composite node's first hardware id carries a revision, which would scope
    // the INF to one revision; the revision-free id covers every one.
    const std::string &id = target.hardware_id;
    const std::size_t revision = id.find("&REV_");
    return widen(revision == std::string::npos ? id : id.substr(0, revision));
}

std::string inf_text(const DriverTarget &target, const DriverOptions &options, const std::string &catalog)
{
    std::string text = R"inf(; Generated by ioscpp.
[Version]
Signature   = "$Windows NT$"
Class       = USBDevice
ClassGuid   = {88BAE032-5A81-49F0-BC3D-A4FF138216D6}
Provider    = %ManufacturerName%
CatalogFile = @CATALOG@
DriverVer   = @DATE@,1.0.0.0

[Manufacturer]
%ManufacturerName% = DeviceList, @DECORATION@

[DeviceList.@DECORATION@]
%DeviceName% = USB_Install, USB\@DEVICEID@

[DestinationDirs]
libusb_files_sys = 10,system32\drivers
libusb_files_dll = 10,system32

[SourceDisksNames.@ARCH@]
1 = %DiskName%

[SourceDisksFiles.@ARCH@]
libusb0.sys = 1,@ARCH@
libusb0.dll = 1,@ARCH@

[USB_Install]
CopyFiles = libusb_files_sys, libusb_files_dll

[USB_Install.Services]
AddService = libusb0, 0x00000002, libusb_add_service

[USB_Install.HW]
AddReg = libusb_add_reg_hw

[libusb_files_sys]
libusb0.sys

[libusb_files_dll]
libusb0.dll

[libusb_add_service]
DisplayName    = %ServiceDisplayName%
ServiceType    = 1
StartType      = 3
ErrorControl   = 0
ServiceBinary  = %12%\libusb0.sys

[libusb_add_reg_hw]
HKR,,SurpriseRemovalOK,0x00010001,1
HKR,,InitialConfigValue,0x00010001,@CONFIG@

[Strings]
ManufacturerName   = "ioscpp"
DeviceName          = "Apple Mobile Device (libusb-win32)"
DiskName            = "ioscpp libusb-win32"
ServiceDisplayName  = "libusb-win32"
)inf";

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    char date[16] = {};
    std::snprintf(date, sizeof(date), "%02u/%02u/%04u", now.wMonth, now.wDay, now.wYear);

    std::string device_id = narrow(hardware_id_of(target));
    constexpr std::string_view prefix = "USB\\";
    if (device_id.starts_with(prefix))
    {
        device_id.erase(0, prefix.size());
    }

    const auto replace_all = [&text](std::string_view from, const std::string &to)
    {
        std::size_t position = 0;
        while ((position = text.find(from, position)) != std::string::npos)
        {
            text.replace(position, from.size(), to);
            position += to.size();
        }
    };
    replace_all("@CATALOG@", catalog);
    replace_all("@DATE@", date);
    replace_all("@DECORATION@", host_arch() == L"arm64" ? "NTARM64" : (host_arch() == L"x64" ? "NTamd64" : "NTx86"));
    replace_all("@ARCH@", narrow(host_arch()));
    replace_all("@DEVICEID@", device_id);
    replace_all("@CONFIG@", std::to_string(options.config_value));
    return text;
}

Status write_file(const std::filesystem::path &path, std::string_view text)
{
    std::FILE *file = nullptr;
    if (_wfopen_s(&file, path.wstring().c_str(), L"wb") != 0 || file == nullptr)
    {
        return tl::unexpected(Error{ErrorCode::Io, "could not write " + path.string()});
    }
    std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
    return {};
}

// Creates a self-signed code-signing certificate in the current user's personal
// store and trusts it on the machine, which is what Zadig does. `signtool` then
// signs by the certificate's name, so no key file is written.
Status ensure_certificate()
{
    // The value has spaces, so it is quoted in the X500 string, which
    // `CertStrToName` otherwise reads as a delimiter.
    const std::wstring name = L"CN=\"ioscpp driver signing\"";
    const std::wstring container = L"ioscpp-driver-signing";

    // `pSubjectIssuerBlob` is an encoded name, not the text, so it is encoded.
    DWORD subject_size = 0;
    CertStrToNameW(X509_ASN_ENCODING, name.c_str(), CERT_X500_NAME_STR, nullptr, nullptr, &subject_size, nullptr);
    std::vector<BYTE> subject_encoded(subject_size);
    if (CertStrToNameW(X509_ASN_ENCODING, name.c_str(), CERT_X500_NAME_STR, nullptr, subject_encoded.data(),
                       &subject_size, nullptr) == 0)
    {
        return tl::unexpected(fail("CertStrToName", GetLastError()));
    }
    CERT_NAME_BLOB subject = {};
    subject.cbData = subject_size;
    subject.pbData = subject_encoded.data();

    CRYPT_KEY_PROV_INFO key = {};
    key.pwszContainerName = const_cast<LPWSTR>(container.c_str());
    key.dwProvType = PROV_RSA_AES;
    key.dwKeySpec = AT_SIGNATURE;

    CRYPT_ALGORITHM_IDENTIFIER algorithm = {};
    algorithm.pszObjId = const_cast<char *>(szOID_RSA_SHA256RSA);

    LPSTR code_signing = const_cast<LPSTR>(szOID_PKIX_KP_CODE_SIGNING);
    CERT_ENHKEY_USAGE usage = {};
    usage.cUsageIdentifier = 1;
    usage.rgpszUsageIdentifier = &code_signing;

    // `pExtensions` carries each extension's encoded value, so the code-signing
    // usage is encoded rather than passed as the raw structure.
    DWORD value_size = 0;
    CryptEncodeObject(X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE, &usage, nullptr, &value_size);
    std::vector<BYTE> value(value_size);
    CryptEncodeObject(X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE, &usage, value.data(), &value_size);

    CERT_EXTENSION extension = {};
    extension.pszObjId = const_cast<char *>(szOID_ENHANCED_KEY_USAGE);
    extension.Value.cbData = value_size;
    extension.Value.pbData = value.data();
    CERT_EXTENSIONS extensions = {};
    extensions.cExtension = 1;
    extensions.rgExtension = &extension;

    PCCERT_CONTEXT certificate =
        CertCreateSelfSignCertificate(0, &subject, 0, &key, &algorithm, nullptr, nullptr, nullptr);
    if (certificate == nullptr)
    {
        return tl::unexpected(fail("CertCreateSelfSignCertificate", GetLastError()));
    }

    const auto add = [&](DWORD location, LPCWSTR store_name)
    {
        HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, location, store_name);
        if (store == nullptr)
        {
            return false;
        }
        const BOOL added =
            CertAddCertificateContextToStore(store, certificate, CERT_STORE_ADD_REPLACE_EXISTING, nullptr);
        CertCloseStore(store, 0);
        return added != 0;
    };

    const bool added = add(CERT_SYSTEM_STORE_CURRENT_USER, L"MY") && add(CERT_SYSTEM_STORE_LOCAL_MACHINE, L"ROOT") &&
                       add(CERT_SYSTEM_STORE_LOCAL_MACHINE, L"TrustedPublisher");
    CertFreeCertificateContext(certificate);
    if (!added)
    {
        return tl::unexpected(Error{ErrorCode::Io, "the signing certificate could not be trusted"});
    }
    return {};
}

Status sign_package(const std::filesystem::path &directory, const std::string &catalog)
{
    const std::optional<std::filesystem::path> signtool = find_tool(L"signtool.exe");
    if (!signtool)
    {
        log(LogLevel::Warning, "signtool was not found; the package is unsigned");
        return {};
    }
    const std::optional<std::filesystem::path> makecat = find_tool(L"makecat.exe");
    if (!makecat)
    {
        log(LogLevel::Warning, "makecat was not found; the package is unsigned");
        return {};
    }

    // `makecat` needs a catalog definition file and resolves its members relative
    // to the current directory, which is why it is run from the package.
    const std::string cdf = "[CatalogHeader]\nName=" + catalog +
                            "\nPublicVersion=0x00000001\nEncodingType=0x00010001\n"
                            "CATATTR1=0x10010001:OSAttr:2:10.0\n\n[CatalogFiles]\n"
                            "<hash>libusb0.inf=libusb0.inf\n<hash>libusb0.sys=" +
                            narrow(host_arch()) + "\\libusb0.sys\n<hash>libusb0.dll=" + narrow(host_arch()) +
                            "\\libusb0.dll\n";
    if (Status written = write_file(directory / L"libusb0.cdf", cdf); !written)
    {
        return written;
    }

    const std::filesystem::path previous = std::filesystem::current_path();
    std::filesystem::current_path(directory);
    Result<RunResult> made = run(quote(*makecat) + L" libusb0.cdf");
    std::filesystem::current_path(previous);
    if (!made || made->code != 0)
    {
        return tl::unexpected(Error{ErrorCode::Io, "makecat failed"});
    }

    if (Status trusted = ensure_certificate(); !trusted)
    {
        return trusted;
    }

    const std::wstring line =
        quote(*signtool) + L" sign /fd sha256 /s My /n \"ioscpp driver signing\" /a " + quote(directory / catalog);
    Result<RunResult> signed_code = run(line);
    if (!signed_code || signed_code->code != 0)
    {
        return tl::unexpected(Error{ErrorCode::Io, "signtool failed"});
    }
    return {};
}

} // namespace

bool DriverTarget::is_interface() const
{
    return hardware_id.ends_with("&MI_01");
}

Result<std::vector<DriverTarget>> driver_targets()
{
    HDEVINFO set =
        SetupDiGetClassDevsW(&GUID_DEVINTERFACE_USB_DEVICE, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
    {
        return tl::unexpected(fail("SetupDiGetClassDevs", GetLastError()));
    }

    std::vector<DriverTarget> interfaces;
    std::vector<DriverTarget> composites;
    for (DWORD index = 0;; ++index)
    {
        SP_DEVINFO_DATA data = {};
        data.cbSize = sizeof(data);
        if (SetupDiEnumDeviceInfo(set, index, &data) == 0)
        {
            if (GetLastError() == ERROR_NO_MORE_ITEMS)
            {
                break;
            }
            continue;
        }
        std::vector<DriverTarget> candidates;
        add_target(set, data, candidates);
        if (candidates.empty())
        {
            continue;
        }
        if (candidates.front().is_interface())
        {
            interfaces.push_back(std::move(candidates.front()));
        }
        else
        {
            composites.push_back(std::move(candidates.front()));
        }
    }
    SetupDiDestroyDeviceInfoList(set);

    // The mux interface is preferred, because binding it leaves the rest of the
    // device on Apple's driver; a fresh device has only the composite node.
    interfaces.insert(interfaces.end(), std::make_move_iterator(composites.begin()),
                      std::make_move_iterator(composites.end()));
    return interfaces;
}

Status install_driver(const DriverTarget &target, const DriverOptions &options)
{
    if (!options.force && target.service == "libusb0")
    {
        log(LogLevel::Info, target.instance_id + " is already on libusb-win32");
        return {};
    }

    // The binaries come from the package, or from the copy a previous install
    // left under `System32`; the INF names them under an arch subdirectory.
    const std::wstring arch = host_arch();
    std::vector<std::filesystem::path> sources;
    std::error_code error;
    if (options.package.empty())
    {
        std::array<wchar_t, MAX_PATH> root{};
        if (GetEnvironmentVariableW(L"SystemRoot", root.data(), static_cast<DWORD>(root.size())) > 0)
        {
            const std::filesystem::path system = std::filesystem::path(root.data()) / L"System32";
            sources.push_back(system);
            sources.push_back(system / L"drivers");
        }
    }
    else
    {
        const std::filesystem::path base = std::filesystem::path(widen(options.package)) / L"bin" / arch;
        sources.push_back(std::filesystem::exists(base, error) ? base : std::filesystem::path(widen(options.package)));
    }

    // The instance id carries backslashes, so they are replaced for the path.
    std::string name = target.instance_id;
    std::replace(name.begin(), name.end(), '\\', '_');
    std::replace(name.begin(), name.end(), '&', '_');
    const std::filesystem::path work = std::filesystem::temp_directory_path() / ("ioscpp-driver-" + name);
    std::filesystem::remove_all(work, error);
    std::filesystem::create_directories(work / arch, error);

    for (const std::wstring &binary : {std::wstring(L"libusb0.sys"), std::wstring(L"libusb0.dll")})
    {
        for (const std::filesystem::path &directory : sources)
        {
            if (std::filesystem::exists(directory / binary, error))
            {
                std::filesystem::copy_file(directory / binary, work / arch / binary,
                                           std::filesystem::copy_options::overwrite_existing, error);
                break;
            }
        }
        if (!std::filesystem::exists(work / arch / binary, error))
        {
            return tl::unexpected(Error{ErrorCode::Io, "the package is missing " + narrow(binary)});
        }
    }

    const std::string catalog = "ioscpp-libusb0.cat";
    const std::string text = inf_text(target, options, catalog);
    if (Status written = write_file(work / L"libusb0.inf", text); !written)
    {
        return written;
    }

    if (options.sign)
    {
        if (Status signed_code = sign_package(work, catalog); !signed_code)
        {
            log(LogLevel::Warning,
                "the package could not be signed (" + signed_code.error().message + "); it is installed unsigned");
        }
    }

    // `DiInstallDriver` installs the package and binds it to the matching node.
    // It needs elevation, and reports the reason when it does not have it.
    BOOL reboot = FALSE;
    const std::filesystem::path inf = work / L"libusb0.inf";
    if (DiInstallDriverW(nullptr, inf.wstring().c_str(), 0, &reboot) == 0)
    {
        return tl::unexpected(fail("DiInstallDriver", GetLastError()));
    }
    log(LogLevel::Info, "installed the driver for " + target.hardware_id);
    return {};
}

Status uninstall_driver()
{
    const std::optional<std::filesystem::path> pnputil = find_tool(L"pnputil.exe");
    if (!pnputil)
    {
        return tl::unexpected(Error{ErrorCode::Io, "pnputil was not found"});
    }

    Result<RunResult> listed = run(quote(*pnputil) + L" /enum-drivers");
    if (!listed || listed->code != 0)
    {
        return tl::unexpected(Error{ErrorCode::Io, "pnputil /enum-drivers failed"});
    }

    // `pnputil` prints a package's published name before its original name, so
    // a published name is kept until the original name is read.
    std::vector<std::string> targets;
    std::string published;
    std::string original;
    std::size_t start = 0;
    while (start < listed->output.size())
    {
        const std::size_t end = listed->output.find('\n', start);
        const std::string line =
            listed->output.substr(start, (end == std::string::npos ? listed->output.size() : end) - start);
        if (line.find("Published Name") != std::string::npos)
        {
            if (original == "libusb0.inf" && !published.empty())
            {
                targets.push_back(published);
            }
            published = after_colon(line);
            original.clear();
        }
        else if (line.find("Original Name") != std::string::npos)
        {
            original = after_colon(line);
        }
        start = (end == std::string::npos ? listed->output.size() : end + 1);
    }
    if (original == "libusb0.inf" && !published.empty())
    {
        targets.push_back(published);
    }

    if (targets.empty())
    {
        log(LogLevel::Info, "no libusb-win32 package is in the driver store");
        return {};
    }
    for (const std::string &target : targets)
    {
        Result<RunResult> deleted = run(quote(*pnputil) + L" /delete-driver " + widen(target) + L" /uninstall /force");
        if (!deleted || deleted->code != 0)
        {
            return tl::unexpected(Error{ErrorCode::Io, "pnputil /delete-driver failed for " + target});
        }
    }
    return {};
}

} // namespace ioscpp::usb

#endif
