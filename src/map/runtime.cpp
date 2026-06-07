#include "ctm/map/runtime.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>

#include <opus/opus.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/sbc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
}

struct IniFile {
    std::map<std::string, std::map<std::string, std::string>> sections;
};

static constexpr uint32_t kMaxPhysicalFeatureReportBytes = 2048;

struct FfmpegSbcPrivPrefix {
    const AVClass *avClass = nullptr;
    int64_t maxDelay = 0;
    int msbc = 0;
    alignas(SBC_ALIGN) struct sbc_frame frame;
};

static_assert(offsetof(FfmpegSbcPrivPrefix, frame) % SBC_ALIGN == 0, "SBC frame must match FFmpeg alignment");

static std::string trim_ascii(std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

static bool load_ini(const std::wstring &path, IniFile *ini)
{
    // libstdc++ (MinGW) doesn't accept std::wstring; route through filesystem::path.
    std::ifstream file(std::filesystem::path{path});
    if (!file || ini == nullptr) {
        return false;
    }

    IniFile local;
    std::string section;
    std::string line;
    while (std::getline(file, line)) {
        line = trim_ascii(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = trim_ascii(line.substr(1, line.size() - 2));
            continue;
        }
        size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        local.sections[section][trim_ascii(line.substr(0, equals))] = trim_ascii(line.substr(equals + 1));
    }

    *ini = local;
    return true;
}

static std::string ini_get(const IniFile &ini, const std::string &section, const std::string &key)
{
    auto sectionIt = ini.sections.find(section);
    if (sectionIt == ini.sections.end()) {
        return {};
    }
    auto keyIt = sectionIt->second.find(key);
    if (keyIt == sectionIt->second.end()) {
        return {};
    }
    return keyIt->second;
}

static bool parse_u32(const std::string &text, uint32_t *value)
{
    if (text.empty() || value == nullptr) {
        return false;
    }
    char *end = nullptr;
    unsigned long parsed = std::strtoul(text.c_str(), &end, 0);
    if (end == text || *end != '\0' || parsed > 0xffffffffUL) {
        return false;
    }
    *value = static_cast<uint32_t>(parsed);
    return true;
}

static uint32_t get_u32(const IniFile &ini, const std::string &section, const std::string &key, uint32_t fallback)
{
    uint32_t value = 0;
    return parse_u32(ini_get(ini, section, key), &value) ? value : fallback;
}

static bool parse_i32(const std::string &text, int32_t *value)
{
    if (text.empty() || value == nullptr) {
        return false;
    }
    char *end = nullptr;
    long parsed = std::strtol(text.c_str(), &end, 0);
    if (end == text || *end != '\0' || parsed < INT32_MIN || parsed > INT32_MAX) {
        return false;
    }
    *value = static_cast<int32_t>(parsed);
    return true;
}

static int32_t get_i32(const IniFile &ini, const std::string &section, const std::string &key, int32_t fallback)
{
    int32_t value = 0;
    return parse_i32(ini_get(ini, section, key), &value) ? value : fallback;
}

static bool parse_range_copy(const std::string &text, CtmMapRuntime::CopyRange *range)
{
    if (range == nullptr) {
        return false;
    }

    unsigned int sb = 0;
    unsigned int se = 0;
    unsigned int db = 0;
    unsigned int de = 0;
    if (sscanf_s(text.c_str(), "source[%u..%u] -> destination[%u..%u]", &sb, &se, &db, &de) != 4) {
        return false;
    }
    if (se < sb || de < db || (se - sb) != (de - db)) {
        return false;
    }
    range->sourceBegin = sb;
    range->sourceEnd = se;
    range->destinationBegin = db;
    range->destinationEnd = de;
    return true;
}

static bool parse_hex_bytes(const std::string &text, std::vector<uint8_t> *bytes)
{
    if (bytes == nullptr) {
        return false;
    }
    bytes->clear();

    std::string normalized = text;
    for (char &ch : normalized) {
        if (ch == ',' || ch == ':') {
            ch = ' ';
        }
    }

    std::istringstream stream(normalized);
    std::string token;
    while (stream >> token) {
        char *end = nullptr;
        unsigned long parsed = std::strtoul(token.c_str(), &end, 16);
        if (end == token.c_str() || *end != '\0' || parsed > 0xff) {
            return false;
        }
        bytes->push_back(static_cast<uint8_t>(parsed));
    }
    return !bytes->empty();
}

static bool bytes_match_prefix(
    const uint8_t *data,
    size_t length,
    size_t offset,
    const std::vector<uint8_t> &prefix)
{
    if (data == nullptr || length < offset || length - offset < prefix.size()) {
        return false;
    }
    return std::memcmp(data + offset, prefix.data(), prefix.size()) == 0;
}

static bool starts_with(const std::string &text, const char *prefix);

static std::map<std::string, std::string> parse_op_args(const std::string &op)
{
    std::map<std::string, std::string> args;
    std::istringstream stream(op);
    std::string token;
    while (stream >> token) {
        size_t equals = token.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        args[token.substr(0, equals)] = token.substr(equals + 1);
    }
    return args;
}

static std::string first_positional_arg(const std::string &op, const char *primitive)
{
    if (primitive == nullptr || !starts_with(op, primitive)) {
        return {};
    }
    std::istringstream stream(op.substr(std::strlen(primitive)));
    std::string token;
    while (stream >> token) {
        if (token.find('=') == std::string::npos) {
            return token;
        }
    }
    return {};
}

static bool parse_crc32_tail_op(const std::string &op, uint8_t *seed)
{
    if (op.empty() || seed == nullptr ||
        (!starts_with(op, "crc32.hid_feature_set ") &&
         !starts_with(op, "crc32.hid_feature "))) {
        return false;
    }

    const auto args = parse_op_args(op);
    const auto seedIt = args.find("seed");
    const auto tailIt = args.find("tail");
    uint32_t parsedSeed = 0;
    if (seedIt == args.end() ||
        tailIt == args.end() ||
        tailIt->second != "last4" ||
        !parse_u32(seedIt->second, &parsedSeed) ||
        parsedSeed > 0xff) {
        return false;
    }

    *seed = static_cast<uint8_t>(parsedSeed);
    return true;
}

static std::vector<unsigned int> parse_index_list(const std::string &text)
{
    std::vector<unsigned int> out;
    std::string normalized = text;
    for (char &ch : normalized) {
        if (ch == ',') {
            ch = ' ';
        }
    }
    std::istringstream stream(normalized);
    std::string token;
    while (stream >> token) {
        uint32_t value = 0;
        if (!parse_u32(token, &value)) {
            out.clear();
            return out;
        }
        out.push_back(value);
    }
    return out;
}

static bool parse_range(const std::string &text, size_t *begin, size_t *end)
{
    if (begin == nullptr || end == nullptr) {
        return false;
    }
    unsigned int b = 0;
    unsigned int e = 0;
    if (sscanf_s(text.c_str(), "%u..%u", &b, &e) != 2 || e < b) {
        return false;
    }
    *begin = b;
    *end = e;
    return true;
}

static bool starts_with(const std::string &text, const char *prefix)
{
    return text.rfind(prefix, 0) == 0;
}

static uint8_t parse_sbc_channel_mode(const std::string &text, uint8_t fallback)
{
    if (text == "mono") return SBC_MODE_MONO;
    if (text == "dual" || text == "dual_channel") return SBC_MODE_DUAL_CHANNEL;
    if (text == "stereo") return SBC_MODE_STEREO;
    if (text == "joint" || text == "joint_stereo") return SBC_MODE_JOINT_STEREO;
    return fallback;
}

static uint8_t parse_sbc_allocation(const std::string &text, uint8_t fallback)
{
    if (text == "loudness") return SBC_AM_LOUDNESS;
    if (text == "snr") return SBC_AM_SNR;
    return fallback;
}

static uint8_t sbc_frequency_code(uint32_t sampleRate)
{
    if (sampleRate == 16000) return SBC_FREQ_16000;
    if (sampleRate == 32000) return SBC_FREQ_32000;
    if (sampleRate == 44100) return SBC_FREQ_44100;
    if (sampleRate == 48000) return SBC_FREQ_48000;
    return 0xff;
}

static int16_t read_s16_le(const uint8_t *data)
{
    return static_cast<int16_t>(static_cast<uint16_t>(data[0]) |
        (static_cast<uint16_t>(data[1]) << 8));
}

static int8_t s16_to_s8(int32_t value)
{
    value /= 256;
    if (value < -128) {
        value = -128;
    } else if (value > 127) {
        value = 127;
    }
    return static_cast<int8_t>(value);
}

static uint32_t crc32_step(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            uint32_t mask = static_cast<uint32_t>(0U - (crc & 1U));
            crc = (crc >> 1) ^ (0xedb88320U & mask);
        }
    }
    return crc;
}

static void init_usb_response(CTM_USB_RESPONSE *response, const CTM_USB_EVENT &event, uint32_t status)
{
    std::memset(response, 0, sizeof(*response));
    response->request_id = event.request_id;
    response->status = status;
}

static bool complete_u8_response(CTM_USB_RESPONSE *response, const CTM_USB_EVENT &event, uint8_t value, uint16_t requestedLength)
{
    if (response == nullptr || requestedLength < 1) {
        return false;
    }
    init_usb_response(response, event, CTM_USB_RESPONSE_SUCCESS);
    response->length = 1;
    response->data[0] = value;
    return true;
}

static bool complete_s16_response(CTM_USB_RESPONSE *response, const CTM_USB_EVENT &event, int16_t value, uint16_t requestedLength)
{
    if (response == nullptr || requestedLength < 2) {
        return false;
    }
    init_usb_response(response, event, CTM_USB_RESPONSE_SUCCESS);
    response->length = 2;
    response->data[0] = static_cast<uint8_t>(value & 0xff);
    response->data[1] = static_cast<uint8_t>((static_cast<uint16_t>(value) >> 8) & 0xff);
    return true;
}

static bool complete_u24_response(CTM_USB_RESPONSE *response, const CTM_USB_EVENT &event, uint32_t value, uint16_t requestedLength)
{
    if (response == nullptr || requestedLength < 3) {
        return false;
    }
    init_usb_response(response, event, CTM_USB_RESPONSE_SUCCESS);
    response->length = 3;
    response->data[0] = static_cast<uint8_t>(value & 0xff);
    response->data[1] = static_cast<uint8_t>((value >> 8) & 0xff);
    response->data[2] = static_cast<uint8_t>((value >> 16) & 0xff);
    return true;
}

CtmMapRuntime::~CtmMapRuntime()
{
    if (opus_ != nullptr) {
        opus_encoder_destroy(opus_);
        opus_ = nullptr;
    }
    if (sbcPacket_ != nullptr) {
        av_packet_free(&sbcPacket_);
    }
    if (sbcFrame_ != nullptr) {
        av_frame_free(&sbcFrame_);
    }
    if (sbc_ != nullptr) {
        avcodec_free_context(&sbc_);
    }
}

bool CtmMapRuntime::load(const std::wstring &path, std::wstring *error)
{
    path_ = path;
    if (!parse_map(error)) {
        return false;
    }

    mapCounter_ = 1;
    speakerPcm_.assign(isoFrameSamples_ * 2, 0);
    hapticPcm_.assign((isoSampleRate_ / hapticOutputRate_) * 32 * 2, 0);
    hapticPcmDownsampled_.assign((isoSampleRate_ / hapticOutputRate_) * 32 * 2, 0);
    hapticS8_.assign(64, 0);
    opusPayload_.assign(opusPayloadBytes_, 0);
    sbcPayload_.assign(sbcPayloadBytes_, 0);
    const size_t defaultEffectsPayloadBytes = outputEffectsPayloadLength_ != 0
        ? outputEffectsPayloadLength_
        : (outputOps_.empty()
            ? (outputCopy_.sourceEnd - outputCopy_.sourceBegin + 1)
            : (outputDestinationLength_ > 4 ? outputDestinationLength_ - 4 : 0));
    outputEffectsPayload_.assign(defaultEffectsPayloadBytes, 0);
    stateBlock_.assign(65, 0);
    timingBlock_.assign(9, 0);
    audioBlock_.assign(2 + opusPayloadBytes_, 0);
    hapticBlock_.assign(66, 0);
    return true;
}

