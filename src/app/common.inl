
constexpr uint16_t kUsbipVersion = 0x0111;
constexpr uint16_t kOpReqDevlist = 0x8005;
constexpr uint16_t kOpRepDevlist = 0x0005;
constexpr uint16_t kOpReqImport = 0x8003;
constexpr uint16_t kOpRepImport = 0x0003;
constexpr uint32_t kCmdSubmit = 0x00000001;
constexpr uint32_t kCmdUnlink = 0x00000002;
constexpr uint32_t kRetSubmit = 0x00000003;
constexpr uint32_t kRetUnlink = 0x00000004;
constexpr uint32_t kUsbipDirOut = 0;
constexpr uint32_t kUsbipDirIn = 1;
constexpr uint32_t kNonIsoPackets = 0xffffffffu;
constexpr int32_t kStatusOk = 0;
constexpr int32_t kStatusStall = -32; // -EPIPE
constexpr int32_t kStatusConnReset = -104; // -ECONNRESET
constexpr uint16_t kDefaultUsbipPort = 3240;
constexpr uint32_t kUsbSpeedHigh = 3; // Linux USB_SPEED_HIGH for USB/IP.
constexpr uint32_t kUsbSpeedFull = 2; // Linux USB_SPEED_FULL (composite puck: 64B bulk eps).
constexpr const wchar_t *kDefaultBusId = L"ctm-ds5";

std::atomic_bool g_stop(false);

// Set when this process was launched as a Windows service (service-run), and
// when a hard restart has been requested over the control channel. Read by the
// service wrapper (app/service.inl) to decide whether to report a failure exit
// so the SCM restart recovery action fires.
std::atomic_bool g_running_as_service(false);
std::atomic_bool g_restart_requested(false);

// Defined in app/service.inl. Requests a full SCM-driven restart of this
// process when running as a service; returns false in interactive agent mode,
// where a hard restart is a no-op. Forward declared here so agent.inl's control
// handler can call it before service.inl is included.
static bool request_service_restart();

// Runtime selection of the additive ENet/UDP transport. Off by default so the
// process behaves byte-identically to the TCP-only build; set true only when
// the --enet flag is passed. Read by the agent session worker and bridge mode
// to choose EnetBridgeBackend over the TCP BridgeBackend.
std::atomic_bool g_use_enet(false);

// RAII guard for ENet's process-global init/deinit. enet_initialize() is called
// once at startup and enet_deinitialize() once at exit, regardless of which
// transport is actually selected (cheap, and keeps the lifetime trivially
// correct across every wmain exit path including the early agent return).
struct EnetGlobalGuard {
    bool ok = false;
    EnetGlobalGuard()
    {
        ok = enet_initialize() == 0;
    }
    ~EnetGlobalGuard()
    {
        if (ok) {
            enet_deinitialize();
        }
    }
    EnetGlobalGuard(const EnetGlobalGuard &) = delete;
    EnetGlobalGuard &operator=(const EnetGlobalGuard &) = delete;
};

static bool ctm_verbose_logs()
{
    static const bool enabled = []() {
        wchar_t value[16] = {};
        const DWORD len = GetEnvironmentVariableW(L"CTM_USBIP_VERBOSE", value, 16);
        return len != 0 && value[0] != L'0';
    }();
    return enabled;
}

static BOOL WINAPI console_ctrl_handler(DWORD ctrlType)
{
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT ||
        ctrlType == CTRL_CLOSE_EVENT || ctrlType == CTRL_SHUTDOWN_EVENT) {
        g_stop.store(true);
        return TRUE;
    }
    return FALSE;
}

static std::wstring last_error_message(const wchar_t *prefix)
{
    wchar_t *buffer = nullptr;
    DWORD err = GetLastError();
    FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        err,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);
    std::wstring message = prefix;
    message += L": ";
    if (buffer) {
        message += buffer;
        LocalFree(buffer);
    } else {
        message += L"unknown error";
    }
    return message;
}

static std::wstring wsa_error_message(const wchar_t *prefix)
{
    wchar_t *buffer = nullptr;
    DWORD err = WSAGetLastError();
    FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        err,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);
    std::wstring message = prefix;
    message += L": ";
    if (buffer) {
        message += buffer;
        LocalFree(buffer);
    } else {
        message += L"unknown winsock error";
    }
    return message;
}

