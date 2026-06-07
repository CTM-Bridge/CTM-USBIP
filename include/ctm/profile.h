#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct CtmDescriptorProfile {
    std::string profile_id;
    // Present the virtual device as full-speed instead of the default
    // high-speed. Required for the composite puck so its 64-byte CDC bulk
    // endpoints are legal (usbip-win2 rejects 64B bulk at high-speed).
    bool full_speed = false;
    std::vector<unsigned char> device_descriptor;
    std::vector<unsigned char> configuration_descriptor;
    std::vector<unsigned char> hid_report_descriptor;
    std::vector<unsigned char> string_descriptors;
    std::vector<unsigned char> microsoft_os_string_descriptor;
    std::vector<unsigned char> microsoft_os_compatible_id_descriptor;
    // Composite devices: per-interface HID report descriptors keyed by
    // bInterfaceNumber (the control-transfer wIndex). When non-empty, the
    // device serves these for GET_DESCRIPTOR(report) instead of the single
    // hid_report_descriptor above. Built from the forwarded CTMB_MSG_ENUM.
    std::map<uint8_t, std::vector<unsigned char>> interface_report_descriptors;
};

bool ctm_load_descriptor_profile(const std::wstring &path, CtmDescriptorProfile *profile, std::wstring *error);