bool CtmMapRuntime::parse_map(std::wstring *error)
{
    IniFile ini;
    if (!load_ini(path_, &ini)) {
        if (error) *error = L"Could not open map file.";
        return false;
    }

    usbInputEndpoint_ = static_cast<uint8_t>(get_u32(ini, "usb.endpoints", "hid_in", usbInputEndpoint_));
    usbOutputEndpoint_ = static_cast<uint8_t>(get_u32(ini, "usb.endpoints", "hid_out", usbOutputEndpoint_));
    usbIsoOutEndpoint_ = static_cast<uint8_t>(get_u32(ini, "usb.endpoints", "iso_out", usbIsoOutEndpoint_));
    usbIsoInEndpoint_ = static_cast<uint8_t>(get_u32(ini, "usb.endpoints", "iso_in", usbIsoInEndpoint_));

    inputSourceReport_ = static_cast<uint8_t>(get_u32(ini, "path.input.controller_to_virtual", "source_report", 0));
    inputSourceOffset_ = static_cast<size_t>(get_u32(ini, "path.input.controller_to_virtual", "source_offset", 0));
    inputDestinationReport_ = static_cast<uint8_t>(get_u32(ini, "path.input.controller_to_virtual", "destination_report", 0));
    inputDestinationLength_ = static_cast<uint16_t>(get_u32(ini, "path.input.controller_to_virtual", "destination_length", 0));
    const std::string legacyInputCopy = ini_get(ini, "path.input.controller_to_virtual", "copy");
    inputOps_.clear();
    for (uint32_t i = 1; i <= 128; ++i) {
        const std::string op = ini_get(ini, "path.input.controller_to_virtual", "op." + std::to_string(i));
        if (!op.empty()) {
            inputOps_.push_back(op);
        }
    }
    if (inputOps_.empty()) {
        // Legacy shorthand: a single bulk copy declared via `copy = ...`.
        if (!parse_range_copy(legacyInputCopy, &inputCopy_)) {
            if (error) *error = L"Map input path has invalid copy range.";
            return false;
        }
    } else if (!legacyInputCopy.empty()) {
        if (error) *error = L"Map input path may not declare both copy = and op.* lists.";
        return false;
    }

    outputSourceReport_ = static_cast<uint8_t>(get_u32(ini, "path.output.virtual_to_controller", "source_report", 0));
    outputDestinationReport_ = static_cast<uint8_t>(get_u32(ini, "path.output.virtual_to_controller", "destination_report", 0));
    outputDestinationLength_ = static_cast<uint16_t>(get_u32(ini, "path.output.virtual_to_controller", "destination_length", 0));
    outputTag_ = static_cast<uint8_t>(get_u32(ini, "path.output.virtual_to_controller", "tag", 0));
    outputEffectsPayloadLength_ = static_cast<size_t>(get_u32(ini, "path.output.virtual_to_controller", "effects_payload_length", 0));
    outputCrcEnabled_ = true;
    outputOps_.clear();
    for (uint32_t i = 1; i <= 128; ++i) {
        const std::string op = ini_get(ini, "path.output.virtual_to_controller", "op." + std::to_string(i));
        if (!op.empty()) {
            outputOps_.push_back(op);
        }
    }
    {
        const std::string tail = ini_get(ini, "path.output.virtual_to_controller", "tail");
        if (tail == "none" || tail == "disabled" || tail == "off") {
            outputCrcEnabled_ = false;
        } else if (!tail.empty()) {
            outputCrcEnabled_ = true;
            const auto args = parse_op_args(tail);
            auto seedIt = args.find("seed");
            if (seedIt == args.end() || !starts_with(tail, "crc32.hid_output ")) {
                if (error) *error = L"Map output path has unsupported tail.";
                return false;
            }
            uint32_t seed = 0;
            if (!parse_u32(seedIt->second, &seed) || seed > 0xff) {
                if (error) *error = L"Map output path has invalid CRC seed.";
                return false;
            }
            outputCrcSeed_ = static_cast<uint8_t>(seed);
        }
    }
    const std::string legacyOutputCopy = ini_get(ini, "path.output.virtual_to_controller", "copy");
    if (outputOps_.empty()) {
        if (!parse_range_copy(legacyOutputCopy, &outputCopy_)) {
            if (error) *error = L"Map output path has invalid copy range.";
            return false;
        }
    } else if (!legacyOutputCopy.empty()) {
        if (error) *error = L"Map output path may not declare both copy = and op.* lists.";
        return false;
    }

    featureSelectorReport_ = static_cast<uint8_t>(get_u32(ini, "path.control.feature_pages", "selector_report", 0));
    physicalSelectorReport_ = static_cast<uint8_t>(get_u32(ini, "path.control.feature_pages", "physical_selector_report", 0));
    physicalSelectorLength_ = static_cast<uint16_t>(get_u32(ini, "path.control.feature_pages", "physical_selector_length", 64));
    physicalSelectorCrc32Tail_ = false;
    physicalSelectorCrcSeed_ = 0;
    const std::string physicalSelectorCrcText = ini_get(ini, "path.control.feature_pages", "physical_selector_crc");
    if (!physicalSelectorCrcText.empty()) {
        if (!parse_crc32_tail_op(physicalSelectorCrcText, &physicalSelectorCrcSeed_)) {
            if (error) *error = L"Map physical selector CRC rule is invalid.";
            return false;
        }
        physicalSelectorCrc32Tail_ = true;
    }
    featurePageRules_.clear();
    for (uint32_t i = 1; i <= 64; ++i) {
        const std::string prefix = "page." + std::to_string(i) + ".";
        const std::string selectorText = ini_get(ini, "path.control.feature_pages", prefix + "selector");
        if (selectorText.empty()) {
            continue;
        }

        FeaturePageRule rule;
        if (!parse_hex_bytes(selectorText, &rule.selector)) {
            if (error) *error = L"Map feature page has invalid selector bytes.";
            return false;
        }

        rule.ackOnly = ini_get(ini, "path.control.feature_pages", prefix + "ack_only") == "true";
        rule.responseReport = static_cast<uint8_t>(get_u32(ini, "path.control.feature_pages", prefix + "response_report", 0));
        rule.responseLength = static_cast<uint16_t>(get_u32(ini, "path.control.feature_pages", prefix + "response_length", 0));
        rule.physicalReport = static_cast<uint8_t>(get_u32(ini, "path.control.feature_pages", prefix + "physical_report", 0));

        const std::string responsePrefixText = ini_get(ini, "path.control.feature_pages", prefix + "response_prefix");
        if (!responsePrefixText.empty() && !parse_hex_bytes(responsePrefixText, &rule.responsePrefix)) {
            if (error) *error = L"Map feature page has invalid response prefix bytes.";
            return false;
        }
        if (ini_get(ini, "path.control.feature_pages", prefix + "static") == "true") {
            rule.staticResponse.assign(rule.responseLength, 0);
            if (rule.responsePrefix.size() > rule.staticResponse.size()) {
                if (error) *error = L"Map feature page static response prefix is too long.";
                return false;
            }
            std::copy(rule.responsePrefix.begin(), rule.responsePrefix.end(), rule.staticResponse.begin());
        }
        const std::string physicalPrefixText = ini_get(ini, "path.control.feature_pages", prefix + "physical_request_prefix");
        if (!physicalPrefixText.empty() && !parse_hex_bytes(physicalPrefixText, &rule.physicalRequestPrefix)) {
            if (error) *error = L"Map feature page has invalid physical request prefix bytes.";
            return false;
        }
        rule.physicalRequestLength = static_cast<uint16_t>(
            get_u32(ini, "path.control.feature_pages", prefix + "physical_request_length", 0));
        rule.physicalSetSelector =
            ini_get(ini, "path.control.feature_pages", prefix + "physical_set_selector") == "true";
        if (!rule.ackOnly && (rule.responseReport == 0 || rule.responseLength == 0)) {
            if (error) *error = L"Map feature page response rule is incomplete.";
            return false;
        }
        if (rule.physicalSetSelector && physicalSelectorReport_ == 0) {
            if (error) *error = L"Map feature page set selector requires physical_selector_report.";
            return false;
        }
        featurePageRules_.push_back(rule);
    }

    usbControlMapRules_.clear();
    for (uint32_t i = 1; i <= 128; ++i) {
        const std::string prefix = "rule." + std::to_string(i) + ".";
        const std::string match = ini_get(ini, "path.control.usb_control_map", prefix + "match");
        if (match.empty()) {
            continue;
        }

        UsbControlMapRule rule;
        const auto matchArgs = parse_op_args(match);
        if (starts_with(match, "usb.feature_get")) {
            rule.matchEvent = CTM_USB_EVENT_FEATURE_GET;
        } else if (starts_with(match, "usb.feature_set")) {
            rule.matchEvent = CTM_USB_EVENT_FEATURE_SET;
        } else {
            if (error) *error = L"Map USB control rule has unsupported match.";
            return false;
        }

        auto matchReportIt = matchArgs.find("report");
        if (matchReportIt != matchArgs.end()) {
            uint32_t report = 0;
            if (!parse_u32(matchReportIt->second, &report) || report > 0xff) {
                if (error) *error = L"Map USB control rule has invalid match report.";
                return false;
            }
            rule.matchReport = static_cast<uint8_t>(report);
            rule.hasMatchReport = true;
        }

        rule.actions.clear();
        for (uint32_t actionIndex = 1; actionIndex <= 16; ++actionIndex) {
            const std::string action = ini_get(
                ini,
                "path.control.usb_control_map",
                prefix + "action." + std::to_string(actionIndex));
            if (action.empty()) {
                continue;
            }

            PhysicalFeatureAction parsedAction;
            if (starts_with(action, "physical.hid.get_feature ")) {
                parsedAction.operation = PhysicalFeatureOperation::GetFeature;
            } else if (starts_with(action, "physical.hid.set_feature ")) {
                parsedAction.operation = PhysicalFeatureOperation::SetFeature;
            } else {
                if (error) *error = L"Map USB control rule has unsupported action.";
                return false;
            }

            const auto actionArgs = parse_op_args(action);
            auto physicalReportIt = actionArgs.find("report");
            auto physicalLengthIt = actionArgs.find("length");
            uint32_t physicalReport = 0;
            uint32_t physicalLength = 0;
            if (physicalReportIt == actionArgs.end() ||
                physicalLengthIt == actionArgs.end() ||
                !parse_u32(physicalReportIt->second, &physicalReport) ||
                !parse_u32(physicalLengthIt->second, &physicalLength) ||
                physicalReport > 0xff ||
                physicalLength == 0 ||
                physicalLength > kMaxPhysicalFeatureReportBytes) {
                if (error) *error = L"Map USB control rule has invalid physical feature action.";
                return false;
            }

            parsedAction.report = static_cast<uint8_t>(physicalReport);
            parsedAction.length = static_cast<uint16_t>(physicalLength);
            auto payloadIt = actionArgs.find("payload");
            if (payloadIt == actionArgs.end()) {
                payloadIt = actionArgs.find("prefix");
            }
            if (payloadIt != actionArgs.end() && !parse_hex_bytes(payloadIt->second, &parsedAction.payload)) {
                if (error) *error = L"Map USB control rule has invalid physical action payload.";
                return false;
            }
            if (1 + parsedAction.payload.size() > parsedAction.length) {
                if (error) *error = L"Map USB control rule physical action payload is too long.";
                return false;
            }
            auto bestEffortIt = actionArgs.find("best_effort");
            if (bestEffortIt != actionArgs.end()) {
                parsedAction.bestEffort = bestEffortIt->second == "true";
            }
            auto crcSeedIt = actionArgs.find("crc32_seed");
            if (crcSeedIt != actionArgs.end()) {
                uint32_t crcSeed = 0;
                auto crcTailIt = actionArgs.find("crc32_tail");
                if (!parse_u32(crcSeedIt->second, &crcSeed) ||
                    crcSeed > 0xff ||
                    (crcTailIt != actionArgs.end() && crcTailIt->second != "last4")) {
                    if (error) *error = L"Map USB control rule has invalid physical action CRC.";
                    return false;
                }
                parsedAction.crc32Tail = true;
                parsedAction.crc32Seed = static_cast<uint8_t>(crcSeed);
            }
            if (parsedAction.crc32Tail && !apply_physical_feature_crc(&parsedAction)) {
                if (error) *error = L"Map USB control rule physical action CRC could not be applied.";
                return false;
            }

            if (parsedAction.operation == PhysicalFeatureOperation::GetFeature) {
                rule.physicalReport = parsedAction.report;
                rule.physicalLength = parsedAction.length;
                rule.physicalRequestPrefix = parsedAction.payload;
            }
            rule.actions.push_back(std::move(parsedAction));
        }
        const std::string response = ini_get(ini, "path.control.usb_control_map", prefix + "response");
        if (rule.matchEvent == CTM_USB_EVENT_FEATURE_SET && response.empty() && rule.actions.empty()) {
            usbControlMapRules_.push_back(rule);
            continue;
        }
        if (!starts_with(response, "usb.respond ")) {
            if (error) *error = L"Map USB control rule has unsupported response.";
            return false;
        }
        const auto responseArgs = parse_op_args(response);
        auto responseLengthIt = responseArgs.find("length");
        uint32_t responseLength = 0;
        if (responseLengthIt == responseArgs.end() ||
            !parse_u32(responseLengthIt->second, &responseLength) ||
            responseLength == 0 ||
            responseLength > CTM_USB_RESPONSE_DATA_BYTES) {
            if (error) *error = L"Map USB control rule has invalid response length.";
            return false;
        }
        rule.responseLength = static_cast<uint16_t>(responseLength);
        const std::string responsePrefixText = ini_get(
            ini,
            "path.control.usb_control_map",
            prefix + "response_prefix");
        auto responsePrefixIt = responseArgs.find("prefix");
        if (!responsePrefixText.empty()) {
            if (!parse_hex_bytes(responsePrefixText, &rule.responsePrefix)) {
                if (error) *error = L"Map USB control rule has invalid response prefix.";
                return false;
            }
        } else if (responsePrefixIt != responseArgs.end() &&
            !parse_hex_bytes(responsePrefixIt->second, &rule.responsePrefix)) {
            if (error) *error = L"Map USB control rule has invalid response prefix.";
            return false;
        }
        auto staticIt = responseArgs.find("static");
        if (staticIt != responseArgs.end() && staticIt->second == "true") {
            rule.staticResponse.assign(rule.responseLength, 0);
            if (rule.responsePrefix.size() > rule.staticResponse.size()) {
                if (error) *error = L"Map USB control static response prefix is too long.";
                return false;
            }
            std::copy(rule.responsePrefix.begin(), rule.responsePrefix.end(), rule.staticResponse.begin());
        }
        if (rule.actions.empty()) {
            if (rule.staticResponse.empty()) {
                if (error) *error = L"Map USB control rule must include physical action or static response.";
                return false;
            }
        } else if (rule.physicalReport == 0 || rule.physicalLength == 0) {
            if (error) *error = L"Map USB control rule must include a physical get_feature action.";
            return false;
        }
        rule.cache =
            ini_get(ini, "path.control.usb_control_map", prefix + "cache") == "true" ||
            ini_get(ini, "path.control.usb_control_map", prefix + "cacheable_info") == "true";
        rule.preload = ini_get(ini, "path.control.usb_control_map", prefix + "preload") == "true";
        if (rule.preload && (!rule.cache || !rule.hasMatchReport)) {
            if (error) *error = L"Map USB control preload requires cache=true and a match report.";
            return false;
        }

        usbControlMapRules_.push_back(rule);
    }

    connectFeatureRequests_.clear();
    for (uint32_t i = 1; i <= 64; ++i) {
        const std::string key = "action." + std::to_string(i);
        const std::string action = ini_get(ini, "path.connect.actions", key);
        if (action.empty()) {
            continue;
        }
        if (!starts_with(action, "hid.get_feature ")) {
            if (error) *error = L"Map connect action is unsupported.";
            return false;
        }
        const auto args = parse_op_args(action);
        auto reportIt = args.find("report");
        auto lengthIt = args.find("length");
        if (reportIt == args.end() || lengthIt == args.end()) {
            if (error) *error = L"Map connect feature action has no report or length.";
            return false;
        }
        uint32_t report = 0;
        uint32_t length = 0;
        if (!parse_u32(reportIt->second, &report) ||
            !parse_u32(lengthIt->second, &length) ||
            report > 0xff ||
            length == 0 ||
            length > kMaxPhysicalFeatureReportBytes) {
            if (error) *error = L"Map connect feature action has invalid report or length.";
            return false;
        }
        ConnectFeatureRequest request;
        request.report = static_cast<uint8_t>(report);
        request.length = static_cast<uint16_t>(length);
        auto prefixIt = args.find("prefix");
        if (prefixIt != args.end() && !parse_hex_bytes(prefixIt->second, &request.prefix)) {
            if (error) *error = L"Map connect feature action has invalid prefix.";
            return false;
        }
        if (1 + request.prefix.size() > request.length) {
            if (error) *error = L"Map connect feature action prefix is longer than length.";
            return false;
        }
        connectFeatureRequests_.push_back(request);
    }

    virtualInputRules_.clear();
    for (uint32_t i = 1; i <= 128; ++i) {
        const std::string prefix = "rule." + std::to_string(i) + ".";
        const std::string match = ini_get(ini, "path.virtual.input_packets", prefix + "match");
        if (match.empty()) {
            continue;
        }

        VirtualInputRule rule;
        const auto args = parse_op_args(match);
        if (starts_with(match, "usb.control")) {
            rule.matchEvent = CTM_USB_EVENT_CONTROL;
        } else if (starts_with(match, "usb.endpoint_out")) {
            rule.matchEvent = CTM_USB_EVENT_HID_OUTPUT;
        } else {
            if (error) *error = L"Map virtual input rule has unsupported match.";
            return false;
        }

        auto endpointIt = args.find("endpoint");
        if (endpointIt != args.end()) {
            uint32_t endpoint = 0;
            if (!parse_u32(endpointIt->second, &endpoint) || endpoint > 0xff) {
                if (error) *error = L"Map virtual input rule has invalid match endpoint.";
                return false;
            }
            rule.matchEndpoint = static_cast<uint8_t>(endpoint);
            rule.hasMatchEndpoint = true;
        }

        auto prefixIt = args.find("prefix");
        if (prefixIt == args.end()) {
            prefixIt = args.find("setup_prefix");
        }
        if (prefixIt != args.end() && !parse_hex_bytes(prefixIt->second, &rule.matchPrefix)) {
            if (error) *error = L"Map virtual input rule has invalid match prefix.";
            return false;
        }

        rule.destinationEndpoint = static_cast<uint8_t>(
            get_u32(ini, "path.virtual.input_packets", prefix + "destination_endpoint", usbInputEndpoint_));
        if (rule.destinationEndpoint == 0) {
            if (error) *error = L"Map virtual input rule has invalid destination endpoint.";
            return false;
        }

        for (uint32_t packetIndex = 1; packetIndex <= 64; ++packetIndex) {
            const std::string packet = ini_get(
                ini,
                "path.virtual.input_packets",
                prefix + "packet." + std::to_string(packetIndex));
            if (packet.empty()) {
                continue;
            }
            std::vector<uint8_t> bytes;
            if (!parse_hex_bytes(packet, &bytes) || bytes.size() > sizeof(((CTM_INPUT_REPORT *)0)->data)) {
                if (error) *error = L"Map virtual input rule has invalid packet bytes.";
                return false;
            }
            rule.packets.push_back(std::move(bytes));
        }
        if (rule.packets.empty()) {
            if (error) *error = L"Map virtual input rule has no packets.";
            return false;
        }
        virtualInputRules_.push_back(std::move(rule));
    }

    audioControlInterface_ = static_cast<uint8_t>(get_u32(ini, "path.control.uac1_feature_units", "audio_control_interface", audioControlInterface_));
    speakerFeatureUnit_ = static_cast<uint8_t>(get_u32(ini, "path.control.uac1_feature_units", "speaker_unit", speakerFeatureUnit_));
    headsetFeatureUnit_ = static_cast<uint8_t>(get_u32(ini, "path.control.uac1_feature_units", "headset_unit", headsetFeatureUnit_));
    muteControl_ = static_cast<uint8_t>(get_u32(ini, "path.control.uac1_feature_units", "mute_control", muteControl_));
    volumeControl_ = static_cast<uint8_t>(get_u32(ini, "path.control.uac1_feature_units", "volume_control", volumeControl_));
    sampleFreqControl_ = static_cast<uint8_t>(get_u32(ini, "path.control.uac1_endpoint_sample_freq", "sample_freq_control", sampleFreqControl_));

    speakerMute_ = static_cast<uint8_t>(get_u32(ini, "path.control.uac1_feature_units", "speaker.mute.default", speakerMute_));
    speakerVolumeRaw_ = static_cast<int16_t>(get_i32(ini, "path.control.uac1_feature_units", "speaker.volume.default", speakerVolumeRaw_));
    speakerVolumeMin_ = static_cast<int16_t>(get_i32(ini, "path.control.uac1_feature_units", "speaker.volume.min", speakerVolumeMin_));
    speakerVolumeMax_ = static_cast<int16_t>(get_i32(ini, "path.control.uac1_feature_units", "speaker.volume.max", speakerVolumeMax_));
    speakerVolumeRes_ = static_cast<int16_t>(get_i32(ini, "path.control.uac1_feature_units", "speaker.volume.res", speakerVolumeRes_));
    audioMute_ = speakerMute_ != 0 ? 0x00 : 0xff;

    headsetMute_ = static_cast<uint8_t>(get_u32(ini, "path.control.uac1_feature_units", "headset.mute.default", headsetMute_));
    headsetVolumeRaw_ = static_cast<int16_t>(get_i32(ini, "path.control.uac1_feature_units", "headset.volume.default", headsetVolumeRaw_));
    headsetVolumeMin_ = static_cast<int16_t>(get_i32(ini, "path.control.uac1_feature_units", "headset.volume.min", headsetVolumeMin_));
    headsetVolumeMax_ = static_cast<int16_t>(get_i32(ini, "path.control.uac1_feature_units", "headset.volume.max", headsetVolumeMax_));
    headsetVolumeRes_ = static_cast<int16_t>(get_i32(ini, "path.control.uac1_feature_units", "headset.volume.res", headsetVolumeRes_));

    isoOutSampleRate_ = get_u32(ini, "path.control.uac1_endpoint_sample_freq", "endpoint.0x01.default", isoOutSampleRate_);
    isoOutSampleRateMin_ = get_u32(ini, "path.control.uac1_endpoint_sample_freq", "endpoint.0x01.min", isoOutSampleRateMin_);
    isoOutSampleRateMax_ = get_u32(ini, "path.control.uac1_endpoint_sample_freq", "endpoint.0x01.max", isoOutSampleRateMax_);
    isoOutSampleRateRes_ = get_u32(ini, "path.control.uac1_endpoint_sample_freq", "endpoint.0x01.res", isoOutSampleRateRes_);
    isoInSampleRate_ = get_u32(ini, "path.control.uac1_endpoint_sample_freq", "endpoint.0x82.default", isoInSampleRate_);
    isoInSampleRateMin_ = get_u32(ini, "path.control.uac1_endpoint_sample_freq", "endpoint.0x82.min", isoInSampleRateMin_);
    isoInSampleRateMax_ = get_u32(ini, "path.control.uac1_endpoint_sample_freq", "endpoint.0x82.max", isoInSampleRateMax_);
    isoInSampleRateRes_ = get_u32(ini, "path.control.uac1_endpoint_sample_freq", "endpoint.0x82.res", isoInSampleRateRes_);

    isoExpectedLength_ = static_cast<uint16_t>(get_u32(ini, "path.iso.virtual_to_physical_stream", "expected_urb_bytes", 3840));
    isoSampleRate_ = get_u32(ini, "path.iso.virtual_to_physical_stream", "sample_rate", isoSampleRate_);
    reservoirSampleRate_ = get_u32(
        ini,
        "path.iso.virtual_to_physical_stream",
        "reservoir_sample_rate",
        isoSampleRate_);
    if (reservoirSampleRate_ < 8000 || reservoirSampleRate_ > 96000) {
        reservoirSampleRate_ = isoSampleRate_;
    }
    isoChannels_ = static_cast<uint8_t>(get_u32(ini, "path.iso.virtual_to_physical_stream", "channels", isoChannels_));
    isoFrameSamples_ = static_cast<uint16_t>(get_u32(ini, "path.iso.virtual_to_physical_stream", "frame_samples", isoFrameSamples_));
    hapticOutputRate_ = static_cast<uint16_t>(get_u32(ini, "path.iso.virtual_to_physical_stream", "haptic_output_rate", hapticOutputRate_));
    streamReportId_ = static_cast<uint8_t>(get_u32(ini, "path.iso.virtual_to_physical_stream", "physical_report_id", streamReportId_));
    streamReportLength_ = static_cast<uint16_t>(get_u32(ini, "path.iso.virtual_to_physical_stream", "physical_report_length", streamReportLength_));
    {
        const std::string pace = ini_get(ini, "path.iso.virtual_to_physical_stream", "bt_pace_ms");
        if (!pace.empty()) {
            char *end = nullptr;
            const double parsed = std::strtod(pace.c_str(), &end);
            if (end != pace.c_str() && parsed >= 0.0 && parsed <= 50.0) {
                btAudioPaceMs_ = parsed;
            }
        }
    }
    {
        const std::string pace = ini_get(
            ini,
            "path.iso.virtual_to_physical_stream",
            "audio_builder_pace_ms");
        if (!pace.empty()) {
            char *end = nullptr;
            const double parsed = std::strtod(pace.c_str(), &end);
            if (end != pace.c_str() && parsed >= 0.0 && parsed <= 50.0) {
                audioBuilderPaceMs_ = parsed;
            }
        }
    }
    // Optional. 0 means "leave it to ctmctl's CLI flag / default". Clamp to a
    // sane range to avoid a misconfigured map locking up the audio engine.
    {
        const uint32_t parsed = get_u32(
            ini, "path.iso.virtual_to_physical_stream", "iso_out_completion_delay_us", 0);
        if (parsed != 0 && parsed <= 100000) {
            isoOutCompletionDelayUs_ = parsed;
        }
    }
    {
        const std::string scale = ini_get(
            ini,
            "path.iso.virtual_to_physical_stream",
            "iso_out_completion_delay_scale");
        if (!scale.empty()) {
            char *end = nullptr;
            const double parsed = std::strtod(scale.c_str(), &end);
            if (end != scale.c_str() && parsed > 0.0 && parsed <= 2.0) {
                isoOutCompletionDelayScale_ = parsed;
            }
        }
    }
    // Intermediate reservoir + adaptive-pace knobs. All optional; defaults are
    // applied in the header. Bounds-check to avoid pathological configs.
    intermediateBufferWarmupMs_ = get_u32(
        ini, "path.iso.virtual_to_physical_stream", "intermediate_buffer_warmup_ms", intermediateBufferWarmupMs_);
    intermediateBufferMaxMs_ = get_u32(
        ini, "path.iso.virtual_to_physical_stream", "intermediate_buffer_max_ms", intermediateBufferMaxMs_);
    if (intermediateBufferMaxMs_ < intermediateBufferWarmupMs_) {
        intermediateBufferMaxMs_ = intermediateBufferWarmupMs_;
    }
    paceHardMinFillMs_ = get_u32(
        ini, "path.iso.virtual_to_physical_stream", "pace_hard_min_fill_ms", paceHardMinFillMs_);
    paceHardMaxFillMs_ = get_u32(
        ini, "path.iso.virtual_to_physical_stream", "pace_hard_max_fill_ms", paceHardMaxFillMs_);
    if (paceHardMaxFillMs_ < paceHardMinFillMs_) {
        paceHardMaxFillMs_ = paceHardMinFillMs_ + 1;
    }
    paceHardStepMultiplier_ = get_u32(
        ini, "path.iso.virtual_to_physical_stream", "pace_hard_step_multiplier", paceHardStepMultiplier_);
    if (paceHardStepMultiplier_ == 0) paceHardStepMultiplier_ = 1;
    paceVelocityDeadbandMs_ = get_u32(
        ini, "path.iso.virtual_to_physical_stream", "pace_velocity_deadband_ms", paceVelocityDeadbandMs_);
    paceAdjustIntervalFrames_ = get_u32(
        ini, "path.iso.virtual_to_physical_stream", "pace_adjust_interval_frames", paceAdjustIntervalFrames_);
    if (paceAdjustIntervalFrames_ == 0) paceAdjustIntervalFrames_ = 1;
    {
        const std::string s = ini_get(ini, "path.iso.virtual_to_physical_stream", "pace_adjust_step_ms");
        if (!s.empty()) {
            char *end = nullptr;
            const double parsed = std::strtod(s.c_str(), &end);
            if (end != s.c_str() && parsed > 0.0 && parsed <= 5.0) {
                paceAdjustStepMs_ = parsed;
            }
        }
    }
    {
        const std::string s = ini_get(ini, "path.iso.virtual_to_physical_stream", "pace_min_ms");
        if (!s.empty()) {
            char *end = nullptr;
            const double parsed = std::strtod(s.c_str(), &end);
            if (end != s.c_str() && parsed > 0.0 && parsed <= 50.0) {
                paceMinMs_ = parsed;
            }
        }
    }
    {
        const std::string s = ini_get(ini, "path.iso.virtual_to_physical_stream", "pace_max_ms");
        if (!s.empty()) {
            char *end = nullptr;
            const double parsed = std::strtod(s.c_str(), &end);
            if (end != s.c_str() && parsed >= paceMinMs_ && parsed <= 50.0) {
                paceMaxMs_ = parsed;
            }
        }
    }
    opusSampleRate_ = get_u32(ini, "path.iso.virtual_to_physical_stream", "opus_sample_rate", opusSampleRate_);
    opusFrameSamples_ = static_cast<uint16_t>(get_u32(ini, "path.iso.virtual_to_physical_stream", "opus_frame_samples", opusFrameSamples_));
    sbcSampleRate_ = get_u32(ini, "path.iso.virtual_to_physical_stream", "sbc_sample_rate", sbcSampleRate_);
    sbcFrameSamples_ = static_cast<uint16_t>(get_u32(ini, "path.iso.virtual_to_physical_stream", "sbc_frame_samples", sbcFrameSamples_));
    stateBlockId_ = static_cast<uint8_t>(get_u32(ini, "path.iso.virtual_to_physical_stream", "state_block_id", stateBlockId_));
    timingBlockId_ = static_cast<uint8_t>(get_u32(ini, "path.iso.virtual_to_physical_stream", "timing_block_id", timingBlockId_));
    hapticBlockId_ = static_cast<uint8_t>(get_u32(ini, "path.iso.virtual_to_physical_stream", "haptic_block_id", hapticBlockId_));
    audioBlockId_ = static_cast<uint8_t>(get_u32(ini, "path.iso.virtual_to_physical_stream", "audio_block_id", audioBlockId_));
    opusPayloadBytes_ = static_cast<uint16_t>(get_u32(ini, "path.iso.virtual_to_physical_stream", "opus_payload_bytes", opusPayloadBytes_));
    sbcPayloadBytes_ = static_cast<uint16_t>(get_u32(ini, "path.iso.virtual_to_physical_stream", "sbc_payload_bytes", sbcPayloadBytes_));
    sbcBitpool_ = static_cast<uint8_t>(get_u32(ini, "path.iso.virtual_to_physical_stream", "sbc_bitpool", sbcBitpool_));
    sbcBlocks_ = static_cast<uint8_t>(get_u32(ini, "path.iso.virtual_to_physical_stream", "sbc_blocks", sbcBlocks_));
    sbcSubbands_ = static_cast<uint8_t>(get_u32(ini, "path.iso.virtual_to_physical_stream", "sbc_subbands", sbcSubbands_));
    sbcChannelMode_ = parse_sbc_channel_mode(
        ini_get(ini, "path.iso.virtual_to_physical_stream", "sbc_channel_mode"),
        sbcChannelMode_);
    sbcAllocation_ = parse_sbc_allocation(
        ini_get(ini, "path.iso.virtual_to_physical_stream", "sbc_allocation"),
        sbcAllocation_);
    logMappedInput_ = ini_get(ini, "debug", "log_mapped_input") == "true";
    streamOps_.clear();
    for (uint32_t i = 1; i <= 128; ++i) {
        const std::string op = ini_get(ini, "path.iso.virtual_to_physical_stream", "op." + std::to_string(i));
        if (!op.empty()) {
            streamOps_.push_back(op);
        }
    }

    const bool inputPassThrough =
        inputSourceReport_ == 0 && inputDestinationReport_ == 0;
    const bool outputPassThrough =
        outputSourceReport_ == 0 && outputDestinationReport_ == 0;
    const bool inputMapped =
        inputPassThrough || inputDestinationReport_ != 0;
    const bool outputMapped =
        outputPassThrough || (outputSourceReport_ != 0 && outputDestinationReport_ != 0);
    if (!inputMapped || inputDestinationLength_ == 0 ||
        !outputMapped || outputDestinationLength_ == 0) {
        if (error) *error = L"Map input/output report ids or lengths are invalid.";
        return false;
    }
    return true;
}

