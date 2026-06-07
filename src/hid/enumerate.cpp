#include "ctm/hid.h"

#include <windows.h>
#include <cfgmgr32.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>
#include <algorithm>
#include <cwctype>
#include <set>
#include <string>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

static const unsigned short CTM_SONY_VID = 0x054c;
static const unsigned short CTM_MICROSOFT_VID = 0x045e;

static std::wstring lower_copy(const std::wstring &text)
{
    std::wstring out = text;
    for (wchar_t &ch : out) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    return out;
}

static bool contains_case_insensitive(const std::wstring &text, const wchar_t *needle)
{
    return lower_copy(text).find(lower_copy(needle)) != std::wstring::npos;
}

static bool starts_with_case_insensitive(const std::wstring &text, const wchar_t *prefix)
{
    std::wstring lowerText = lower_copy(text);
    std::wstring lowerPrefix = lower_copy(prefix);
    return lowerText.rfind(lowerPrefix, 0) == 0;
}

static bool sony_ds5_pid(unsigned short pid)
{
    return pid == 0x0ce6 || pid == 0x0df2 || pid == 0x0e5f;
}

static bool sony_ds4_pid(unsigned short pid)
{
    return pid == 0x05c4 || pid == 0x09cc;
}

static bool sony_supported_controller_pid(unsigned short pid)
{
    return sony_ds5_pid(pid) || sony_ds4_pid(pid);
}

static bool instance_looks_bluetooth_sony_controller(const std::wstring &instance)
{
    return contains_case_insensitive(instance, L"VID&0002054C");
}

static bool instance_looks_bluetooth(const std::wstring &instance)
{
    return contains_case_insensitive(instance, L"BTHENUM") ||
           contains_case_insensitive(instance, L"BTHLE") ||
           contains_case_insensitive(instance, L"Bluetooth") ||
           instance_looks_bluetooth_sony_controller(instance);
}

static bool name_looks_controller(const std::wstring &name)
{
    return contains_case_insensitive(name, L"DualSense") ||
           contains_case_insensitive(name, L"DualShock") ||
           contains_case_insensitive(name, L"Wireless Controller") ||
           contains_case_insensitive(name, L"Xbox") ||
           contains_case_insensitive(name, L"Gamepad") ||
           contains_case_insensitive(name, L"Game Controller") ||
           contains_case_insensitive(name, L"Steam Controller");
}

static std::wstring get_device_instance_id(DEVINST devInst)
{
    wchar_t instanceId[512] = {};
    if (CM_Get_Device_IDW(devInst, instanceId, ARRAYSIZE(instanceId), 0) != CR_SUCCESS) {
        return L"";
    }
    return instanceId;
}

static bool ancestor_looks_bluetooth(DEVINST devInst, std::wstring *firstBluetoothAncestor)
{
    DEVINST current = devInst;
    for (int depth = 0; depth < 8; ++depth) {
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, current, 0) != CR_SUCCESS) {
            break;
        }

        std::wstring parentId = get_device_instance_id(parent);
        if (instance_looks_bluetooth(parentId)) {
            if (firstBluetoothAncestor != nullptr) {
                *firstBluetoothAncestor = parentId;
            }
            return true;
        }

        current = parent;
    }

    return false;
}

static HANDLE open_hid_path(const std::wstring &path, DWORD desiredAccess)
{
    return CreateFileW(
        path.c_str(),
        desiredAccess,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        nullptr);
}

static std::wstring query_hid_string(HANDLE handle, BOOLEAN (__stdcall *query)(HANDLE, PVOID, ULONG))
{
    wchar_t buffer[256] = {};
    if (!query(handle, buffer, sizeof(buffer)) || buffer[0] == L'\0') {
        return L"";
    }
    return buffer;
}

static void query_hid_caps(HANDLE handle, CtmBtDevice *device)
{
    PHIDP_PREPARSED_DATA preparsed = nullptr;
    HIDP_CAPS caps = {};

    if (!HidD_GetPreparsedData(handle, &preparsed)) {
        return;
    }

    if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS) {
        device->usage_page = caps.UsagePage;
        device->usage = caps.Usage;
        device->input_report_length = caps.InputReportByteLength;
        device->output_report_length = caps.OutputReportByteLength;
        device->feature_report_length = caps.FeatureReportByteLength;
    }

    HidD_FreePreparsedData(preparsed);
}

static void fill_hid_details(HANDLE handle, CtmBtDevice *device)
{
    HIDD_ATTRIBUTES attr = {};
    attr.Size = sizeof(attr);
    if (HidD_GetAttributes(handle, &attr)) {
        device->vendor_id = attr.VendorID;
        device->product_id = attr.ProductID;
        device->version_number = attr.VersionNumber;
    }

    device->product = query_hid_string(handle, HidD_GetProductString);
    device->manufacturer = query_hid_string(handle, HidD_GetManufacturerString);
    device->serial = query_hid_string(handle, HidD_GetSerialNumberString);
    query_hid_caps(handle, device);
}

