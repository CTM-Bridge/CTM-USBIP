
static const char *feature_get_miss_state(const CtmMapRuntime::FeatureGetMissDiagnostic &diagnostic)
{
    if (!diagnostic.selectorGated) {
        return "no-map-rule";
    }
    if (diagnostic.responseRuleCount == 0) {
        return "no-response-rule";
    }
    if (diagnostic.lastFeatureSetLength == 0) {
        return "no-last-feature-set";
    }
    if (!diagnostic.lastFeatureSetIsSelector) {
        return "last-feature-set-not-selector";
    }
    if (diagnostic.matchedAckOnlySelector) {
        return "ack-only-selector";
    }
    if (diagnostic.matchedSelectorForOtherReport) {
        return "selector-for-other-report";
    }
    if (!diagnostic.matchedSelectorForRequestedReport) {
        return "selector-not-mapped";
    }
    if (diagnostic.missingPhysicalReport) {
        return "mapped-rule-missing-physical-report";
    }
    if (diagnostic.invalidPhysicalRequestLength) {
        return "mapped-rule-invalid-physical-length";
    }
    if (diagnostic.invalidPhysicalSelectorLength) {
        return "mapped-rule-invalid-selector-length";
    }
    if (diagnostic.invalidPhysicalSelectorPayload) {
        return "mapped-rule-selector-too-long";
    }
    if (diagnostic.invalidPhysicalRequestPrefix) {
        return "mapped-rule-request-prefix-too-long";
    }
    return "mapped-rule-rejected-later";
}

static void log_feature_get_unmapped_detail(
    const CTM_USB_EVENT &event,
    const std::vector<uint8_t> &lastFeatureSet,
    const CtmMapRuntime::FeatureGetMissDiagnostic &diagnostic)
{
    const size_t lastHeadLength = (std::min<size_t>)(lastFeatureSet.size(), 24);
    std::cout << "map issue"
              << " reason=feature-get-unmapped-detail"
              << " request_id=" << event.request_id
              << " report=0x" << std::hex << std::setw(2) << std::setfill('0')
              << static_cast<unsigned int>(event.report_id)
              << " selector_report=0x" << std::setw(2)
              << static_cast<unsigned int>(diagnostic.selectorReport)
              << " last_report=0x" << std::setw(2)
              << static_cast<unsigned int>(diagnostic.lastFeatureSetReport)
              << " matched_response=0x" << std::setw(2)
              << static_cast<unsigned int>(diagnostic.matchedSelectorResponseReport)
              << " physical=0x" << std::setw(2)
              << static_cast<unsigned int>(diagnostic.matchedPhysicalReport)
              << std::dec << std::setfill(' ')
              << " state=" << feature_get_miss_state(diagnostic)
              << " gated=" << (diagnostic.selectorGated ? "yes" : "no")
              << " response_rules=" << diagnostic.responseRuleCount
              << " last_len=" << diagnostic.lastFeatureSetLength
              << " selector=" << hex_vector_or_dash(diagnostic.matchedSelector)
              << " physical_len=" << diagnostic.matchedPhysicalRequestLength
              << " response_len=" << diagnostic.matchedResponseLength
              << " last_head=" << (lastFeatureSet.empty()
                  ? "-"
                  : hex_span(lastFeatureSet.data(), lastHeadLength))
              << std::endl;
}

static void log_feature_get_response_rejected(
    const CTM_USB_EVENT &event,
    const std::vector<uint8_t> &lastFeatureSet,
    const CtmMapRuntime::FeatureResponseExpectation &expectation,
    const uint8_t *physicalResponse,
    size_t physicalResponseLength)
{
    (void)lastFeatureSet;
    std::cout << "map issue"
              << " reason=feature-get-response-rejected"
              << " request_id=" << event.request_id
              << " report=0x" << std::hex << std::setw(2) << std::setfill('0')
              << static_cast<unsigned int>(event.report_id)
              << " expected_physical=0x" << std::setw(2)
              << static_cast<unsigned int>(expectation.physicalReport)
              << std::dec << std::setfill(' ')
              << " expected_len=" << expectation.responseLength
              << " selector=" << hex_vector_or_dash(expectation.selector)
              << " prefix=" << hex_vector_or_dash(expectation.responsePrefix)
              << " physical_len=" << physicalResponseLength
              << " physical_head=" << (physicalResponse == nullptr || physicalResponseLength == 0
                  ? "-"
                  : hex_span(physicalResponse, (std::min<size_t>)(physicalResponseLength, 24)))
              << std::endl;
}

static std::vector<uint8_t> string_descriptor_by_index(
    const CtmDescriptorProfile &profile,
    uint8_t wanted)
{
    size_t offset = 0;
    uint8_t index = 0;
    while (offset + 2 <= profile.string_descriptors.size()) {
        const uint8_t length = profile.string_descriptors[offset];
        const uint8_t type = profile.string_descriptors[offset + 1];
        if (length < 2 || offset + length > profile.string_descriptors.size()) {
            break;
        }
        if (type == 0x03 && index == wanted) {
            return std::vector<uint8_t>(
                profile.string_descriptors.begin() + static_cast<std::ptrdiff_t>(offset),
                profile.string_descriptors.begin() + static_cast<std::ptrdiff_t>(offset + length));
        }
        offset += length;
        ++index;
    }
    return {};
}

static std::vector<uint8_t> make_hid_descriptor(size_t reportLength)
{
    std::vector<uint8_t> descriptor(9, 0);
    descriptor[0] = 0x09;
    descriptor[1] = 0x21;
    descriptor[2] = 0x11;
    descriptor[3] = 0x01;
    descriptor[4] = 0x00;
    descriptor[5] = 0x01;
    descriptor[6] = 0x22;
    descriptor[7] = static_cast<uint8_t>(reportLength & 0xff);
    descriptor[8] = static_cast<uint8_t>((reportLength >> 8) & 0xff);
    return descriptor;
}
