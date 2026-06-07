#include "ctm/profile.h"

#include <windows.h>
#include <filesystem>
#include <fstream>
#include <sstream>

static bool file_exists(const std::wstring &path)
{
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring parent_path(const std::wstring &path)
{
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L"";
    }
    return path.substr(0, slash);
}

static std::wstring resolve_profile_path(const std::wstring &path)
{
    if (file_exists(path)) {
        return path;
    }

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, ARRAYSIZE(exePath));

    std::vector<std::wstring> bases;
    bases.push_back(parent_path(exePath));

    wchar_t cwd[MAX_PATH] = {};
    if (GetCurrentDirectoryW(ARRAYSIZE(cwd), cwd) > 0) {
        bases.push_back(cwd);
    }

    for (std::wstring base : bases) {
        for (int depth = 0; depth < 6 && !base.empty(); ++depth) {
            std::wstring candidate = base + L"\\" + path;
            if (file_exists(candidate)) {
                return candidate;
            }
            candidate = base + L"\\profiles\\descriptors\\" + path;
            if (file_exists(candidate)) {
                return candidate;
            }
            base = parent_path(base);
        }
    }

    return path;
}

static bool parse_hex_list(const std::string &text, std::vector<unsigned char> *out)
{
    std::stringstream ss(text);
    std::string token;
    while (ss >> token) {
        if (token.rfind("0x", 0) == 0 || token.rfind("0X", 0) == 0) {
            token = token.substr(2);
        }
        char *end = nullptr;
        long value = strtol(token.c_str(), &end, 16);
        if (!end || *end != '\0' || value < 0 || value > 255) {
            return false;
        }
        out->push_back(static_cast<unsigned char>(value));
    }
    return true;
}

static std::string trim_ascii(std::string value)
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.erase(value.begin());
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

bool ctm_load_descriptor_profile(const std::wstring &path, CtmDescriptorProfile *profile, std::wstring *error)
{
    std::wstring resolvedPath = resolve_profile_path(path);
    // Wrap in std::filesystem::path so the std::ifstream constructor that
    // accepts wide paths via path::native() is used. MSVC's ifstream accepts
    // std::wstring directly as a non-standard extension; libstdc++ (MinGW)
    // does not, but both honor the std::filesystem::path overload (C++17).
    std::ifstream file(std::filesystem::path{resolvedPath});
    if (!file) {
        if (error) *error = L"Could not open descriptor profile.";
        return false;
    }

    CtmDescriptorProfile local;
    std::string section;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        std::string key = trim_ascii(line.substr(0, equals));
        std::string value = trim_ascii(line.substr(equals + 1));

        if (section == "profile" && key == "id") {
            local.profile_id = value;
        } else if (section == "device_descriptor" && key == "bytes") {
            if (!parse_hex_list(value, &local.device_descriptor)) return false;
        } else if (section == "configuration_descriptor" && key == "bytes") {
            if (!parse_hex_list(value, &local.configuration_descriptor)) return false;
        } else if (section == "hid_report_descriptor" && key == "bytes") {
            if (!parse_hex_list(value, &local.hid_report_descriptor)) return false;
        } else if (section == "string_descriptors" && key == "bytes") {
            if (!parse_hex_list(value, &local.string_descriptors)) return false;
        } else if (section == "microsoft_os_string_descriptor" && key == "bytes") {
            if (!parse_hex_list(value, &local.microsoft_os_string_descriptor)) return false;
        } else if (section == "microsoft_os_compatible_id_descriptor" && key == "bytes") {
            if (!parse_hex_list(value, &local.microsoft_os_compatible_id_descriptor)) return false;
        }
    }

    if (local.profile_id.empty()) {
        local.profile_id = "unknown";
    }
    if (local.device_descriptor.empty() || local.configuration_descriptor.empty()) {
        if (error) *error = L"Descriptor profile is missing device or configuration descriptor bytes.";
        return false;
    }

    *profile = local;
    return true;
}