static bool hid_usage_is_controller(const CtmBtDevice &device)
{
    return device.usage_page == 0x01 &&
           (device.usage == 0x04 || device.usage == 0x05 || device.usage == 0x08);
}

static bool known_controller_vid_pid(const CtmBtDevice &device)
{
    return device.vendor_id == CTM_SONY_VID && sony_supported_controller_pid(device.product_id);
}

static bool microsoft_xbox_controller(const CtmBtDevice &device)
{
    std::wstring name = device.product + L" " + device.manufacturer + L" " + device.instance_id;
    return device.vendor_id == CTM_MICROSOFT_VID &&
           (device.is_game_controller || contains_case_insensitive(name, L"Xbox"));
}

static void classify_device(CtmBtDevice *device)
{
    std::wstring name = device->product + L" " + device->manufacturer + L" " + device->instance_id;
    device->is_game_controller =
        hid_usage_is_controller(*device) ||
        known_controller_vid_pid(*device) ||
        name_looks_controller(name);

    if (device->vendor_id == CTM_SONY_VID && sony_ds5_pid(device->product_id)) {
        device->device_type = device->is_bluetooth ? L"ds5_bt" : L"ds5_usb";
        device->is_supported = device->is_bluetooth && device->can_open_read_write;
        if (!device->is_supported) {
            device->unavailable_reason = device->is_bluetooth ? L"open read/write failed" : L"not bluetooth";
        }
        return;
    }

    if (device->vendor_id == CTM_SONY_VID && sony_ds4_pid(device->product_id)) {
        device->device_type = device->is_bluetooth ? L"ds4_bt" : L"ds4_usb";
        device->is_supported = device->is_bluetooth && device->can_open_read_write;
        if (!device->is_supported) {
            device->unavailable_reason = device->is_bluetooth ? L"open read/write failed" : L"not bluetooth";
        }
        return;
    }

    if (microsoft_xbox_controller(*device)) {
        device->device_type = device->is_bluetooth ? L"xbox_bt" : L"xbox_usb";
        device->is_supported = device->is_bluetooth && device->can_open_read_write;
        if (!device->is_supported) {
            device->unavailable_reason = device->is_bluetooth ? L"open read/write failed" : L"not bluetooth";
        }
        return;
    }

    device->device_type = device->is_bluetooth
        ? (device->is_game_controller ? L"unknown_bt_controller" : L"bt_hid")
        : L"hid";
    device->unavailable_reason = device->is_game_controller ? L"unsupported" : L"not a game controller";
}

static CtmBtDevice make_empty_device()
{
    CtmBtDevice device = {};
    device.is_bluetooth = false;
    device.is_game_controller = false;
    device.is_supported = false;
    device.can_open_read_write = false;
    return device;
}

static std::wstring query_device_registry_string(HDEVINFO info, SP_DEVINFO_DATA *devInfo, DWORD property)
{
    wchar_t buffer[512] = {};
    DWORD type = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(
            info,
            devInfo,
            property,
            &type,
            reinterpret_cast<PBYTE>(buffer),
            sizeof(buffer),
            nullptr)) {
        return L"";
    }
    if (type != REG_SZ && type != REG_EXPAND_SZ) {
        return L"";
    }
    return buffer;
}

static void append_bluetooth_controller_parents(
    std::vector<CtmBtDevice> *devices,
    const std::set<std::wstring> &knownBluetoothParents)
{
    HDEVINFO info = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (info == INVALID_HANDLE_VALUE) {
        return;
    }

    for (DWORD index = 0; index < 256; ++index) {
        SP_DEVINFO_DATA devInfo;
        ZeroMemory(&devInfo, sizeof(devInfo));
        devInfo.cbSize = sizeof(devInfo);
        if (!SetupDiEnumDeviceInfo(info, index, &devInfo)) {
            break;
        }

        std::wstring instance = get_device_instance_id(devInfo.DevInst);
        if (!starts_with_case_insensitive(instance, L"BTHENUM\\DEV_")) {
            continue;
        }
        if (knownBluetoothParents.find(lower_copy(instance)) != knownBluetoothParents.end()) {
            continue;
        }

        std::wstring friendly = query_device_registry_string(info, &devInfo, SPDRP_FRIENDLYNAME);
        std::wstring description = query_device_registry_string(info, &devInfo, SPDRP_DEVICEDESC);
        std::wstring display = !friendly.empty() ? friendly : description;
        if (!name_looks_controller(display) && !instance_looks_bluetooth_sony_controller(instance)) {
            continue;
        }

        CtmBtDevice device = make_empty_device();
        device.instance_id = instance;
        device.parent_instance_id = instance;
        device.product = !display.empty() ? display : L"Bluetooth controller";
        device.is_bluetooth = true;
        device.is_game_controller = true;
        device.unavailable_reason = L"HID interface not visible";

        if (contains_case_insensitive(display, L"DualShock")) {
            device.device_type = L"ds4_bt";
            device.vendor_id = CTM_SONY_VID;
            device.product_id = 0x05c4;
        } else if (contains_case_insensitive(display, L"DualSense") ||
                   instance_looks_bluetooth_sony_controller(instance)) {
            device.device_type = L"ds5_bt";
            device.vendor_id = CTM_SONY_VID;
            device.product_id = 0x0ce6;
        } else if (contains_case_insensitive(display, L"Xbox")) {
            device.device_type = L"xbox_bt";
            device.vendor_id = CTM_MICROSOFT_VID;
        } else {
            device.device_type = L"unknown_bt_controller";
        }

        devices->push_back(device);
    }

    SetupDiDestroyDeviceInfoList(info);
}