static bool file_exists(const std::wstring &path)
{
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static std::wstring module_directory()
{
    wchar_t path[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (len == 0 || len >= ARRAYSIZE(path)) {
        return L".";
    }
    std::wstring value = path;
    size_t slash = value.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : value.substr(0, slash);
}

static std::wstring find_ds5_descriptor_profile()
{
    const std::wstring exeDir = module_directory();
    const std::vector<std::wstring> candidates = {
        L"profiles\\descriptors\\ds5_composite.profile",
        L"..\\profiles\\descriptors\\ds5_composite.profile",
        exeDir + L"\\profiles\\descriptors\\ds5_composite.profile",
        exeDir + L"\\..\\..\\..\\profiles\\descriptors\\ds5_composite.profile"
    };
    for (const std::wstring &candidate : candidates) {
        if (file_exists(candidate)) {
            return candidate;
        }
    }
    return candidates[0];
}

static std::wstring find_ds5_map_file()
{
    const std::wstring exeDir = module_directory();
    const std::vector<std::wstring> candidates = {
        L"maps\\ds5_usb_over_ds5_bt.map",
        L"..\\maps\\ds5_usb_over_ds5_bt.map",
        exeDir + L"\\maps\\ds5_usb_over_ds5_bt.map",
        exeDir + L"\\..\\..\\..\\maps\\ds5_usb_over_ds5_bt.map"
    };
    for (const std::wstring &candidate : candidates) {
        if (file_exists(candidate)) {
            return candidate;
        }
    }
    return candidates[0];
}

static std::wstring find_hid_identity_map_file()
{
    const std::wstring exeDir = module_directory();
    const std::vector<std::wstring> candidates = {
        L"maps\\hid_identity.map",
        L"..\\maps\\hid_identity.map",
        exeDir + L"\\maps\\hid_identity.map",
        exeDir + L"\\..\\..\\..\\maps\\hid_identity.map"
    };
    for (const std::wstring &candidate : candidates) {
        if (file_exists(candidate)) {
            return candidate;
        }
    }
    return candidates[0];
}

static std::string narrow_ascii(const std::wstring &text)
{
    std::string out;
    for (wchar_t ch : text) {
        if (ch <= 0 || ch > 0x7f) {
            return {};
        }
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

static wchar_t uppercase_ascii(wchar_t ch)
{
    return (ch >= L'a' && ch <= L'z') ? static_cast<wchar_t>(ch - L'a' + L'A') : ch;
}

static bool is_ascii_alnum(wchar_t ch)
{
    ch = uppercase_ascii(ch);
    return (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9');
}

static bool is_ascii_hex(wchar_t ch)
{
    ch = uppercase_ascii(ch);
    return (ch >= L'A' && ch <= L'F') || (ch >= L'0' && ch <= L'9');
}

static std::wstring uppercase_ascii_copy(std::wstring text)
{
    for (wchar_t &ch : text) {
        ch = uppercase_ascii(ch);
    }
    return text;
}

static std::wstring sanitize_usb_serial(const std::wstring &text)
{
    std::wstring out;
    for (wchar_t ch : text) {
        if (is_ascii_alnum(ch)) {
            out.push_back(uppercase_ascii(ch));
            if (out.size() >= 64) {
                break;
            }
        }
    }
    return out;
}

static std::wstring extract_bt_dev_identifier(const std::wstring &text)
{
    const std::wstring upper = uppercase_ascii_copy(text);
    size_t pos = upper.find(L"DEV_");
    while (pos != std::wstring::npos) {
        std::wstring value;
        for (size_t i = pos + 4; i < upper.size() && value.size() < 12; ++i) {
            if (is_ascii_hex(upper[i])) {
                value.push_back(upper[i]);
            } else if (!value.empty()) {
                break;
            }
        }
        if (value.size() == 12) {
            return value;
        }
        pos = upper.find(L"DEV_", pos + 1);
    }
    return L"";
}

static std::wstring virtual_usb_serial_from_bt_device(const CtmBtDevice &device)
{
    // Prefer the BTHENUM\Dev_XXXXXXXXXXXX MAC over the HID firmware serial.
    // The MAC is the authoritative hardware identifier exposed by the Bluetooth
    // stack; the HID-reported serial is firmware-stamped and varies per
    // controller (e.g. DualShock 4 returns a 12-digit decimal unit number,
    // not the BT MAC). Falling back through the device tree first lets every
    // controller use the same MAC-based key.
    std::wstring value = extract_bt_dev_identifier(device.parent_instance_id);
    if (!value.empty()) return value;
    value = extract_bt_dev_identifier(device.instance_id);
    if (!value.empty()) return value;
    value = extract_bt_dev_identifier(device.path);
    if (!value.empty()) return value;
    value = sanitize_usb_serial(device.serial);
    if (!value.empty()) return value;
    value = sanitize_usb_serial(device.parent_instance_id);
    if (!value.empty()) return value;
    value = sanitize_usb_serial(device.instance_id);
    if (!value.empty()) return value;
    return sanitize_usb_serial(device.path);
}

static std::vector<unsigned char> make_usb_string_descriptor(const std::wstring &text)
{
    const size_t chars = (std::min)(text.size(), static_cast<size_t>(126));
    std::vector<unsigned char> descriptor(2 + chars * 2, 0);
    descriptor[0] = static_cast<unsigned char>(descriptor.size());
    descriptor[1] = 0x03;
    for (size_t i = 0; i < chars; ++i) {
        descriptor[2 + i * 2] = static_cast<unsigned char>(text[i] & 0xff);
        descriptor[3 + i * 2] = static_cast<unsigned char>((text[i] >> 8) & 0xff);
    }
    return descriptor;
}

static bool replace_usb_string_descriptor(
    std::vector<unsigned char> *descriptors,
    unsigned int targetIndex,
    const std::vector<unsigned char> &replacement)
{
    if (descriptors == nullptr || replacement.size() < 2 || replacement.size() > 255) {
        return false;
    }
    size_t offset = 0;
    unsigned int index = 0;
    while (offset + 2 <= descriptors->size()) {
        const unsigned int length = (*descriptors)[offset];
        if (length < 2 || offset + length > descriptors->size()) {
            return false;
        }
        if (index == targetIndex) {
            descriptors->erase(descriptors->begin() + offset, descriptors->begin() + offset + length);
            descriptors->insert(descriptors->begin() + offset, replacement.begin(), replacement.end());
            return true;
        }
        offset += length;
        ++index;
    }
    if (index == targetIndex) {
        descriptors->insert(descriptors->end(), replacement.begin(), replacement.end());
        return true;
    }
    return false;
}

static bool apply_virtual_serial_to_profile(
    CtmDescriptorProfile *profile,
    const std::wstring &requestedSerial,
    std::wstring *serialOut,
    std::wstring *error)
{
    if (profile == nullptr || profile->device_descriptor.size() < 17) {
        if (error) *error = L"profile has no writable iSerialNumber field";
        return false;
    }
    std::wstring serial = sanitize_usb_serial(requestedSerial);
    if (serial.empty()) {
        serial = L"CTMUSBIP";
    }
    const std::vector<unsigned char> descriptor = make_usb_string_descriptor(serial);
    if (!replace_usb_string_descriptor(&profile->string_descriptors, 3, descriptor)) {
        if (error) *error = L"could not replace USB serial descriptor index 3";
        return false;
    }
    profile->device_descriptor[16] = 3;
    if (serialOut) {
        *serialOut = serial;
    }
    return true;
}

static std::string hex_span(const uint8_t *data, size_t len)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        if (i) out << ' ';
        out << std::setw(2) << static_cast<unsigned int>(data[i]);
    }
    return out.str();
}

static std::string hex_vector_or_dash(const std::vector<uint8_t> &data)
{
    if (data.empty()) {
        return "-";
    }
    return hex_span(data.data(), data.size());
}

static std::wstring widen_ascii(const char *text, size_t len)
{
    std::wstring out;
    for (size_t i = 0; i < len && text[i] != '\0'; ++i) {
        out.push_back(static_cast<unsigned char>(text[i]));
    }
    return out;
}

static bool parse_uint_arg(const wchar_t *text, unsigned long maxValue, unsigned long *value)
{
    if (text == nullptr || *text == L'\0' || value == nullptr) {
        return false;
    }
    wchar_t *end = nullptr;
    unsigned long parsed = wcstoul(text, &end, 0);
    if (end == text || *end != L'\0' || parsed > maxValue) {
        return false;
    }
    *value = parsed;
    return true;
}

static void append_be16(std::vector<uint8_t> *out, uint16_t value)
{
    out->push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out->push_back(static_cast<uint8_t>(value & 0xff));
}

static void append_be32(std::vector<uint8_t> *out, uint32_t value)
{
    out->push_back(static_cast<uint8_t>((value >> 24) & 0xff));
    out->push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out->push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out->push_back(static_cast<uint8_t>(value & 0xff));
}

static uint16_t read_be16(const uint8_t *data)
{
    return static_cast<uint16_t>((data[0] << 8) | data[1]);
}

static uint32_t read_be32(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
        (static_cast<uint32_t>(data[1]) << 16) |
        (static_cast<uint32_t>(data[2]) << 8) |
        static_cast<uint32_t>(data[3]);
}

static uint16_t read_le16(const uint8_t *data)
{
    return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

static int16_t read_s16_le(const uint8_t *data)
{
    return static_cast<int16_t>(read_le16(data));
}

static void append_fixed_string(std::vector<uint8_t> *out, const char *text, size_t bytes)
{
    const size_t start = out->size();
    out->resize(start + bytes, 0);
    if (text != nullptr) {
        const size_t copy = (std::min)(strlen(text), bytes ? bytes - 1 : 0);
        if (copy != 0) {
            memcpy(out->data() + start, text, copy);
        }
    }
}

static bool recv_all(SOCKET s, uint8_t *data, size_t length)
{
    size_t got = 0;
    while (got < length) {
        const int chunk = static_cast<int>((std::min)(length - got, static_cast<size_t>(INT_MAX)));
        int rc = recv(s, reinterpret_cast<char *>(data + got), chunk, 0);
        if (rc <= 0) {
            return false;
        }
        got += static_cast<size_t>(rc);
    }
    return true;
}

static bool send_all(SOCKET s, const uint8_t *data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        const int chunk = static_cast<int>((std::min)(length - sent, static_cast<size_t>(INT_MAX)));
        int rc = send(s, reinterpret_cast<const char *>(data + sent), chunk, 0);
        if (rc <= 0) {
            return false;
        }
        sent += static_cast<size_t>(rc);
    }
    return true;
}

