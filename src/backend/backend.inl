struct BackendCaps {
    uint16_t vendorId = 0x054c;
    uint16_t productId = 0x0ce6;
    uint16_t version = 0x0100;
    uint16_t inputReportLength = 64;
    uint16_t outputReportLength = 64;
    uint16_t featureReportLength = 64;
    std::wstring serial;
    std::wstring product;
    std::wstring path;
    std::vector<uint8_t> hidReportDescriptor;
};

// endpoint = the physical IN endpoint address the report came from (composite
// puck); 0 for single-interface backends (the device uses its default input ep).
using RawInputCallback = std::function<void(const uint8_t *, size_t, uint8_t /*endpoint*/)>;

class CtmBackend {
public:
    virtual ~CtmBackend() = default;
    virtual bool start(RawInputCallback callback, std::wstring *error) = 0;
    virtual void stop() = 0;
    virtual BackendCaps caps() const = 0;
    virtual bool execute_feature_actions(
        const std::vector<CtmMapRuntime::PhysicalFeatureAction> &actions,
        std::vector<uint8_t> *scratch,
        const uint8_t **lastGetResponse,
        size_t *lastGetResponseLength,
        const char *reason,
        unsigned int timeoutMs) = 0;
    virtual bool send_output_report(const std::vector<uint8_t> &report, bool paced, std::wstring *error) = 0;

    // Composite: forward an OUT report tagged with the physical OUT endpoint it
    // targets (carried in the bridge message's request_id). Default ignores the
    // endpoint -- single-interface backends have one OUT pipe.
    virtual bool send_output_report_ep(const std::vector<uint8_t> &report, uint8_t /*endpoint*/, bool paced, std::wstring *error)
    {
        return send_output_report(report, paced, error);
    }

    // Composite: forward a SET/GET_REPORT verbatim to one interface's physical
    // hidraw (the map runtime is bypassed for composites). On GET, *reply gets
    // the device's response. Default: unsupported. Used only by multi-interface
    // composites (the puck) so each slot's config/feature reaches the right HID.
    virtual bool remote_interface_feature(uint8_t /*interface*/, bool /*get*/,
                                          const std::vector<uint8_t> & /*request*/,
                                          std::vector<uint8_t> * /*reply*/,
                                          unsigned int /*timeoutMs*/) { return false; }

    // Forwarded composite enumeration (CTMB_MSG_ENUM); empty for non-composite
    // backends. The agent builds the virtual composite device from this.
    virtual const std::vector<uint8_t> &enum_payload() const
    {
        static const std::vector<uint8_t> empty;
        return empty;
    }
};

static void append_usb_string_blob(std::vector<unsigned char> *out, const std::wstring &text)
{
    const std::vector<unsigned char> descriptor = make_usb_string_descriptor(text);
    out->insert(out->end(), descriptor.begin(), descriptor.end());
}

static bool make_dynamic_hid_profile(
    const BackendCaps &caps,
    CtmDescriptorProfile *profile,
    std::wstring *error)
{
    if (profile == nullptr) {
        return false;
    }
    if (caps.hidReportDescriptor.empty() || caps.hidReportDescriptor.size() > 4096) {
        if (error) *error = L"bridge did not provide a usable HID report descriptor";
        return false;
    }

    const uint16_t reportLen = static_cast<uint16_t>(caps.hidReportDescriptor.size());
    const bool hasOut = caps.outputReportLength != 0;
    const uint16_t totalLength = static_cast<uint16_t>(9 + 9 + 9 + 7 + (hasOut ? 7 : 0));
    const uint8_t endpointCount = hasOut ? 2 : 1;
    const uint16_t inPacket = static_cast<uint16_t>((std::max<uint16_t>)(1, (std::min<uint16_t>)(1024, caps.inputReportLength)));
    const uint16_t outPacket = static_cast<uint16_t>((std::max<uint16_t>)(1, (std::min<uint16_t>)(1024, caps.outputReportLength)));

    CtmDescriptorProfile local;
    local.profile_id = "dynamic_hid";
    local.device_descriptor = {
        0x12, 0x01, 0x00, 0x02,
        0x00, 0x00, 0x00, 0x40,
        static_cast<unsigned char>(caps.vendorId & 0xff),
        static_cast<unsigned char>((caps.vendorId >> 8) & 0xff),
        static_cast<unsigned char>(caps.productId & 0xff),
        static_cast<unsigned char>((caps.productId >> 8) & 0xff),
        static_cast<unsigned char>(caps.version & 0xff),
        static_cast<unsigned char>((caps.version >> 8) & 0xff),
        0x01, 0x02, 0x03, 0x01
    };

    local.configuration_descriptor = {
        0x09, 0x02,
        static_cast<unsigned char>(totalLength & 0xff),
        static_cast<unsigned char>((totalLength >> 8) & 0xff),
        0x01, 0x01, 0x00, 0x80, 0x32,
        0x09, 0x04, 0x00, 0x00, endpointCount, 0x03, 0x00, 0x00, 0x00,
        0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22,
        static_cast<unsigned char>(reportLen & 0xff),
        static_cast<unsigned char>((reportLen >> 8) & 0xff),
        0x07, 0x05, 0x81, 0x03,
        static_cast<unsigned char>(inPacket & 0xff),
        static_cast<unsigned char>((inPacket >> 8) & 0xff),
        0x01
    };
    if (hasOut) {
        local.configuration_descriptor.insert(local.configuration_descriptor.end(), {
            0x07, 0x05, 0x01, 0x03,
            static_cast<unsigned char>(outPacket & 0xff),
            static_cast<unsigned char>((outPacket >> 8) & 0xff),
            0x01
        });
    }

    local.hid_report_descriptor.assign(caps.hidReportDescriptor.begin(), caps.hidReportDescriptor.end());
    local.string_descriptors = {0x04, 0x03, 0x09, 0x04};
    append_usb_string_blob(&local.string_descriptors, L"CTM");
    append_usb_string_blob(
        &local.string_descriptors,
        caps.product.empty() ? L"CTM HID Device" : caps.product);
    append_usb_string_blob(
        &local.string_descriptors,
        caps.serial.empty() ? L"CTMUSBIP" : caps.serial);

    *profile = std::move(local);
    return true;
}