static std::vector<CtmBtDevice> enumerate_hid_devices(bool bluetoothOnly, bool controllersOnly)
{
    std::vector<CtmBtDevice> devices;
    std::set<std::wstring> bluetoothParents;
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO info = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (info == INVALID_HANDLE_VALUE) {
        return devices;
    }

    for (DWORD index = 0; index < 256; ++index) {
        SP_DEVICE_INTERFACE_DATA ifData;
        ZeroMemory(&ifData, sizeof(ifData));
        ifData.cbSize = sizeof(ifData);
        if (!SetupDiEnumDeviceInterfaces(info, nullptr, &hidGuid, index, &ifData)) {
            break;
        }

        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &ifData, nullptr, 0, &required, nullptr);
        if (required == 0) {
            continue;
        }

        std::vector<unsigned char> storage(required);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
        detail->cbSize = sizeof(*detail);

        SP_DEVINFO_DATA devInfo;
        ZeroMemory(&devInfo, sizeof(devInfo));
        devInfo.cbSize = sizeof(devInfo);

        if (!SetupDiGetDeviceInterfaceDetailW(info, &ifData, detail, required, nullptr, &devInfo)) {
            continue;
        }

        std::wstring instance = get_device_instance_id(devInfo.DevInst);
        std::wstring path = detail->DevicePath;
        std::wstring parentInstance;
        // Always walk ancestors so we capture the BTHENUM\Dev_XXXXXXXXXXXX
        // grandparent (which carries the real BT MAC). Previously this was
        // gated by short-circuit OR: when the HID interface instance already
        // contained VID&0002054C, instance_looks_bluetooth() returned true and
        // ancestor_looks_bluetooth() was never called, leaving the MAC unknown
        // and forcing us to fall back to the firmware-stamped serial.
        const bool ancestorBt = ancestor_looks_bluetooth(devInfo.DevInst, &parentInstance);
        const bool bt = instance_looks_bluetooth(instance) ||
                        instance_looks_bluetooth(path) ||
                        ancestorBt;
        if (!parentInstance.empty()) {
            bluetoothParents.insert(lower_copy(parentInstance));
        }
        if (bluetoothOnly && !bt) {
            continue;
        }

        CtmBtDevice device = make_empty_device();
        device.path = path;
        device.instance_id = instance;
        device.parent_instance_id = parentInstance;
        device.is_bluetooth = bt;

        HANDLE handle = open_hid_path(path, GENERIC_READ | GENERIC_WRITE);
        if (handle != INVALID_HANDLE_VALUE) {
            device.can_open_read_write = true;
            fill_hid_details(handle, &device);
            CloseHandle(handle);
        } else {
            handle = open_hid_path(path, 0);
            if (handle != INVALID_HANDLE_VALUE) {
                fill_hid_details(handle, &device);
                CloseHandle(handle);
            }
        }

        if (device.product.empty()) {
            device.product = query_device_registry_string(info, &devInfo, SPDRP_FRIENDLYNAME);
        }
        if (device.product.empty()) {
            device.product = query_device_registry_string(info, &devInfo, SPDRP_DEVICEDESC);
        }
        if (device.product.empty()) {
            device.product = bt ? L"Bluetooth HID device" : L"HID device";
        }

        classify_device(&device);
        if (controllersOnly && !device.is_game_controller) {
            continue;
        }

        devices.push_back(device);
    }

    SetupDiDestroyDeviceInfoList(info);

    if (bluetoothOnly && controllersOnly) {
        append_bluetooth_controller_parents(&devices, bluetoothParents);
    }
    return devices;
}

std::vector<CtmBtDevice> ctm_list_bluetooth_hid_devices()
{
    return enumerate_hid_devices(true, true);
}

std::vector<CtmBtDevice> ctm_list_hid_devices()
{
    return enumerate_hid_devices(false, false);
}
