#pragma once

#include <string>
#include <vector>

struct CtmBtDevice {
    std::wstring path;
    std::wstring instance_id;
    std::wstring parent_instance_id;
    std::wstring product;
    std::wstring manufacturer;
    std::wstring serial;
    std::wstring device_type;
    std::wstring unavailable_reason;
    unsigned short vendor_id;
    unsigned short product_id;
    unsigned short version_number;
    unsigned short usage_page;
    unsigned short usage;
    unsigned short input_report_length;
    unsigned short output_report_length;
    unsigned short feature_report_length;
    bool is_bluetooth;
    bool is_game_controller;
    bool is_supported;
    bool can_open_read_write;
};

std::vector<CtmBtDevice> ctm_list_bluetooth_hid_devices();
std::vector<CtmBtDevice> ctm_list_hid_devices();
