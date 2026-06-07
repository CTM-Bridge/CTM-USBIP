struct EndpointInfo {
    uint8_t address = 0;
    uint8_t attributes = 0;
    uint16_t maxPacket = 0;
    uint8_t interval = 0;
};

struct InterfaceInfo {
    uint8_t number = 0;
    uint8_t cls = 0;
    uint8_t subClass = 0;
    uint8_t protocol = 0;
};

struct UsbDeviceInfo {
    uint16_t vid = 0x054c;
    uint16_t pid = 0x0ce6;
    uint16_t bcdDevice = 0x0100;
    uint8_t deviceClass = 0;
    uint8_t deviceSubClass = 0;
    uint8_t deviceProtocol = 0;
    uint8_t configurationValue = 1;
    uint8_t numConfigurations = 1;
    uint8_t numInterfaces = 0;
    bool full_speed = false;   // export as USB_SPEED_FULL instead of HIGH
    std::vector<InterfaceInfo> interfaces;
    std::vector<EndpointInfo> endpoints;
};

static UsbDeviceInfo parse_usb_info(const CtmDescriptorProfile &profile)
{
    UsbDeviceInfo info;
    info.full_speed = profile.full_speed;
    if (profile.device_descriptor.size() >= 18) {
        const uint8_t *d = profile.device_descriptor.data();
        info.deviceClass = d[4];
        info.deviceSubClass = d[5];
        info.deviceProtocol = d[6];
        info.vid = read_le16(d + 8);
        info.pid = read_le16(d + 10);
        info.bcdDevice = read_le16(d + 12);
        info.numConfigurations = d[17];
    }
    if (profile.configuration_descriptor.size() >= 9) {
        const uint8_t *c = profile.configuration_descriptor.data();
        info.numInterfaces = c[4];
        info.configurationValue = c[5];
    }

    std::array<bool, 256> seenInterface = {};
    size_t offset = 0;
    while (offset + 2 <= profile.configuration_descriptor.size()) {
        const uint8_t len = profile.configuration_descriptor[offset];
        const uint8_t type = profile.configuration_descriptor[offset + 1];
        if (len < 2 || offset + len > profile.configuration_descriptor.size()) {
            break;
        }
        const uint8_t *desc = profile.configuration_descriptor.data() + offset;
        if (type == 0x04 && len >= 9) {
            const uint8_t number = desc[2];
            if (!seenInterface[number]) {
                seenInterface[number] = true;
                InterfaceInfo intf;
                intf.number = number;
                intf.cls = desc[5];
                intf.subClass = desc[6];
                intf.protocol = desc[7];
                info.interfaces.push_back(intf);
            }
        } else if (type == 0x05 && len >= 7) {
            EndpointInfo ep;
            ep.address = desc[2];
            ep.attributes = desc[3];
            ep.maxPacket = read_le16(desc + 4);
            ep.interval = desc[6];
            info.endpoints.push_back(ep);
        }
        offset += len;
    }
    if (info.numInterfaces == 0) {
        info.numInterfaces = static_cast<uint8_t>(info.interfaces.size());
    }
    return info;
}

static const EndpointInfo *find_endpoint(const UsbDeviceInfo &info, uint8_t address)
{
    for (const EndpointInfo &ep : info.endpoints) {
        if (ep.address == address) {
            return &ep;
        }
    }
    return nullptr;
}

static const InterfaceInfo *find_interface(const UsbDeviceInfo &info, uint8_t number)
{
    for (const InterfaceInfo &intf : info.interfaces) {
        if (intf.number == number) {
            return &intf;
        }
    }
    return nullptr;
}

static bool interface_is_class(const UsbDeviceInfo &info, uint8_t number, uint8_t cls)
{
    const InterfaceInfo *intf = find_interface(info, number);
    return intf != nullptr && intf->cls == cls;
}

static bool endpoint_is_iso(const UsbDeviceInfo &info, uint8_t address)
{
    const EndpointInfo *ep = find_endpoint(info, address);
    return ep != nullptr && ((ep->attributes & 0x03) == 0x01);
}

static bool endpoint_is_interrupt(const UsbDeviceInfo &info, uint8_t address)
{
    const EndpointInfo *ep = find_endpoint(info, address);
    return ep != nullptr && ((ep->attributes & 0x03) == 0x03);
}

static uint32_t endpoint_interval_us(const UsbDeviceInfo &info, uint8_t address, uint32_t fallbackUs)
{
    const EndpointInfo *ep = find_endpoint(info, address);
    if (ep == nullptr || ep->interval == 0) {
        return fallbackUs;
    }
    const uint32_t exponent = (std::min<uint32_t>)(ep->interval, 16);
    const uint32_t highSpeedUs = 125u << (exponent - 1);
    return (std::max<uint32_t>)(125, (std::min<uint32_t>)(highSpeedUs, 100000));
}

// USB/IP advertised device speed, derived from the actual config descriptor. A
// full-speed bulk endpoint has a 64-byte max packet, which is ILLEGAL at high
// speed (HS bulk must be 512) -> Windows rejects the config ("Invalid
// Configuration Descriptor"). The composite builder raises the puck's bulk eps
// to 512 and we stay HIGH speed here; this is the safety net for any remaining
// sub-512 bulk endpoint. Interrupt-only devices (DS4/DS5) stay high-speed.
static uint32_t usb_speed_for(const UsbDeviceInfo &info)
{
    for (const EndpointInfo &ep : info.endpoints) {
        if ((ep.attributes & 0x03) == 0x02 && ep.maxPacket > 0 && ep.maxPacket < 512) {
            return kUsbSpeedFull;
        }
    }
    return kUsbSpeedHigh;
}