bool CtmMapRuntime::translate_controller_input(
    const uint8_t *source,
    size_t sourceLength,
    CTM_INPUT_REPORT *destination)
{
    const bool passThrough = inputSourceReport_ == 0 && inputDestinationReport_ == 0;
    const bool hasSourceReportFilter = inputSourceReport_ != 0;
    if (source == nullptr || destination == nullptr || sourceLength == 0 ||
        inputDestinationLength_ == 0 ||
        (!passThrough && hasSourceReportFilter && source[0] != inputSourceReport_) ||
        (!passThrough && inputDestinationLength_ > sizeof(destination->data))) {
        return false;
    }

    std::memset(destination, 0, sizeof(*destination));
    destination->endpoint_address = usbInputEndpoint_;
    if (passThrough) {
        const size_t copyLength = (std::min)(sourceLength, sizeof(destination->data));
        destination->length = static_cast<uint16_t>(copyLength);
        std::memcpy(destination->data, source, copyLength);
        return true;
    }

    destination->length = inputDestinationLength_;
    destination->data[0] = inputDestinationReport_;

    const uint8_t *opSource = source;
    size_t opSourceLength = sourceLength;
    if (inputSourceOffset_ != 0) {
        if (sourceLength <= inputSourceOffset_) {
            return false;
        }
        opSource = source + inputSourceOffset_;
        opSourceLength = sourceLength - inputSourceOffset_;
    }

    if (!inputOps_.empty()) {
        return execute_input_ops(opSource, opSourceLength, destination->data, inputDestinationLength_);
    }

    // Legacy single bulk copy path (kept for back-compat with maps that
    // still declare `copy = source[..] -> destination[..]`).
    if (opSourceLength <= inputCopy_.sourceEnd ||
        inputCopy_.destinationEnd >= inputDestinationLength_) {
        return false;
    }
    std::memcpy(
        destination->data + inputCopy_.destinationBegin,
        opSource + inputCopy_.sourceBegin,
        inputCopy_.sourceEnd - inputCopy_.sourceBegin + 1);
    return true;
}