// Build a composite descriptor profile from a forwarded CTMB_MSG_ENUM payload.
// The payload carries the puck's OWN enumeration (device + configuration
// descriptors read verbatim from sysfs on the TV) followed by each HID
// interface's report descriptor. We replay the descriptors byte-for-byte; the
// only transform is presenting the device as full-speed (done at the usbip
// export layer) so the 64-byte CDC bulk endpoints are legal. Report descriptors
// are indexed by bInterfaceNumber for per-interface GET_DESCRIPTOR(report).
// Payload layout mirrors the TV's build_puck_enum_payload:
//   [ctmb_enum_info_t = 32B packed][descriptors blob]
//   iface_count x ( [ctmb_enum_iface_t = 4B packed][report_desc] )
static bool make_composite_profile_from_enum(
    const std::vector<uint8_t> &payload,
    CtmDescriptorProfile *profile,
    std::wstring *error)
{
    if (profile == nullptr) {
        return false;
    }
    const size_t kInfoSize = 32;   // sizeof(ctmb_enum_info_t), packed
    const size_t kIfaceSize = 4;   // sizeof(ctmb_enum_iface_t), packed
    if (payload.size() < kInfoSize) {
        if (error) *error = L"composite enum payload too small";
        return false;
    }
    const uint16_t descriptorsLen =
        static_cast<uint16_t>(payload[0] | (static_cast<uint16_t>(payload[1]) << 8));
    const uint8_t ifaceCount = payload[2];
    // payload[3] = full_speed flag (consumed at the usbip export layer).
    size_t off = kInfoSize;
    if (descriptorsLen < 18 || off + descriptorsLen > payload.size()) {
        if (error) *error = L"composite enum descriptors length invalid";
        return false;
    }

    CtmDescriptorProfile local;
    local.profile_id = "puck_composite";
    // First 18 bytes = device descriptor; the remainder = configuration
    // descriptor(s) (config header + all interfaces + endpoints), verbatim.
    local.device_descriptor.assign(
        payload.begin() + off, payload.begin() + off + 18);
    local.configuration_descriptor.assign(
        payload.begin() + off + 18, payload.begin() + off + descriptorsLen);
    off += descriptorsLen;

    // The real puck is a full-speed device: its CDC-data BULK endpoints report a
    // 64-byte wMaxPacketSize. We present the composite at HIGH speed (usbip-win2's
    // UDE host does not reliably honor a full-speed import), where bulk MUST be
    // 512 -- a 64-byte bulk endpoint makes Windows reject the whole configuration
    // ("Invalid Configuration Descriptor"). We never bridge the CDC bulk data
    // (COM is TBD), so raise any sub-512 bulk endpoint to 512: cosmetic, and it
    // makes the config high-speed-legal so usbccgp builds the composite tree.
    // HID interrupt endpoints are valid at 64 bytes and left untouched. (This is
    // what the known-good steam_puck_composite.profile did by hand.)
    {
        std::vector<unsigned char> &cfg = local.configuration_descriptor;
        size_t o = 0;
        while (o + 2 <= cfg.size()) {
            const uint8_t dlen = cfg[o];
            if (dlen < 2 || o + dlen > cfg.size()) break;
            if (dlen >= 7 && cfg[o + 1] == 0x05 && (cfg[o + 3] & 0x03) == 0x02) {
                const uint16_t mps =
                    static_cast<uint16_t>(cfg[o + 4] | (static_cast<uint16_t>(cfg[o + 5]) << 8));
                if (mps > 0 && mps < 512) { cfg[o + 4] = 0x00; cfg[o + 5] = 0x02; }  // -> 512
            }
            o += dlen;
        }
    }

    for (uint8_t i = 0; i < ifaceCount; ++i) {
        if (off + kIfaceSize > payload.size()) {
            if (error) *error = L"composite enum truncated (iface header)";
            return false;
        }
        const uint8_t interfaceNumber = payload[off];
        // payload[off + 1] = bInterfaceClass (informational here).
        const uint16_t reportLen =
            static_cast<uint16_t>(payload[off + 2] | (static_cast<uint16_t>(payload[off + 3]) << 8));
        off += kIfaceSize;
        if (off + reportLen > payload.size()) {
            if (error) *error = L"composite enum truncated (report descriptor)";
            return false;
        }
        if (reportLen > 0) {
            local.interface_report_descriptors[interfaceNumber].assign(
                payload.begin() + off, payload.begin() + off + reportLen);
        }
        off += reportLen;
    }

    // The forwarded device descriptor references string indices 1/2/3
    // (iManufacturer/iProduct/iSerialNumber). The real strings are not part of
    // the enum payload, so supply generic placeholders at those indices; any
    // iInterface index the config references that we do not provide simply
    // stalls (Windows tolerates a missing interface name).
    local.string_descriptors = {0x04, 0x03, 0x09, 0x04};
    append_usb_string_blob(&local.string_descriptors, L"Valve");
    append_usb_string_blob(&local.string_descriptors, L"Steam Controller");
    append_usb_string_blob(&local.string_descriptors, L"CTMUSBIP");

    *profile = std::move(local);
    return true;
}