bool CtmMapRuntime::execute_input_ops(
    const uint8_t *source,
    size_t sourceLength,
    uint8_t *destination,
    size_t destinationLength)
{
    for (const std::string &op : inputOps_) {
        if (!execute_byte_op(op, source, sourceLength, destination, destinationLength)) {
            return false;
        }
    }
    return true;
}

bool CtmMapRuntime::execute_byte_op(
    const std::string &op,
    const uint8_t *source,
    size_t sourceLength,
    uint8_t *destination,
    size_t destinationLength)
{
    const auto args = parse_op_args(op);
    auto get = [&](const char *key) -> std::string {
        auto it = args.find(key);
        return it == args.end() ? std::string() : it->second;
    };

    if (starts_with(op, "bytes.copy ")) {
        // bytes.copy source[A..B] -> destination[X..Y]
        // Remainder of the line after the primitive name is parsed for ranges
        // exactly like the legacy `copy = ...` shorthand.
        CopyRange range = {};
        const std::string remainder = op.substr(std::strlen("bytes.copy "));
        if (!parse_range_copy(remainder, &range) ||
            range.sourceEnd >= sourceLength ||
            range.destinationEnd >= destinationLength) {
            return false;
        }
        std::memcpy(
            destination + range.destinationBegin,
            source + range.sourceBegin,
            range.sourceEnd - range.sourceBegin + 1);
        return true;
    }

    if (starts_with(op, "bytes.set ")) {
        // bytes.set dst=X src=M           (copy one byte)
        // bytes.set dst=X value=0xVV      (literal byte)
        uint32_t dst = 0;
        if (!parse_u32(get("dst"), &dst) || dst >= destinationLength) {
            return false;
        }
        const std::string srcText = get("src");
        if (!srcText.empty()) {
            uint32_t srcIdx = 0;
            if (!parse_u32(srcText, &srcIdx) || srcIdx >= sourceLength) {
                return false;
            }
            destination[dst] = source[srcIdx];
            return true;
        }
        uint32_t literal = 0;
        if (!parse_u32(get("value"), &literal) || literal > 0xff) {
            return false;
        }
        destination[dst] = static_cast<uint8_t>(literal);
        return true;
    }

    if (starts_with(op, "counter.u8 ")) {
        // counter.u8 dst=X
        uint32_t dst = 0;
        if (!parse_u32(get("dst"), &dst) || dst >= destinationLength) {
            return false;
        }
        destination[dst] = mapCounter_;
        mapCounter_ = static_cast<uint8_t>(mapCounter_ + 1U);
        return true;
    }

    if (starts_with(op, "bits.merge ")) {
        // bits.merge dst=X src=M src_mask=0xMM src_shift=N
        //                        dst_mask=0xDD dst_shift=S
        // Extract masked bits from src, shift, then OR into the dst_mask
        // window of dst (clearing those bits first).
        uint32_t dst = 0;
        uint32_t srcIdx = 0;
        uint32_t srcMask = 0xff;
        uint32_t srcShift = 0;
        uint32_t dstMask = 0xff;
        uint32_t dstShift = 0;
        if (!parse_u32(get("dst"), &dst) || dst >= destinationLength ||
            !parse_u32(get("src"), &srcIdx) || srcIdx >= sourceLength ||
            !parse_u32(get("src_mask"), &srcMask) || srcMask > 0xff ||
            !parse_u32(get("src_shift"), &srcShift) || srcShift > 7 ||
            !parse_u32(get("dst_mask"), &dstMask) || dstMask > 0xff ||
            !parse_u32(get("dst_shift"), &dstShift) || dstShift > 7) {
            return false;
        }
        const uint8_t value = static_cast<uint8_t>(
            (((source[srcIdx] & srcMask) >> srcShift) << dstShift) & dstMask);
        destination[dst] = static_cast<uint8_t>(
            (destination[dst] & ~static_cast<uint8_t>(dstMask)) | value);
        return true;
    }

    if (starts_with(op, "hat8.to_dpad_bits ")) {
        // hat8.to_dpad_bits dst=X src=M
        // Converts a common 8-way hat ordinal to d-pad
        // bits in dst low nibble: up/down/left/right = 0x01/0x02/0x04/0x08.
        uint32_t dst = 0;
        uint32_t srcIdx = 0;
        if (!parse_u32(get("dst"), &dst) || dst >= destinationLength ||
            !parse_u32(get("src"), &srcIdx) || srcIdx >= sourceLength) {
            return false;
        }
        uint8_t bits = 0;
        switch (source[srcIdx]) {
        case 1: bits = 0x01; break;
        case 2: bits = 0x01 | 0x08; break;
        case 3: bits = 0x08; break;
        case 4: bits = 0x08 | 0x02; break;
        case 5: bits = 0x02; break;
        case 6: bits = 0x02 | 0x04; break;
        case 7: bits = 0x04; break;
        case 8: bits = 0x04 | 0x01; break;
        default: bits = 0; break;
        }
        destination[dst] = static_cast<uint8_t>((destination[dst] & 0xf0U) | bits);
        return true;
    }

    if (starts_with(op, "u16le.add ")) {
        // u16le.add dst=X src=M value=N
        // Reads a little-endian u16, adds N modulo 16 bits, writes LE u16.
        uint32_t dst = 0;
        uint32_t srcIdx = 0;
        int32_t add = 0;
        if (!parse_u32(get("dst"), &dst) || dst + 1 >= destinationLength ||
            !parse_u32(get("src"), &srcIdx) || srcIdx + 1 >= sourceLength ||
            !parse_i32(get("value"), &add)) {
            return false;
        }
        const uint16_t raw = static_cast<uint16_t>(
            static_cast<uint16_t>(source[srcIdx]) |
            (static_cast<uint16_t>(source[srcIdx + 1]) << 8));
        const uint16_t value = static_cast<uint16_t>((static_cast<int32_t>(raw) + add) & 0xffff);
        destination[dst] = static_cast<uint8_t>(value & 0xff);
        destination[dst + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
        return true;
    }

    if (starts_with(op, "u16le.rsub ")) {
        // u16le.rsub dst=X src=M value=N
        // Reads a little-endian u16, computes (N - u16) modulo 16 bits, writes
        // LE u16. Inverts an axis while recentering: with value=32767 a u16
        // stick [0..65535] maps to a signed-centered [+32767..-32768], i.e.
        // source minimum -> positive max. Used to flip HID Y-down sticks to the
        // GIP/XInput Y-up convention.
        uint32_t dst = 0;
        uint32_t srcIdx = 0;
        int32_t sub = 0;
        if (!parse_u32(get("dst"), &dst) || dst + 1 >= destinationLength ||
            !parse_u32(get("src"), &srcIdx) || srcIdx + 1 >= sourceLength ||
            !parse_i32(get("value"), &sub)) {
            return false;
        }
        const uint16_t raw = static_cast<uint16_t>(
            static_cast<uint16_t>(source[srcIdx]) |
            (static_cast<uint16_t>(source[srcIdx + 1]) << 8));
        const uint16_t value = static_cast<uint16_t>((sub - static_cast<int32_t>(raw)) & 0xffff);
        destination[dst] = static_cast<uint8_t>(value & 0xff);
        destination[dst + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
        return true;
    }

    if (starts_with(op, "u16le.to_u8 ")) {
        // u16le.to_u8 dst=X src=M shift=N
        // Reads a little-endian u16, right shifts, clamps to u8.
        uint32_t dst = 0;
        uint32_t srcIdx = 0;
        uint32_t shift = 0;
        if (!parse_u32(get("dst"), &dst) || dst >= destinationLength ||
            !parse_u32(get("src"), &srcIdx) || srcIdx + 1 >= sourceLength ||
            !parse_u32(get("shift"), &shift) || shift > 15) {
            return false;
        }
        const uint16_t raw = static_cast<uint16_t>(
            static_cast<uint16_t>(source[srcIdx]) |
            (static_cast<uint16_t>(source[srcIdx + 1]) << 8));
        const uint32_t shifted = static_cast<uint32_t>(raw) >> shift;
        destination[dst] = static_cast<uint8_t>((std::min<uint32_t>)(shifted, 0xff));
        return true;
    }

    return false;
}

bool CtmMapRuntime::translate_controller_output(
    const CTM_USB_EVENT &source,
    uint8_t *sequence,
    std::vector<uint8_t> *destination)
{
    const bool passThrough = outputSourceReport_ == 0 && outputDestinationReport_ == 0;
    if (sequence == nullptr || destination == nullptr ||
        source.event_type != CTM_USB_EVENT_HID_OUTPUT ||
        source.endpoint_address != usbOutputEndpoint_ ||
        source.length == 0 ||
        (!passThrough && source.data[0] != outputSourceReport_) ||
        outputDestinationLength_ == 0) {
        return false;
    }

    if (passThrough) {
        const size_t copyLength = (std::min<size_t>)(source.length, sizeof(source.data));
        destination->assign(source.data, source.data + copyLength);
        outputEffectsPayload_.assign(destination->begin(), destination->end());
        return true;
    }

    destination->assign(outputDestinationLength_, 0);
    (*destination)[0] = outputDestinationReport_;
    if (!outputOps_.empty()) {
        for (const std::string &op : outputOps_) {
            if (!execute_byte_op(op, source.data, source.length, destination->data(), outputDestinationLength_)) {
                return false;
            }
        }
        if (source.length > 1) {
            if (outputEffectsPayload_.size() < outputEffectsPayloadLength_) {
                outputEffectsPayload_.resize(outputEffectsPayloadLength_, 0);
            }
            const size_t copyLength = (std::min)(
                static_cast<size_t>(source.length - 1),
                outputEffectsPayload_.size());
            std::memcpy(outputEffectsPayload_.data(), source.data + 1, copyLength);
        } else if (outputEffectsPayloadLength_ == 0) {
            outputEffectsPayload_.clear();
        }
    } else {
        if (source.length <= outputCopy_.sourceEnd ||
            outputCopy_.destinationEnd >= outputDestinationLength_) {
            return false;
        }
        (*destination)[1] = static_cast<uint8_t>((*sequence & 0x0fU) << 4);
        *sequence = static_cast<uint8_t>((*sequence + 1U) & 0x0fU);
        if (outputDestinationLength_ > 2) {
            (*destination)[2] = outputTag_;
        }
        std::memcpy(
            destination->data() + outputCopy_.destinationBegin,
            source.data + outputCopy_.sourceBegin,
            outputCopy_.sourceEnd - outputCopy_.sourceBegin + 1);
        if (outputEffectsPayload_.size() < outputEffectsPayloadLength_) {
            outputEffectsPayload_.resize(outputEffectsPayloadLength_, 0);
        }
        const size_t copyLength = (std::min)(
            outputCopy_.sourceEnd - outputCopy_.sourceBegin + 1,
            outputEffectsPayload_.size());
        std::memcpy(outputEffectsPayload_.data(), source.data + outputCopy_.sourceBegin, copyLength);
    }
    if (outputCrcEnabled_) {
        sign_report(destination, outputCrcSeed_);
    }
    return true;
}

uint8_t CtmMapRuntime::scale_volume_to_byte(int16_t value, int16_t minimum, int16_t maximum, uint8_t maxByte)
{
    const int32_t range = static_cast<int32_t>(maximum) - minimum;
    if (range <= 0) {
        return 0;
    }
    int32_t normalized = static_cast<int32_t>(value) - minimum;
    normalized = (std::max)(int32_t{0}, (std::min)(normalized, range));
    return static_cast<uint8_t>((normalized * maxByte) / range);
}

uint8_t CtmMapRuntime::scale_volume_to_padded_byte(int16_t value, int16_t minimum, int16_t maximum)
{
    return static_cast<uint8_t>(0x80 + scale_volume_to_byte(value, minimum, maximum, 0x7f));
}

bool CtmMapRuntime::apply_usb_control_state(const CTM_USB_EVENT &event)
{
    if (event.event_type != CTM_USB_EVENT_CONTROL || event.length < 8) {
        return false;
    }

    const uint8_t *setup = event.data;
    const uint8_t request = setup[1];
    const uint16_t value = static_cast<uint16_t>(setup[2] | (setup[3] << 8));
    const uint16_t index = static_cast<uint16_t>(setup[4] | (setup[5] << 8));
    const uint16_t payloadLen = event.length > 8 ? static_cast<uint16_t>(event.length - 8) : 0;
    const uint8_t *payload = event.data + 8;
    const uint8_t control = static_cast<uint8_t>(value >> 8);
    const uint8_t channel = static_cast<uint8_t>(value & 0xff);
    const uint8_t unitId = static_cast<uint8_t>(index >> 8);

    if (request != 0x01 || channel != 0) {
        return false;
    }

    if (control == muteControl_ && payloadLen >= 1) {
        if (unitId == speakerFeatureUnit_) {
            speakerMute_ = payload[0];
            audioMute_ = speakerMute_ != 0 ? 0x00 : 0xff;
        } else if (unitId == headsetFeatureUnit_) {
            headsetMute_ = payload[0];
        } else {
            return false;
        }
    } else if (control == volumeControl_ && payloadLen >= 2) {
        if (unitId == speakerFeatureUnit_) {
            speakerVolumeRaw_ = static_cast<int16_t>(payload[0] | (payload[1] << 8));
            speakerVolume_ = scale_volume_to_padded_byte(
                speakerVolumeRaw_, speakerVolumeMin_, speakerVolumeMax_);
        } else if (unitId == headsetFeatureUnit_) {
            headsetVolumeRaw_ = static_cast<int16_t>(payload[0] | (payload[1] << 8));
            headsetVolume_ = scale_volume_to_padded_byte(
                headsetVolumeRaw_, headsetVolumeMin_, headsetVolumeMax_);
        } else {
            return false;
        }
    } else {
        return false;
    }
    return true;
}

bool CtmMapRuntime::handle_feature_set(const CTM_USB_EVENT &event) const
{
    if (event.event_type != CTM_USB_EVENT_FEATURE_SET) {
        return false;
    }

    if (featureSelectorReport_ != 0 &&
        event.report_id == featureSelectorReport_ &&
        event.length >= 2) {
        for (const FeaturePageRule &rule : featurePageRules_) {
            if (bytes_match_prefix(event.data, event.length, 1, rule.selector)) {
                return true;
            }
        }
    }

    if (feature_passthrough_enabled()) {
        return event.length != 0;
    }

    return find_usb_control_map_rule(event) != nullptr;
}

bool CtmMapRuntime::build_static_feature_response(
    const CTM_USB_EVENT &event,
    const std::vector<uint8_t> &lastFeatureSet,
    CTM_USB_RESPONSE *response) const
{
    if (response == nullptr || event.request_id == 0 || event.event_type != CTM_USB_EVENT_FEATURE_GET) {
        return false;
    }

    const FeaturePageRule *pageRule = find_feature_page_rule(event, lastFeatureSet);
    if (pageRule != nullptr && !pageRule->staticResponse.empty()) {
        std::memset(response, 0, sizeof(*response));
        response->request_id = event.request_id;
        response->status = CTM_USB_RESPONSE_SUCCESS;
        response->length = static_cast<uint16_t>(pageRule->staticResponse.size());
        std::memcpy(response->data, pageRule->staticResponse.data(), pageRule->staticResponse.size());
        return true;
    }

    if (is_selector_gated_report(event.report_id)) {
        return false;
    }

    const UsbControlMapRule *rule = find_usb_control_map_rule(event);
    if (rule == nullptr || rule->staticResponse.empty()) {
        return false;
    }

    std::memset(response, 0, sizeof(*response));
    response->request_id = event.request_id;
    response->status = CTM_USB_RESPONSE_SUCCESS;
    response->length = static_cast<uint16_t>(rule->staticResponse.size());
    std::memcpy(response->data, rule->staticResponse.data(), rule->staticResponse.size());
    return true;
}

bool CtmMapRuntime::build_control_response(const CTM_USB_EVENT &event, CTM_USB_RESPONSE *response)
{
    if (response == nullptr ||
        event.event_type != CTM_USB_EVENT_CONTROL ||
        event.request_id == 0 ||
        event.length < 8) {
        return false;
    }

    const uint8_t *setup = event.data;
    const uint8_t bmRequestType = setup[0];
    const uint8_t request = setup[1];
    const uint16_t value = static_cast<uint16_t>(setup[2] | (setup[3] << 8));
    const uint16_t index = static_cast<uint16_t>(setup[4] | (setup[5] << 8));
    const uint16_t requestedLength = static_cast<uint16_t>(setup[6] | (setup[7] << 8));
    const uint8_t requestType = static_cast<uint8_t>((bmRequestType >> 5) & 0x03);
    const uint8_t recipient = static_cast<uint8_t>(bmRequestType & 0x1f);
    const uint8_t control = static_cast<uint8_t>(value >> 8);
    const uint8_t channel = static_cast<uint8_t>(value & 0xff);
    const uint16_t payloadLen = event.length > 8 ? static_cast<uint16_t>(event.length - 8) : 0;
    const uint8_t *payload = event.data + 8;

    if (requestType != 1) {
        return false;
    }

    constexpr uint8_t kSetCur = 0x01;
    constexpr uint8_t kGetCur = 0x81;
    constexpr uint8_t kGetMin = 0x82;
    constexpr uint8_t kGetMax = 0x83;
    constexpr uint8_t kGetRes = 0x84;

    if (recipient == 1) {
        const uint8_t unitId = static_cast<uint8_t>(index >> 8);
        const uint8_t interfaceNumber = static_cast<uint8_t>(index & 0xff);
        if (interfaceNumber != audioControlInterface_ || channel != 0) {
            return false;
        }

        uint8_t *mute = nullptr;
        int16_t *volume = nullptr;
        int16_t volumeMin = 0;
        int16_t volumeMax = 0;
        int16_t volumeRes = 0;
        if (unitId == speakerFeatureUnit_) {
            mute = &speakerMute_;
            volume = &speakerVolumeRaw_;
            volumeMin = speakerVolumeMin_;
            volumeMax = speakerVolumeMax_;
            volumeRes = speakerVolumeRes_;
        } else if (unitId == headsetFeatureUnit_) {
            mute = &headsetMute_;
            volume = &headsetVolumeRaw_;
            volumeMin = headsetVolumeMin_;
            volumeMax = headsetVolumeMax_;
            volumeRes = headsetVolumeRes_;
        } else {
            return false;
        }

        if (request == kSetCur) {
            if (control == muteControl_ && payloadLen >= 1) {
                *mute = payload[0];
                if (unitId == speakerFeatureUnit_) {
                    audioMute_ = speakerMute_ != 0 ? 0x00 : 0xff;
                }
            } else if (control == volumeControl_ && payloadLen >= 2) {
                *volume = read_s16_le(payload);
                if (unitId == speakerFeatureUnit_) {
                    speakerVolume_ = scale_volume_to_padded_byte(
                        speakerVolumeRaw_, speakerVolumeMin_, speakerVolumeMax_);
                } else if (unitId == headsetFeatureUnit_) {
                    headsetVolume_ = scale_volume_to_padded_byte(
                        headsetVolumeRaw_, headsetVolumeMin_, headsetVolumeMax_);
                }
            } else {
                return false;
            }
            init_usb_response(response, event, CTM_USB_RESPONSE_SUCCESS);
            return true;
        }

        if (control == muteControl_ && request == kGetCur) {
            bool ok = complete_u8_response(response, event, *mute, requestedLength);
            return ok;
        }

        if (control == volumeControl_) {
            int16_t out = 0;
            if (request == kGetCur) {
                out = *volume;
            } else if (request == kGetMin) {
                out = volumeMin;
            } else if (request == kGetMax) {
                out = volumeMax;
            } else if (request == kGetRes) {
                out = volumeRes;
            } else {
                return false;
            }
            bool ok = complete_s16_response(response, event, out, requestedLength);
            return ok;
        }
        return false;
    }

    if (recipient == 2) {
        const uint8_t endpoint = static_cast<uint8_t>(index & 0xff);
        if (control != sampleFreqControl_) {
            return false;
        }

        uint32_t *rate = nullptr;
        uint32_t minRate = 0;
        uint32_t maxRate = 0;
        uint32_t resRate = 0;
        if (endpoint == usbIsoOutEndpoint_) {
            rate = &isoOutSampleRate_;
            minRate = isoOutSampleRateMin_;
            maxRate = isoOutSampleRateMax_;
            resRate = isoOutSampleRateRes_;
        } else if (endpoint == usbIsoInEndpoint_) {
            rate = &isoInSampleRate_;
            minRate = isoInSampleRateMin_;
            maxRate = isoInSampleRateMax_;
            resRate = isoInSampleRateRes_;
        } else {
            return false;
        }

        if (request == kSetCur) {
            if (payloadLen < 3) {
                return false;
            }
            *rate = static_cast<uint32_t>(payload[0]) |
                (static_cast<uint32_t>(payload[1]) << 8) |
                (static_cast<uint32_t>(payload[2]) << 16);
            init_usb_response(response, event, CTM_USB_RESPONSE_SUCCESS);
            return true;
        }

        uint32_t out = 0;
        if (request == kGetCur) {
            out = *rate;
        } else if (request == kGetMin) {
            out = minRate;
        } else if (request == kGetMax) {
            out = maxRate;
        } else if (request == kGetRes) {
            out = resRate;
        } else {
            return false;
        }

        bool ok = complete_u24_response(response, event, out, requestedLength);
        return ok;
    }

    return false;
}

bool CtmMapRuntime::build_virtual_input_reports(
    const CTM_USB_EVENT &event,
    std::vector<CTM_INPUT_REPORT> *reports)
{
    if (reports == nullptr) {
        return false;
    }
    reports->clear();

    for (const VirtualInputRule &rule : virtualInputRules_) {
        if (rule.matchEvent != event.event_type) {
            continue;
        }
        if (rule.hasMatchEndpoint && rule.matchEndpoint != event.endpoint_address) {
            continue;
        }
        if (!rule.matchPrefix.empty() &&
            !bytes_match_prefix(event.data, event.length, 0, rule.matchPrefix)) {
            continue;
        }
        for (const std::vector<uint8_t> &packet : rule.packets) {
            CTM_INPUT_REPORT report = {};
            report.endpoint_address = rule.destinationEndpoint;
            report.length = static_cast<uint16_t>(packet.size());
            if (report.length > sizeof(report.data)) {
                reports->clear();
                return false;
            }
            if (!packet.empty()) {
                std::memcpy(report.data, packet.data(), packet.size());
            }
            reports->push_back(report);
        }
    }
    return !reports->empty();
}

bool CtmMapRuntime::ensure_opus_encoder(uint32_t sampleRate)
{
    if (sampleRate == 0) {
        return false;
    }
    if (opus_ != nullptr && opusEncoderSampleRate_ == sampleRate) {
        return true;
    }
    if (opus_ != nullptr) {
        opus_encoder_destroy(opus_);
        opus_ = nullptr;
        opusEncoderSampleRate_ = 0;
    }

    int err = OPUS_OK;
    opus_ = opus_encoder_create(static_cast<opus_int32>(sampleRate), 2, OPUS_APPLICATION_AUDIO, &err);
    if (opus_ == nullptr || err != OPUS_OK) {
        std::cout << "map runtime opus encoder failed err=" << err << std::endl;
        return false;
    }

    (void)opus_encoder_ctl(opus_, OPUS_SET_BITRATE(160000));
    (void)opus_encoder_ctl(opus_, OPUS_SET_VBR(0));
    (void)opus_encoder_ctl(opus_, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    (void)opus_encoder_ctl(opus_, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
    opusEncoderSampleRate_ = sampleRate;
    return true;
}

bool CtmMapRuntime::build_stream_output_report(
    const CTM_USB_EVENT &source,
    std::vector<uint8_t> *destination)
{
    if (destination == nullptr) {
        return false;
    }
    isoFrames_++;
    return execute_stream_ops(source, destination);
}

bool CtmMapRuntime::select_pcm_channels(
    const CTM_USB_EVENT &source,
    const std::vector<unsigned int> &channels,
    std::vector<int16_t> *destination) const
{
    if (source.event_type != CTM_USB_EVENT_ISO_OUT ||
        source.endpoint_address != usbIsoOutEndpoint_ ||
        source.length < isoExpectedLength_ ||
        channels.empty() ||
        destination == nullptr) {
        return false;
    }
    for (unsigned int channel : channels) {
        if (channel >= isoChannels_) {
            return false;
        }
    }

    destination->assign(static_cast<size_t>(isoFrameSamples_) * channels.size(), 0);
    const size_t bytesPerFrame = static_cast<size_t>(isoChannels_) * sizeof(int16_t);
    for (size_t sample = 0; sample < isoFrameSamples_; ++sample) {
        const uint8_t *frame = source.data + sample * bytesPerFrame;
        for (size_t outChannel = 0; outChannel < channels.size(); ++outChannel) {
            destination->at(sample * channels.size() + outChannel) =
                read_s16_le(frame + channels[outChannel] * sizeof(int16_t));
        }
    }
    return true;
}

bool CtmMapRuntime::encode_opus(
    const std::vector<int16_t> &source,
    uint32_t sampleRate,
    size_t frameSamples,
    size_t bytes,
    std::vector<uint8_t> *destination)
{
    if (destination == nullptr ||
        source.size() < frameSamples * 2 ||
        bytes == 0 ||
        !ensure_opus_encoder(sampleRate)) {
        return false;
    }
    destination->assign(bytes, 0);
    int opusLen = opus_encode(
        opus_,
        source.data(),
        static_cast<int>(frameSamples),
        destination->data(),
        static_cast<opus_int32>(destination->size()));
    if (opusLen <= 0) {
        return false;
    }
    if (opusLen < static_cast<int>(destination->size()) &&
        opus_packet_pad(destination->data(), opusLen, static_cast<opus_int32>(destination->size())) == OPUS_OK) {
        opusLen = static_cast<int>(destination->size());
    }
    destination->resize(std::min<size_t>(bytes, static_cast<size_t>(opusLen)));
    return true;
}

bool CtmMapRuntime::ensure_sbc_encoder(
    uint32_t sampleRate,
    uint8_t bitpool,
    uint8_t blocks,
    uint8_t subbands,
    uint8_t channelMode,
    uint8_t allocation)
{
    if (sampleRate == 0 ||
        bitpool == 0 ||
        (blocks != 4 && blocks != 8 && blocks != 12 && blocks != 16) ||
        (subbands != 4 && subbands != 8) ||
        sbc_frequency_code(sampleRate) == 0xff ||
        channelMode > SBC_MODE_JOINT_STEREO ||
        allocation > SBC_AM_SNR) {
        return false;
    }
    if (sbc_ != nullptr &&
        sbcEncoderSampleRate_ == sampleRate &&
        sbcEncoderBitpool_ == bitpool &&
        sbcEncoderBlocks_ == blocks &&
        sbcEncoderSubbands_ == subbands &&
        sbcEncoderChannelMode_ == channelMode &&
        sbcEncoderAllocation_ == allocation) {
        return true;
    }

    if (sbcPacket_ != nullptr) {
        av_packet_free(&sbcPacket_);
    }
    if (sbcFrame_ != nullptr) {
        av_frame_free(&sbcFrame_);
    }
    if (sbc_ != nullptr) {
        avcodec_free_context(&sbc_);
    }
    sbcEncoderSampleRate_ = 0;

    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_SBC);
    if (codec == nullptr) {
        std::cout << "map runtime sbc encoder not available" << std::endl;
        return false;
    }
    sbc_ = avcodec_alloc_context3(codec);
    if (sbc_ == nullptr) {
        return false;
    }

    sbc_->sample_rate = static_cast<int>(sampleRate);
    sbc_->sample_fmt = AV_SAMPLE_FMT_S16;
    sbc_->bit_rate = 225000;
    sbc_->global_quality = static_cast<int>(bitpool) * FF_QP2LAMBDA;
    av_channel_layout_default(&sbc_->ch_layout, 2);

    const int openRet = avcodec_open2(sbc_, codec, nullptr);
    if (openRet < 0) {
        std::cout << "map runtime sbc encoder failed err=" << openRet << std::endl;
        avcodec_free_context(&sbc_);
        return false;
    }

    auto *priv = reinterpret_cast<FfmpegSbcPrivPrefix *>(sbc_->priv_data);
    priv->frame.frequency = sbc_frequency_code(sampleRate);
    priv->frame.blocks = blocks;
    priv->frame.mode = static_cast<decltype(priv->frame.mode)>(channelMode);
    priv->frame.channels = 2;
    priv->frame.allocation = static_cast<decltype(priv->frame.allocation)>(allocation);
    priv->frame.subbands = subbands;
    priv->frame.bitpool = bitpool;
    priv->frame.codesize = static_cast<uint16_t>(subbands * blocks * 2 * sizeof(int16_t));
    sbc_->frame_size = subbands * blocks;

    sbcFrame_ = av_frame_alloc();
    sbcPacket_ = av_packet_alloc();
    if (sbcFrame_ == nullptr || sbcPacket_ == nullptr) {
        if (sbcPacket_ != nullptr) av_packet_free(&sbcPacket_);
        if (sbcFrame_ != nullptr) av_frame_free(&sbcFrame_);
        avcodec_free_context(&sbc_);
        return false;
    }

    sbcEncoderSampleRate_ = sampleRate;
    sbcEncoderBitpool_ = bitpool;
    sbcEncoderBlocks_ = blocks;
    sbcEncoderSubbands_ = subbands;
    sbcEncoderChannelMode_ = channelMode;
    sbcEncoderAllocation_ = allocation;
    return true;
}

bool CtmMapRuntime::encode_sbc(
    const std::vector<int16_t> &source,
    uint32_t sampleRate,
    size_t frameSamples,
    size_t bytes,
    uint8_t bitpool,
    uint8_t blocks,
    uint8_t subbands,
    uint8_t channelMode,
    uint8_t allocation,
    std::vector<uint8_t> *destination)
{
    const size_t sbcSamplesPerFrame = static_cast<size_t>(blocks) * subbands;
    if (destination == nullptr ||
        source.size() < frameSamples * 2 ||
        frameSamples == 0 ||
        sbcSamplesPerFrame == 0 ||
        (frameSamples % sbcSamplesPerFrame) != 0 ||
        bytes == 0 ||
        !ensure_sbc_encoder(sampleRate, bitpool, blocks, subbands, channelMode, allocation)) {
        return false;
    }

    destination->clear();
    destination->reserve(bytes);
    for (size_t frameOffset = 0; frameOffset < frameSamples; frameOffset += sbcSamplesPerFrame) {
        av_frame_unref(sbcFrame_);
        sbcFrame_->format = AV_SAMPLE_FMT_S16;
        sbcFrame_->sample_rate = static_cast<int>(sampleRate);
        sbcFrame_->nb_samples = static_cast<int>(sbcSamplesPerFrame);
        av_channel_layout_default(&sbcFrame_->ch_layout, 2);
        if (av_frame_get_buffer(sbcFrame_, 0) < 0) {
            return false;
        }

        const size_t sampleOffset = frameOffset * 2;
        const size_t byteCount = sbcSamplesPerFrame * 2 * sizeof(int16_t);
        std::memcpy(sbcFrame_->data[0], source.data() + sampleOffset, byteCount);

        int ret = avcodec_send_frame(sbc_, sbcFrame_);
        if (ret < 0) {
            return false;
        }
        for (;;) {
            ret = avcodec_receive_packet(sbc_, sbcPacket_);
            if (ret == AVERROR(EAGAIN)) {
                break;
            }
            if (ret < 0) {
                return false;
            }
            if (sbcPacket_->size < 0 ||
                destination->size() + static_cast<size_t>(sbcPacket_->size) > bytes) {
                av_packet_unref(sbcPacket_);
                return false;
            }
            destination->insert(
                destination->end(),
                sbcPacket_->data,
                sbcPacket_->data + sbcPacket_->size);
            av_packet_unref(sbcPacket_);
        }
    }

    return !destination->empty();
}

bool CtmMapRuntime::resample_linear(
    const std::vector<int16_t> &source,
    unsigned int channels,
    unsigned int fromRate,
    unsigned int toRate,
    size_t frameSamples,
    std::vector<int16_t> *destination) const
{
    if (destination == nullptr ||
        source.empty() ||
        channels == 0 ||
        fromRate == 0 ||
        toRate == 0 ||
        frameSamples == 0 ||
        (source.size() % channels) != 0) {
        return false;
    }

    const size_t sourceFrames = source.size() / channels;
    destination->assign(frameSamples * channels, 0);
    for (size_t outFrame = 0; outFrame < frameSamples; ++outFrame) {
        const uint64_t scaled = static_cast<uint64_t>(outFrame) * static_cast<uint64_t>(toRate);
        size_t src0 = static_cast<size_t>(scaled / static_cast<uint64_t>(fromRate));
        if (src0 >= sourceFrames) {
            src0 = sourceFrames - 1;
        }
        const uint64_t rem = scaled - static_cast<uint64_t>(src0) * static_cast<uint64_t>(fromRate);
        const double frac = static_cast<double>(rem) / static_cast<double>(fromRate);
        const size_t src1 = (std::min)(src0 + 1, sourceFrames - 1);
        for (unsigned int ch = 0; ch < channels; ++ch) {
            const int32_t a = source[src0 * channels + ch];
            const int32_t b = source[src1 * channels + ch];
            const int32_t v = static_cast<int32_t>(a + frac * (b - a));
            (*destination)[outFrame * channels + ch] =
                static_cast<int16_t>((std::max)(-32768, (std::min)(32767, v)));
        }
    }
    return true;
}

bool CtmMapRuntime::downsample_average(
    const std::vector<int16_t> &source,
    unsigned int fromRate,
    unsigned int toRate,
    std::vector<int16_t> *destination) const
{
    if (destination == nullptr || fromRate == 0 || toRate == 0 || fromRate < toRate || source.empty()) {
        return false;
    }
    const unsigned int factor = fromRate / toRate;
    if (factor == 0 || (fromRate % toRate) != 0 || (source.size() % (factor * 2)) != 0) {
        return false;
    }

    const size_t outSamples = source.size() / (factor * 2);
    destination->assign(outSamples * 2, 0);
    for (size_t sample = 0; sample < outSamples; ++sample) {
        int32_t left = 0;
        int32_t right = 0;
        for (size_t src = 0; src < factor; ++src) {
            const size_t index = ((sample * factor) + src) * 2;
            left += source[index];
            right += source[index + 1];
        }
        (*destination)[sample * 2] = static_cast<int16_t>(left / static_cast<int32_t>(factor));
        (*destination)[sample * 2 + 1] = static_cast<int16_t>(right / static_cast<int32_t>(factor));
    }
    return true;
}

bool CtmMapRuntime::convert_s16_to_s8(const std::vector<int16_t> &source, std::vector<int8_t> *destination) const
{
    if (destination == nullptr) {
        return false;
    }
    destination->assign(source.size(), 0);
    for (size_t i = 0; i < source.size(); ++i) {
        (*destination)[i] = s16_to_s8(source[i]);
    }
    return true;
}

bool CtmMapRuntime::append_block(std::vector<uint8_t> *destination, const std::vector<uint8_t> &block)
{
    if (destination == nullptr || destination->size() < 4) {
        return false;
    }
    const size_t limit = destination->size() - 4;
    if (streamAppendOffset_ + block.size() > limit) {
        return false;
    }
    std::memcpy(destination->data() + streamAppendOffset_, block.data(), block.size());
    streamAppendOffset_ += block.size();
    return true;
}

void CtmMapRuntime::sign_report(std::vector<uint8_t> *report, uint8_t seed) const
{
    if (report == nullptr || report->size() < 8) {
        return;
    }

    uint32_t crc = crc32_step(0xffffffffU, &seed, 1);
    crc = ~crc32_step(crc, report->data(), report->size() - 4);
    (*report)[report->size() - 4] = static_cast<uint8_t>(crc & 0xff);
    (*report)[report->size() - 3] = static_cast<uint8_t>((crc >> 8) & 0xff);
    (*report)[report->size() - 2] = static_cast<uint8_t>((crc >> 16) & 0xff);
    (*report)[report->size() - 1] = static_cast<uint8_t>((crc >> 24) & 0xff);
}

bool CtmMapRuntime::apply_physical_feature_crc(PhysicalFeatureAction *action) const
{
    if (action == nullptr || !action->crc32Tail) {
        return action != nullptr;
    }
    if (action->report == 0 ||
        action->length < 8 ||
        1 + action->payload.size() > action->length) {
        return false;
    }

    std::vector<uint8_t> report(action->length, 0);
    report[0] = action->report;
    if (!action->payload.empty()) {
        std::memcpy(report.data() + 1, action->payload.data(), action->payload.size());
    }
    sign_report(&report, action->crc32Seed);
    action->payload.assign(report.begin() + 1, report.end());
    return true;
}

bool CtmMapRuntime::execute_stream_ops(const CTM_USB_EVENT &source, std::vector<uint8_t> *destination)
{
    if (source.event_type != CTM_USB_EVENT_ISO_OUT ||
        source.endpoint_address != usbIsoOutEndpoint_ ||
        source.length < isoExpectedLength_ ||
        destination == nullptr ||
        streamOps_.empty()) {
        return false;
    }

    for (const std::string &op : streamOps_) {
        if (!execute_stream_op(op, source, destination)) {
            return false;
        }
    }
    streamReports_++;
    return true;
}

bool CtmMapRuntime::execute_stream_op(
    const std::string &op,
    const CTM_USB_EVENT &source,
    std::vector<uint8_t> *destination)
{
    const auto args = parse_op_args(op);
    auto get = [&](const char *key) -> std::string {
        auto it = args.find(key);
        return it == args.end() ? std::string() : it->second;
    };
    auto buffer_u8 = [&](const std::string &name) -> std::vector<uint8_t> * {
        if (name == "physical_report") return destination;
        if (name == "audio_block") return &audioBlock_;
        if (name == "haptic_block") return &hapticBlock_;
        if (name == "state_block") return &stateBlock_;
        if (name == "timing_block") return &timingBlock_;
        if (name == "opus_payload") return &opusPayload_;
        if (name == "sbc_payload") return &sbcPayload_;
        if (name == "output_effects_payload") return &outputEffectsPayload_;
        return nullptr;
    };
    auto buffer_pcm = [&](const std::string &name) -> std::vector<int16_t> * {
        if (name == "speaker_pcm") return &speakerPcm_;
        if (name == "speaker_pcm_resampled") return &speakerPcmResampled_;
        if (name == "haptic_pcm") return &hapticPcm_;
        if (name == "haptic_pcm_3k2") return &hapticPcmDownsampled_;
        return nullptr;
    };
    auto byte_source = [&](const std::string &sourceName, uint8_t *value) -> bool {
        if (value == nullptr) {
            return false;
        }
        if (sourceName == "len(opus_payload)") {
            *value = static_cast<uint8_t>(std::min<size_t>(0xff, opusPayload_.size()));
            return true;
        }
        if (sourceName == "len(sbc_payload)") {
            *value = static_cast<uint8_t>(std::min<size_t>(0xff, sbcPayload_.size()));
            return true;
        }
        if (starts_with(sourceName, "payload_len(") && sourceName.back() == ')') {
            const std::string bufferName = sourceName.substr(12, sourceName.size() - 13);
            std::vector<uint8_t> *buffer = buffer_u8(bufferName);
            if (buffer == nullptr || buffer->size() < 2) {
                return false;
            }
            *value = static_cast<uint8_t>(std::min<size_t>(0xff, buffer->size() - 2));
            return true;
        }
        if (sourceName == "headset_volume") *value = headsetVolume_;
        else if (sourceName == "speaker_volume") *value = speakerVolume_;
        else if (sourceName == "audio_mute") *value = audioMute_;
        else if (sourceName == "audio_latency") *value = audioLatency_;
        else if (sourceName == "state_block_id") *value = stateBlockId_;
        else if (sourceName == "timing_block_id") *value = timingBlockId_;
        else if (sourceName == "haptic_block_id") *value = hapticBlockId_;
        else if (sourceName == "audio_block_id") *value = audioBlockId_;
        else if (sourceName == "audio_sequence++") *value = audioSequence_++;
        else return false;
        return true;
    };

    if (starts_with(op, "pcm.select_channels ")) {
        const std::vector<unsigned int> channels = parse_index_list(get("channels"));
        const std::string out = get("out");
        if (out == "speaker_pcm") {
            return select_pcm_channels(source, channels, &speakerPcm_);
        }
        if (out == "haptic_pcm") {
            return select_pcm_channels(source, channels, &hapticPcm_);
        }
        return false;
    }

    if (starts_with(op, "codec.opus_encode ")) {
        uint32_t bytes = 0;
        uint32_t sampleRate = opusSampleRate_;
        uint32_t frameSamples = opusFrameSamples_;
        const std::vector<int16_t> *pcm = buffer_pcm(first_positional_arg(op, "codec.opus_encode"));
        const std::string sampleRateArg = get("sample_rate");
        const std::string frameSamplesArg = get("frame_samples");
        if (!sampleRateArg.empty() && !parse_u32(sampleRateArg, &sampleRate)) {
            return false;
        }
        if (!frameSamplesArg.empty() && !parse_u32(frameSamplesArg, &frameSamples)) {
            return false;
        }
        if (pcm == nullptr ||
            get("out") != "opus_payload" ||
            !parse_u32(get("bytes"), &bytes)) {
            return false;
        }
        return encode_opus(*pcm, sampleRate, frameSamples, bytes, &opusPayload_);
    }

    if (starts_with(op, "codec.sbc_encode ")) {
        uint32_t bytes = sbcPayloadBytes_;
        uint32_t sampleRate = sbcSampleRate_;
        uint32_t frameSamples = sbcFrameSamples_;
        uint32_t bitpool = sbcBitpool_;
        uint32_t blocks = sbcBlocks_;
        uint32_t subbands = sbcSubbands_;
        uint8_t channelMode = sbcChannelMode_;
        uint8_t allocation = sbcAllocation_;
        const std::vector<int16_t> *pcm = buffer_pcm(first_positional_arg(op, "codec.sbc_encode"));
        const std::string sampleRateArg = get("sample_rate");
        const std::string frameSamplesArg = get("frame_samples");
        if (!sampleRateArg.empty() && !parse_u32(sampleRateArg, &sampleRate)) {
            return false;
        }
        if (!frameSamplesArg.empty() && !parse_u32(frameSamplesArg, &frameSamples)) {
            return false;
        }
        const std::string bitpoolArg = get("bitpool");
        const std::string blocksArg = get("blocks");
        const std::string subbandsArg = get("subbands");
        if (!bitpoolArg.empty() && !parse_u32(bitpoolArg, &bitpool)) {
            return false;
        }
        if (!blocksArg.empty() && !parse_u32(blocksArg, &blocks)) {
            return false;
        }
        if (!subbandsArg.empty() && !parse_u32(subbandsArg, &subbands)) {
            return false;
        }
        const std::string channelModeArg = get("channel_mode");
        const std::string allocationArg = get("allocation");
        if (!channelModeArg.empty()) {
            channelMode = parse_sbc_channel_mode(channelModeArg, channelMode);
        }
        if (!allocationArg.empty()) {
            allocation = parse_sbc_allocation(allocationArg, allocation);
        }
        if (pcm == nullptr ||
            get("out") != "sbc_payload" ||
            !parse_u32(get("bytes"), &bytes) ||
            bitpool > 255 ||
            blocks > 255 ||
            subbands > 255) {
            return false;
        }
        return encode_sbc(
            *pcm,
            sampleRate,
            frameSamples,
            bytes,
            static_cast<uint8_t>(bitpool),
            static_cast<uint8_t>(blocks),
            static_cast<uint8_t>(subbands),
            channelMode,
            allocation,
            &sbcPayload_);
    }

    if (starts_with(op, "pcm.resample ")) {
        uint32_t fromRate = 0;
        uint32_t toRate = 0;
        uint32_t channels = 2;
        uint32_t frameSamples = opusFrameSamples_;
        std::vector<int16_t> *pcm = buffer_pcm(first_positional_arg(op, "pcm.resample"));
        std::vector<int16_t> *out = buffer_pcm(get("out"));
        if (pcm == nullptr ||
            out == nullptr ||
            !parse_u32(get("from"), &fromRate) ||
            !parse_u32(get("to"), &toRate) ||
            !parse_u32(get("channels"), &channels)) {
            return false;
        }
        const std::string frameSamplesArg = get("frame_samples");
        if (!frameSamplesArg.empty() && !parse_u32(frameSamplesArg, &frameSamples)) {
            return false;
        }
        return resample_linear(*pcm, channels, fromRate, toRate, frameSamples, out);
    }

    if (starts_with(op, "pcm.downsample ")) {
        uint32_t fromRate = 0;
        uint32_t toRate = 0;
        if (get("out") != "haptic_pcm_3k2" ||
            !parse_u32(get("from"), &fromRate) ||
            !parse_u32(get("to"), &toRate) ||
            get("method") != "average") {
            return false;
        }
        return downsample_average(hapticPcm_, fromRate, toRate, &hapticPcmDownsampled_);
    }

    if (starts_with(op, "pcm.s16_to_s8 ")) {
        if (get("out") != "haptic_s8") {
            return false;
        }
        return convert_s16_to_s8(hapticPcmDownsampled_, &hapticS8_);
    }

    if (starts_with(op, "bytes.clear ")) {
        std::istringstream stream(op);
        std::string primitive;
        std::string name;
        stream >> primitive >> name;
        std::vector<uint8_t> *buffer = buffer_u8(name);
        if (buffer == nullptr) {
            return false;
        }
        std::fill(buffer->begin(), buffer->end(), uint8_t{0});
        return true;
    }

    if (starts_with(op, "bytes.set ")) {
        std::istringstream stream(op);
        std::string primitive;
        std::string name;
        stream >> primitive >> name;
        std::vector<uint8_t> *buffer = buffer_u8(name);
        uint32_t offset = 0;
        if (buffer == nullptr || !parse_u32(get("offset"), &offset) || offset >= buffer->size()) {
            return false;
        }
        uint8_t value = 0;
        const std::string sourceName = get("source");
        if (!sourceName.empty()) {
            if (!byte_source(sourceName, &value)) {
                return false;
            }
        } else {
            uint32_t parsed = 0;
            if (!parse_u32(get("value"), &parsed) || parsed > 0xff) {
                return false;
            }
            value = static_cast<uint8_t>(parsed);
        }
        (*buffer)[offset] = value;
        return true;
    }

    if (starts_with(op, "word.set_le ")) {
        std::istringstream stream(op);
        std::string primitive;
        std::string name;
        stream >> primitive >> name;
        std::vector<uint8_t> *buffer = buffer_u8(name);
        uint32_t offset = 0;
        if (buffer == nullptr || !parse_u32(get("offset"), &offset) || offset + 1 >= buffer->size()) {
            return false;
        }

        uint16_t value = 0;
        const std::string sourceName = get("source");
        if (sourceName == "audio_frame_counter") {
            value = audioFrameCounter_;
            uint32_t step = 0;
            const std::string stepText = get("step");
            if (!stepText.empty() && !parse_u32(stepText, &step)) {
                return false;
            }
            audioFrameCounter_ = static_cast<uint16_t>(audioFrameCounter_ + step);
        } else {
            uint32_t parsed = 0;
            if (!parse_u32(get("value"), &parsed) || parsed > 0xffff) {
                return false;
            }
            value = static_cast<uint16_t>(parsed);
        }

        (*buffer)[offset] = static_cast<uint8_t>(value & 0xff);
        (*buffer)[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
        return true;
    }

    if (starts_with(op, "bytes.copy ")) {
        std::istringstream stream(op);
        std::string primitive;
        std::string sourceName;
        stream >> primitive >> sourceName;
        size_t srcBegin = 0;
        size_t srcEnd = 0;
        uint32_t dst = 0;
        std::vector<uint8_t> *target = buffer_u8(get("target"));
        if (target == nullptr ||
            !parse_range(get("src"), &srcBegin, &srcEnd) ||
            !parse_u32(get("dst"), &dst)) {
            return false;
        }
        std::vector<uint8_t> *sourceBuffer = buffer_u8(sourceName);
        if (sourceBuffer != nullptr) {
            const size_t count = srcEnd - srcBegin + 1;
            if (srcEnd >= sourceBuffer->size() ||
                dst > target->size() ||
                count > target->size() - dst) {
                return false;
            }
            std::memcpy(target->data() + dst, sourceBuffer->data() + srcBegin, count);
            return true;
        }
        if (sourceName == "haptic_s8") {
            const size_t count = srcEnd - srcBegin + 1;
            if (srcEnd >= hapticS8_.size() ||
                dst > target->size() ||
                count > target->size() - dst) {
                return false;
            }
            for (size_t i = 0; i < count; ++i) {
                (*target)[dst + i] = static_cast<uint8_t>(hapticS8_[srcBegin + i]);
            }
            return true;
        }
        return false;
    }

    if (op == "physical_report.clear") {
        destination->assign(streamReportLength_, 0);
        streamAppendOffset_ = 2;
        return true;
    }

    if (starts_with(op, "physical_report.header ")) {
        uint32_t report = 0;
        if (destination == nullptr || destination->size() < 2 || !parse_u32(get("report"), &report) || report > 0xff) {
            return false;
        }
        streamAppendOffset_ = 2;
        (*destination)[0] = static_cast<uint8_t>(report);
        if (get("seq") == "high_nibble") {
            (*destination)[1] = static_cast<uint8_t>((reportSequence_ & 0x0fU) << 4);
            reportSequence_ = static_cast<uint8_t>((reportSequence_ + 1U) & 0x0fU);
        }
        return true;
    }

    if (starts_with(op, "block.append")) {
        std::istringstream stream(op);
        std::string primitive;
        std::string name;
        stream >> primitive >> name;
        const bool ifPresent = primitive == "block.append_if_present";
        const std::vector<uint8_t> *block = nullptr;
        if (name == "state_block") block = &stateBlock_;
        else if (name == "timing_block") block = &timingBlock_;
        else if (name == "audio_block") block = &audioBlock_;
        else if (name == "haptic_block") block = &hapticBlock_;
        else return false;
        if (ifPresent) {
            // payload=A..B narrows the zero-check to a specific window so a
            // block whose header bytes are non-zero can still be skipped when
            // its data payload is silent. Without payload= we check the whole
            // block (legacy behavior).
            size_t checkBegin = 0;
            size_t checkEnd = block->empty() ? 0 : block->size() - 1;
            const std::string payload = get("payload");
            if (!payload.empty()) {
                if (!parse_range(payload, &checkBegin, &checkEnd) ||
                    checkEnd >= block->size()) {
                    return false;
                }
            }
            bool nonzero = false;
            for (size_t i = checkBegin; i <= checkEnd; ++i) {
                if ((*block)[i] != 0) {
                    nonzero = true;
                    break;
                }
            }
            if (!nonzero) {
                return true;
            }
        }
        return append_block(destination, *block);
    }

    if (starts_with(op, "crc32.hid_output ")) {
        uint32_t seed = 0;
        if (get("target") != "physical_report" ||
            get("tail") != "last4" ||
            !parse_u32(get("seed"), &seed) ||
            seed > 0xff) {
            return false;
        }
        sign_report(destination, static_cast<uint8_t>(seed));
        return true;
    }

    return false;
}

const CtmMapRuntime::FeaturePageRule *CtmMapRuntime::find_feature_page_rule(
    const CTM_USB_EVENT &event,
    const std::vector<uint8_t> &lastFeatureSet) const
{
    if (event.event_type != CTM_USB_EVENT_FEATURE_GET ||
        featureSelectorReport_ == 0 ||
        lastFeatureSet.size() < 2 ||
        lastFeatureSet[0] != featureSelectorReport_) {
        return nullptr;
    }

    for (const FeaturePageRule &rule : featurePageRules_) {
        if (rule.ackOnly ||
            rule.responseReport != event.report_id ||
            !bytes_match_prefix(lastFeatureSet.data(), lastFeatureSet.size(), 1, rule.selector)) {
            continue;
        }
        return &rule;
    }

    return nullptr;
}

const CtmMapRuntime::UsbControlMapRule *CtmMapRuntime::find_usb_control_map_rule(const CTM_USB_EVENT &event) const
{
    for (const UsbControlMapRule &rule : usbControlMapRules_) {
        if (rule.matchEvent != event.event_type) {
            continue;
        }
        if (rule.hasMatchReport && rule.matchReport != event.report_id) {
            continue;
        }
        return &rule;
    }

    return nullptr;
}

bool CtmMapRuntime::feature_passthrough_enabled() const
{
    return
        inputSourceReport_ == 0 &&
        inputDestinationReport_ == 0 &&
        outputSourceReport_ == 0 &&
        outputDestinationReport_ == 0 &&
        featureSelectorReport_ == 0 &&
        featurePageRules_.empty() &&
        usbControlMapRules_.empty();
}

bool CtmMapRuntime::build_physical_feature_request(
    const CTM_USB_EVENT &event,
    const std::vector<uint8_t> &lastFeatureSet,
    size_t physicalReportLength,
    std::vector<uint8_t> *request) const
{
    if (request == nullptr) {
        return false;
    }

    std::vector<PhysicalFeatureAction> actions;
    if (!build_physical_feature_actions(event, lastFeatureSet, physicalReportLength, &actions)) {
        return false;
    }
    for (const PhysicalFeatureAction &action : actions) {
        if (action.operation != PhysicalFeatureOperation::GetFeature) {
            continue;
        }
        request->assign(action.length, 0);
        (*request)[0] = action.report;
        if (!action.payload.empty()) {
            std::memcpy(request->data() + 1, action.payload.data(), action.payload.size());
        }
        return true;
    }
    return false;
}

bool CtmMapRuntime::build_physical_feature_actions(
    const CTM_USB_EVENT &event,
    const std::vector<uint8_t> &lastFeatureSet,
    size_t physicalReportLength,
    std::vector<PhysicalFeatureAction> *actions) const
{
    if (actions == nullptr || physicalReportLength == 0) {
        return false;
    }
    actions->clear();

    const FeaturePageRule *rule = find_feature_page_rule(event, lastFeatureSet);
    if (rule != nullptr && rule->physicalReport != 0) {
        const uint16_t getLength = rule->physicalRequestLength != 0
            ? rule->physicalRequestLength
            : static_cast<uint16_t>(physicalReportLength);
        if (getLength == 0 || getLength > physicalReportLength) {
            return false;
        }

        if (rule->physicalSetSelector && physicalSelectorReport_ != 0) {
            if (physicalSelectorLength_ == 0 ||
                physicalSelectorLength_ > physicalReportLength ||
                1 + rule->selector.size() > physicalSelectorLength_) {
                return false;
            }
            // Belt-and-braces: emit SET first (best-effort, since some HID
            // transports — notably Windows BTHID for the DS5 — silently drop
            // HidD_SetFeature on this report), then emit the GET. The GET
            // also carries the per-page request prefix when the map provides
            // one, so a stack that ignores the SET but reads the buffer can
            // still return the right page.
            // If configured, the selector SET payload is CRC-signed here and
            // the following GET can remain a plain report-id request.
            PhysicalFeatureAction setAction;
            setAction.operation = PhysicalFeatureOperation::SetFeature;
            setAction.report = physicalSelectorReport_;
            setAction.length = physicalSelectorLength_;
            setAction.payload = rule->selector;
            setAction.bestEffort = true;
            setAction.crc32Tail = physicalSelectorCrc32Tail_;
            setAction.crc32Seed = physicalSelectorCrcSeed_;
            if (!apply_physical_feature_crc(&setAction)) {
                return false;
            }
            actions->push_back(std::move(setAction));

            PhysicalFeatureAction getAction;
            getAction.operation = PhysicalFeatureOperation::GetFeature;
            getAction.report = rule->physicalReport;
            getAction.length = getLength;
            if (1 + rule->physicalRequestPrefix.size() <= getLength) {
                getAction.payload = rule->physicalRequestPrefix;
            }
            actions->push_back(std::move(getAction));
            return true;
        }

        if (1 + rule->physicalRequestPrefix.size() > getLength) {
            return false;
        }
        PhysicalFeatureAction action;
        action.operation = PhysicalFeatureOperation::GetFeature;
        action.report = rule->physicalReport;
        action.length = getLength;
        action.payload = rule->physicalRequestPrefix;
        actions->push_back(std::move(action));
        return true;
    }

    // Implicit gating: a USB report id declared as a page response target
    // is only valid behind a selector. If we got here, no selector matched,
    // so the unguarded rule fallback is not what the host wants — let the
    // driver stall and DriftGuard retry with a fresh selector.
    if (is_selector_gated_report(event.report_id)) {
        return false;
    }

    const UsbControlMapRule *controlRule = find_usb_control_map_rule(event);
    if (controlRule == nullptr || controlRule->actions.empty()) {
        if (feature_passthrough_enabled() &&
            (event.event_type == CTM_USB_EVENT_FEATURE_GET ||
             event.event_type == CTM_USB_EVENT_FEATURE_SET)) {
            PhysicalFeatureAction action;
            action.operation = event.event_type == CTM_USB_EVENT_FEATURE_SET
                ? PhysicalFeatureOperation::SetFeature
                : PhysicalFeatureOperation::GetFeature;
            action.report = event.report_id;
            if (event.event_type == CTM_USB_EVENT_FEATURE_SET) {
                if (event.length == 0 || event.length > kMaxPhysicalFeatureReportBytes) {
                    return false;
                }
                action.length = event.length;
                if (event.length > 1) {
                    action.payload.assign(event.data + 1, event.data + event.length);
                }
            } else {
                uint16_t requestedLength = 0;
                if (event.length >= 8) {
                    requestedLength = static_cast<uint16_t>(event.data[6] | (event.data[7] << 8));
                }
                size_t getLength = requestedLength != 0 ? requestedLength : physicalReportLength;
                getLength = (std::max<size_t>)(1, getLength);
                getLength = (std::min<size_t>)(getLength, kMaxPhysicalFeatureReportBytes);
                action.length = static_cast<uint16_t>(getLength);
            }
            actions->push_back(std::move(action));
            return true;
        }
        return false;
    }

    *actions = controlRule->actions;
    return true;
}

bool CtmMapRuntime::build_feature_response_from_physical(
    const CTM_USB_EVENT &event,
    const std::vector<uint8_t> &lastFeatureSet,
    const uint8_t *physicalReport,
    size_t physicalReportLength,
    CTM_USB_RESPONSE *response) const
{
    if (physicalReport == nullptr || response == nullptr || event.request_id == 0) {
        return false;
    }

    const FeaturePageRule *rule = find_feature_page_rule(event, lastFeatureSet);
    if (rule != nullptr &&
        rule->physicalReport != 0 &&
        rule->responseLength != 0 &&
        rule->responseLength <= CTM_USB_RESPONSE_DATA_BYTES &&
        physicalReportLength >= rule->responseLength &&
        physicalReport[0] == rule->physicalReport) {
        if (!rule->responsePrefix.empty() &&
            !bytes_match_prefix(physicalReport, physicalReportLength, 1, rule->responsePrefix)) {
            return false;
        }

        std::memset(response, 0, sizeof(*response));
        response->request_id = event.request_id;
        response->status = CTM_USB_RESPONSE_SUCCESS;
        response->length = rule->responseLength;
        std::memcpy(response->data, physicalReport, rule->responseLength);
        return true;
    }

    if (is_selector_gated_report(event.report_id)) {
        return false;
    }

    if (feature_passthrough_enabled() && event.event_type == CTM_USB_EVENT_FEATURE_GET) {
        if (event.report_id != 0 && physicalReport[0] != event.report_id) {
            return false;
        }

        uint16_t requestedLength = 0;
        if (event.length >= 8) {
            requestedLength = static_cast<uint16_t>(event.data[6] | (event.data[7] << 8));
        }
        size_t responseLength = physicalReportLength;
        if (requestedLength != 0) {
            responseLength = (std::min<size_t>)(responseLength, requestedLength);
        }
        responseLength = (std::min<size_t>)(responseLength, CTM_USB_RESPONSE_DATA_BYTES);
        if (responseLength == 0) {
            return false;
        }

        std::memset(response, 0, sizeof(*response));
        response->request_id = event.request_id;
        response->status = CTM_USB_RESPONSE_SUCCESS;
        response->length = static_cast<uint16_t>(responseLength);
        std::memcpy(response->data, physicalReport, responseLength);
        return true;
    }

    const UsbControlMapRule *controlRule = find_usb_control_map_rule(event);
    if (controlRule == nullptr ||
        controlRule->physicalReport == 0 ||
        controlRule->responseLength == 0 ||
        controlRule->responseLength > CTM_USB_RESPONSE_DATA_BYTES ||
        physicalReportLength < controlRule->responseLength ||
        physicalReport[0] != controlRule->physicalReport) {
        return false;
    }

    if (!controlRule->responsePrefix.empty() &&
        !bytes_match_prefix(physicalReport, physicalReportLength, 1, controlRule->responsePrefix)) {
        return false;
    }

    std::memset(response, 0, sizeof(*response));
    response->request_id = event.request_id;
    response->status = CTM_USB_RESPONSE_SUCCESS;
    response->length = controlRule->responseLength;
    std::memcpy(response->data, physicalReport, controlRule->responseLength);
    return true;
}

bool CtmMapRuntime::feature_response_expectation(
    const CTM_USB_EVENT &event,
    const std::vector<uint8_t> &lastFeatureSet,
    FeatureResponseExpectation *expectation) const
{
    if (expectation == nullptr) {
        return false;
    }

    *expectation = {};
    const FeaturePageRule *pageRule = find_feature_page_rule(event, lastFeatureSet);
    if (pageRule != nullptr) {
        expectation->physicalReport = pageRule->physicalReport;
        expectation->responseLength = pageRule->responseLength;
        expectation->selector = pageRule->selector;
        expectation->responsePrefix = pageRule->responsePrefix;
        return true;
    }

    if (is_selector_gated_report(event.report_id)) {
        return false;
    }

    const UsbControlMapRule *controlRule = find_usb_control_map_rule(event);
    if (controlRule == nullptr) {
        return false;
    }

    expectation->physicalReport = controlRule->physicalReport;
    expectation->responseLength = controlRule->responseLength;
    expectation->responsePrefix = controlRule->responsePrefix;
    return true;
}

bool CtmMapRuntime::feature_get_miss_diagnostic(
    const CTM_USB_EVENT &event,
    const std::vector<uint8_t> &lastFeatureSet,
    size_t physicalReportLength,
    FeatureGetMissDiagnostic *diagnostic) const
{
    if (diagnostic == nullptr || event.event_type != CTM_USB_EVENT_FEATURE_GET) {
        return false;
    }

    *diagnostic = {};
    diagnostic->selectorGated = is_selector_gated_report(event.report_id);
    diagnostic->selectorReport = featureSelectorReport_;
    diagnostic->lastFeatureSetLength = lastFeatureSet.size();
    diagnostic->lastFeatureSetReport = lastFeatureSet.empty() ? 0 : lastFeatureSet[0];
    diagnostic->lastFeatureSetIsSelector =
        featureSelectorReport_ != 0 &&
        !lastFeatureSet.empty() &&
        lastFeatureSet[0] == featureSelectorReport_;

    for (const FeaturePageRule &rule : featurePageRules_) {
        if (!rule.ackOnly && rule.responseReport == event.report_id) {
            ++diagnostic->responseRuleCount;
        }

        if (!diagnostic->lastFeatureSetIsSelector ||
            !bytes_match_prefix(lastFeatureSet.data(), lastFeatureSet.size(), 1, rule.selector)) {
            continue;
        }

        diagnostic->matchedSelector = rule.selector;
        diagnostic->matchedSelectorResponseReport = rule.responseReport;
        diagnostic->matchedPhysicalReport = rule.physicalReport;
        diagnostic->matchedPhysicalRequestLength = rule.physicalRequestLength;
        diagnostic->matchedResponseLength = rule.responseLength;

        if (rule.ackOnly) {
            diagnostic->matchedAckOnlySelector = true;
            continue;
        }

        if (rule.responseReport != event.report_id) {
            diagnostic->matchedSelectorForOtherReport = true;
            continue;
        }

        diagnostic->matchedSelectorForRequestedReport = true;
        diagnostic->missingPhysicalReport = rule.physicalReport == 0;

        const uint16_t getLength = rule.physicalRequestLength != 0
            ? rule.physicalRequestLength
            : static_cast<uint16_t>(physicalReportLength);
        diagnostic->matchedPhysicalRequestLength = getLength;
        diagnostic->invalidPhysicalRequestLength =
            getLength == 0 || getLength > physicalReportLength;

        if (rule.physicalSetSelector && physicalSelectorReport_ != 0) {
            diagnostic->invalidPhysicalSelectorLength =
                physicalSelectorLength_ == 0 || physicalSelectorLength_ > physicalReportLength;
            diagnostic->invalidPhysicalSelectorPayload =
                1 + rule.selector.size() > physicalSelectorLength_;
        }

        diagnostic->invalidPhysicalRequestPrefix =
            1 + rule.physicalRequestPrefix.size() > getLength;
    }

    return true;
}

bool CtmMapRuntime::build_connect_feature_requests(
    size_t physicalReportLength,
    std::vector<std::vector<uint8_t>> *requests) const
{
    if (requests == nullptr || physicalReportLength == 0) {
        return false;
    }

    requests->clear();
    for (const ConnectFeatureRequest &item : connectFeatureRequests_) {
        const size_t length = item.length == 0 ? physicalReportLength : item.length;
        if (item.report == 0 || length > physicalReportLength || 1 + item.prefix.size() > length) {
            return false;
        }
        std::vector<uint8_t> request(length, 0);
        request[0] = item.report;
        if (!item.prefix.empty()) {
            std::memcpy(request.data() + 1, item.prefix.data(), item.prefix.size());
        }
        requests->push_back(std::move(request));
    }
    return true;
}

bool CtmMapRuntime::build_preload_feature_requests(
    size_t physicalReportLength,
    std::vector<FeaturePreloadRequest> *requests) const
{
    if (requests == nullptr || physicalReportLength == 0) {
        return false;
    }

    requests->clear();
    for (const UsbControlMapRule &rule : usbControlMapRules_) {
        if (!rule.preload || !rule.cache || !rule.hasMatchReport || rule.actions.empty()) {
            continue;
        }

        FeaturePreloadRequest request;
        request.usbReport = rule.matchReport;
        request.actions = rule.actions;
        requests->push_back(std::move(request));
    }

    if (featureSelectorReport_ != 0) {
        for (const FeaturePageRule &rule : featurePageRules_) {
            if (rule.ackOnly ||
                rule.selector.empty() ||
                rule.responseReport == 0 ||
                rule.physicalReport == 0 ||
                rule.responseLength == 0) {
                continue;
            }

            CTM_USB_EVENT fakeEvent = {};
            fakeEvent.event_type = CTM_USB_EVENT_FEATURE_GET;
            fakeEvent.report_id = rule.responseReport;

            std::vector<uint8_t> lastFeatureSet;
            lastFeatureSet.reserve(1 + rule.selector.size());
            lastFeatureSet.push_back(featureSelectorReport_);
            lastFeatureSet.insert(lastFeatureSet.end(), rule.selector.begin(), rule.selector.end());

            FeaturePreloadRequest request;
            request.usbReport = rule.responseReport;
            request.cacheSelector = rule.selector;
            request.lastFeatureSet = lastFeatureSet;
            if (!build_physical_feature_actions(
                    fakeEvent,
                    lastFeatureSet,
                    physicalReportLength,
                    &request.actions)) {
                continue;
            }
            requests->push_back(std::move(request));
        }
    }

    return true;
}

bool CtmMapRuntime::should_cache_usb_control_response(const CTM_USB_EVENT &event) const
{
    const UsbControlMapRule *rule = find_usb_control_map_rule(event);
    return rule != nullptr && rule->cache;
}

std::vector<uint8_t> CtmMapRuntime::cache_selector_for(
    const CTM_USB_EVENT &event,
    const std::vector<uint8_t> &lastFeatureSet) const
{
    const FeaturePageRule *rule = find_feature_page_rule(event, lastFeatureSet);
    if (rule != nullptr) {
        return rule->selector;
    }
    return {};
}

bool CtmMapRuntime::is_selector_gated_report(uint8_t reportId) const
{
    if (reportId == 0) {
        return false;
    }
    for (const FeaturePageRule &rule : featurePageRules_) {
        if (!rule.ackOnly && rule.responseReport == reportId) {
            return true;
        }
    }
    return false;
}
