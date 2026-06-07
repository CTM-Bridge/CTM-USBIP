#pragma once

#include "ctm/protocol.h"

#include <cstdint>
#include <string>
#include <vector>

struct OpusEncoder;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

class CtmMapRuntime {
public:
    struct CopyRange {
        size_t sourceBegin = 0;
        size_t sourceEnd = 0;
        size_t destinationBegin = 0;
        size_t destinationEnd = 0;
    };

    ~CtmMapRuntime();

    bool load(const std::wstring &path, std::wstring *error);

    const std::wstring &path() const { return path_; }
    double bt_audio_pace_ms() const { return btAudioPaceMs_; }
    double audio_builder_pace_ms() const { return audioBuilderPaceMs_; }
    // 0 means "not set by map"; caller should fall back to CLI flag or default.
    uint32_t iso_out_completion_delay_us() const { return isoOutCompletionDelayUs_; }
    double iso_out_completion_delay_scale() const { return isoOutCompletionDelayScale_; }
    bool log_mapped_input() const { return logMappedInput_; }

    // --- Intermediate reservoir + adaptive-pace knobs (connect-bt path) ---
    // All in ms of audio held in the reservoir (at the reservoir sample rate).
    uint32_t reservoir_sample_rate() const { return reservoirSampleRate_; }
    uint32_t intermediate_buffer_warmup_ms() const { return intermediateBufferWarmupMs_; }
    uint32_t intermediate_buffer_max_ms() const { return intermediateBufferMaxMs_; }
    double pace_adjust_step_ms() const { return paceAdjustStepMs_; }
    // How many BT frames pass between feedback check ticks.
    uint32_t pace_adjust_interval_frames() const { return paceAdjustIntervalFrames_; }
    // Hard bounds on the writer pace. The feedback loop clamps to this range.
    double pace_min_ms() const { return paceMinMs_; }
    double pace_max_ms() const { return paceMaxMs_; }
    // Velocity-based feedback knobs (see ds5_usb_over_ds5_bt.map for prose).
    //   hard_min_fill_ms / hard_max_fill_ms = panic band; outside these the
    //     pace shifts by pace_adjust_step_ms * hard_step_multiplier per tick.
    //   velocity_deadband_ms = minimum |dfill| in a tick to trigger an
    //     adjustment inside the safe band. Below this the rates are treated
    //     as matched and pace is left alone.
    uint32_t pace_hard_min_fill_ms() const { return paceHardMinFillMs_; }
    uint32_t pace_hard_max_fill_ms() const { return paceHardMaxFillMs_; }
    uint32_t pace_hard_step_multiplier() const { return paceHardStepMultiplier_; }
    uint32_t pace_velocity_deadband_ms() const { return paceVelocityDeadbandMs_; }

    // Exposed so the builder thread can construct a valid synthetic ISO OUT
    // event when pulling from the intermediate reservoir.
    uint8_t usb_iso_out_endpoint() const { return usbIsoOutEndpoint_; }
    uint32_t iso_out_sample_rate() const { return isoOutSampleRate_; }
    uint16_t iso_expected_length() const { return isoExpectedLength_; }
    uint32_t iso_sample_rate() const { return isoSampleRate_; }
    uint16_t iso_frame_samples() const { return isoFrameSamples_; }
    uint8_t iso_channels() const { return isoChannels_; }

    void set_audio_latency(uint8_t latency) { audioLatency_ = latency; }
    void set_audio_block_id(uint8_t blockId) { audioBlockId_ = blockId; }

    enum class PhysicalFeatureOperation {
        GetFeature,
        SetFeature
    };

    struct PhysicalFeatureAction {
        PhysicalFeatureOperation operation = PhysicalFeatureOperation::GetFeature;
        uint8_t report = 0;
        uint16_t length = 0;
        std::vector<uint8_t> payload;
        // best_effort=true means a failure of this action is logged but the
        // sequence keeps going (the GET that follows can still succeed even
        // if a SET selector was silently dropped by the transport).
        bool bestEffort = false;
        bool crc32Tail = false;
        uint8_t crc32Seed = 0;
    };

    struct FeatureResponseExpectation {
        uint8_t physicalReport = 0;
        uint16_t responseLength = 0;
        std::vector<uint8_t> selector;
        std::vector<uint8_t> responsePrefix;
    };

    struct FeatureGetMissDiagnostic {
        bool selectorGated = false;
        uint8_t selectorReport = 0;
        size_t responseRuleCount = 0;
        size_t lastFeatureSetLength = 0;
        uint8_t lastFeatureSetReport = 0;
        bool lastFeatureSetIsSelector = false;
        bool matchedAckOnlySelector = false;
        bool matchedSelectorForOtherReport = false;
        bool matchedSelectorForRequestedReport = false;
        uint8_t matchedSelectorResponseReport = 0;
        uint8_t matchedPhysicalReport = 0;
        uint16_t matchedPhysicalRequestLength = 0;
        uint16_t matchedResponseLength = 0;
        bool missingPhysicalReport = false;
        bool invalidPhysicalRequestLength = false;
        bool invalidPhysicalSelectorLength = false;
        bool invalidPhysicalSelectorPayload = false;
        bool invalidPhysicalRequestPrefix = false;
        std::vector<uint8_t> matchedSelector;
    };

    bool translate_controller_input(
        const uint8_t *source,
        size_t sourceLength,
        CTM_INPUT_REPORT *destination);

    bool translate_controller_output(
        const CTM_USB_EVENT &source,
        uint8_t *sequence,
        std::vector<uint8_t> *destination);

    bool apply_usb_control_state(const CTM_USB_EVENT &event);

    bool handle_feature_set(const CTM_USB_EVENT &event) const;

    bool build_static_feature_response(
        const CTM_USB_EVENT &event,
        const std::vector<uint8_t> &lastFeatureSet,
        CTM_USB_RESPONSE *response) const;

    bool build_physical_feature_request(
        const CTM_USB_EVENT &event,
        const std::vector<uint8_t> &lastFeatureSet,
        size_t physicalReportLength,
        std::vector<uint8_t> *request) const;

    bool build_physical_feature_actions(
        const CTM_USB_EVENT &event,
        const std::vector<uint8_t> &lastFeatureSet,
        size_t physicalReportLength,
        std::vector<PhysicalFeatureAction> *actions) const;

    bool build_feature_response_from_physical(
        const CTM_USB_EVENT &event,
        const std::vector<uint8_t> &lastFeatureSet,
        const uint8_t *physicalReport,
        size_t physicalReportLength,
        CTM_USB_RESPONSE *response) const;

    bool feature_response_expectation(
        const CTM_USB_EVENT &event,
        const std::vector<uint8_t> &lastFeatureSet,
        FeatureResponseExpectation *expectation) const;

    bool feature_get_miss_diagnostic(
        const CTM_USB_EVENT &event,
        const std::vector<uint8_t> &lastFeatureSet,
        size_t physicalReportLength,
        FeatureGetMissDiagnostic *diagnostic) const;

    bool build_connect_feature_requests(
        size_t physicalReportLength,
        std::vector<std::vector<uint8_t>> *requests) const;

    struct FeaturePreloadRequest {
        uint8_t usbReport = 0;
        std::vector<uint8_t> cacheSelector;
        std::vector<uint8_t> lastFeatureSet;
        std::vector<PhysicalFeatureAction> actions;
    };
    bool build_preload_feature_requests(
        size_t physicalReportLength,
        std::vector<FeaturePreloadRequest> *requests) const;

    bool should_cache_usb_control_response(const CTM_USB_EVENT &event) const;

    // Returns the selector tail that identifies which page (if any) is active
    // for this event. Empty vector means "no selector / non-gated". Callers
    // use (event.report_id, this vector) as a composite cache key so different
    // pages on the same report id don't overwrite each other.
    std::vector<uint8_t> cache_selector_for(
        const CTM_USB_EVENT &event,
        const std::vector<uint8_t> &lastFeatureSet) const;

    // True when this report id is only valid behind a selector. Callers
    // should not invoke the unguarded rule fallback for these reports.
    bool is_selector_gated_report(uint8_t reportId) const;

    bool build_stream_output_report(
        const CTM_USB_EVENT &source,
        std::vector<uint8_t> *destination);

    bool build_control_response(
        const CTM_USB_EVENT &event,
        CTM_USB_RESPONSE *response);

    bool build_virtual_input_reports(
        const CTM_USB_EVENT &event,
        std::vector<CTM_INPUT_REPORT> *reports);

private:
    bool parse_map(std::wstring *error);
    struct FeaturePageRule {
        std::vector<uint8_t> selector;
        uint8_t responseReport = 0;
        uint16_t responseLength = 0;
        std::vector<uint8_t> responsePrefix;
        std::vector<uint8_t> staticResponse;
        uint8_t physicalReport = 0;
        std::vector<uint8_t> physicalRequestPrefix;
        uint16_t physicalRequestLength = 0;
        bool physicalSetSelector = false;
        bool ackOnly = false;
    };
    struct UsbControlMapRule {
        uint32_t matchEvent = CTM_USB_EVENT_NONE;
        uint8_t matchReport = 0;
        bool hasMatchReport = false;
        uint8_t physicalReport = 0;
        uint16_t physicalLength = 0;
        std::vector<uint8_t> physicalRequestPrefix;
        std::vector<PhysicalFeatureAction> actions;
        uint16_t responseLength = 0;
        std::vector<uint8_t> responsePrefix;
        std::vector<uint8_t> staticResponse;
        bool cache = false;
        bool preload = false;
    };
    struct ConnectFeatureRequest {
        uint8_t report = 0;
        uint16_t length = 0;
        std::vector<uint8_t> prefix;
    };
    struct VirtualInputRule {
        uint32_t matchEvent = CTM_USB_EVENT_NONE;
        uint8_t matchEndpoint = 0;
        bool hasMatchEndpoint = false;
        std::vector<uint8_t> matchPrefix;
        uint8_t destinationEndpoint = 0;
        std::vector<std::vector<uint8_t>> packets;
    };
    const FeaturePageRule *find_feature_page_rule(
        const CTM_USB_EVENT &event,
        const std::vector<uint8_t> &lastFeatureSet) const;
    const UsbControlMapRule *find_usb_control_map_rule(const CTM_USB_EVENT &event) const;
    bool feature_passthrough_enabled() const;
    bool ensure_opus_encoder(uint32_t sampleRate);
    bool ensure_sbc_encoder(
        uint32_t sampleRate,
        uint8_t bitpool,
        uint8_t blocks,
        uint8_t subbands,
        uint8_t channelMode,
        uint8_t allocation);
    bool execute_stream_ops(const CTM_USB_EVENT &source, std::vector<uint8_t> *destination);
    bool execute_stream_op(const std::string &op, const CTM_USB_EVENT &source, std::vector<uint8_t> *destination);
    bool execute_input_ops(
        const uint8_t *source,
        size_t sourceLength,
        uint8_t *destination,
        size_t destinationLength);
    bool execute_byte_op(
        const std::string &op,
        const uint8_t *source,
        size_t sourceLength,
        uint8_t *destination,
        size_t destinationLength);
    bool select_pcm_channels(const CTM_USB_EVENT &source, const std::vector<unsigned int> &channels, std::vector<int16_t> *destination) const;
    bool encode_opus(const std::vector<int16_t> &source, uint32_t sampleRate, size_t frameSamples, size_t bytes, std::vector<uint8_t> *destination);
    bool encode_sbc(
        const std::vector<int16_t> &source,
        uint32_t sampleRate,
        size_t frameSamples,
        size_t bytes,
        uint8_t bitpool,
        uint8_t blocks,
        uint8_t subbands,
        uint8_t channelMode,
        uint8_t allocation,
        std::vector<uint8_t> *destination);
    bool resample_linear(
        const std::vector<int16_t> &source,
        unsigned int channels,
        unsigned int fromRate,
        unsigned int toRate,
        size_t frameSamples,
        std::vector<int16_t> *destination) const;
    bool downsample_average(const std::vector<int16_t> &source, unsigned int fromRate, unsigned int toRate, std::vector<int16_t> *destination) const;
    bool convert_s16_to_s8(const std::vector<int16_t> &source, std::vector<int8_t> *destination) const;
    bool append_block(std::vector<uint8_t> *destination, const std::vector<uint8_t> &block);
    void sign_report(std::vector<uint8_t> *report, uint8_t seed) const;
    bool apply_physical_feature_crc(PhysicalFeatureAction *action) const;
    static uint8_t scale_volume_to_byte(int16_t value, int16_t minimum, int16_t maximum, uint8_t maxByte);
    static uint8_t scale_volume_to_padded_byte(int16_t value, int16_t minimum, int16_t maximum);

    std::wstring path_;

    uint8_t usbInputEndpoint_ = 0x84;
    uint8_t usbOutputEndpoint_ = 0x03;
    uint8_t usbIsoOutEndpoint_ = 0x01;
    uint8_t usbIsoInEndpoint_ = 0x82;
    uint8_t inputSourceReport_ = 0;
    size_t inputSourceOffset_ = 0;
    uint8_t inputDestinationReport_ = 0;
    uint16_t inputDestinationLength_ = 0;
    CopyRange inputCopy_;
    std::vector<std::string> inputOps_;

    uint8_t outputSourceReport_ = 0;
    uint8_t outputDestinationReport_ = 0;
    uint16_t outputDestinationLength_ = 0;
    uint8_t outputTag_ = 0;
    uint8_t outputCrcSeed_ = 0xa2;
    bool outputCrcEnabled_ = true;
    CopyRange outputCopy_;
    size_t outputEffectsPayloadLength_ = 0;
    std::vector<std::string> outputOps_;

    uint8_t featureSelectorReport_ = 0;
    uint8_t physicalSelectorReport_ = 0;
    uint16_t physicalSelectorLength_ = 64;
    bool physicalSelectorCrc32Tail_ = false;
    uint8_t physicalSelectorCrcSeed_ = 0;
    std::vector<FeaturePageRule> featurePageRules_;
    std::vector<UsbControlMapRule> usbControlMapRules_;
    std::vector<ConnectFeatureRequest> connectFeatureRequests_;
    std::vector<VirtualInputRule> virtualInputRules_;

    uint8_t audioControlInterface_ = 0;
    uint8_t speakerFeatureUnit_ = 2;
    uint8_t headsetFeatureUnit_ = 5;
    uint8_t muteControl_ = 1;
    uint8_t volumeControl_ = 2;
    uint8_t sampleFreqControl_ = 1;

    uint8_t speakerMute_ = 0;
    int16_t speakerVolumeMin_ = -25600;
    int16_t speakerVolumeMax_ = 0;
    int16_t speakerVolumeRes_ = 0x0100;

    uint8_t headsetMute_ = 0;
    int16_t headsetVolumeRaw_ = 0;
    int16_t headsetVolumeMin_ = 0;
    int16_t headsetVolumeMax_ = 12288;
    int16_t headsetVolumeRes_ = 122;

    uint32_t isoOutSampleRate_ = 48000;
    uint32_t isoOutSampleRateMin_ = 48000;
    uint32_t isoOutSampleRateMax_ = 48000;
    uint32_t isoOutSampleRateRes_ = 0;
    uint32_t isoInSampleRate_ = 48000;
    uint32_t isoInSampleRateMin_ = 48000;
    uint32_t isoInSampleRateMax_ = 48000;
    uint32_t isoInSampleRateRes_ = 0;

    uint16_t isoExpectedLength_ = 0;
    uint32_t isoSampleRate_ = 48000;
    uint32_t reservoirSampleRate_ = 44100;
    uint8_t isoChannels_ = 4;
    uint16_t isoFrameSamples_ = 480;
    uint16_t hapticOutputRate_ = 3200;
    uint8_t streamReportId_ = 0x36;
    uint16_t streamReportLength_ = 398;
    double btAudioPaceMs_ = 0.0;
    double audioBuilderPaceMs_ = 0.0;
    uint32_t isoOutCompletionDelayUs_ = 0;  // 0 = not set by map
    double isoOutCompletionDelayScale_ = 1.0;
    // Defaults match the documented "ride near empty" design point.
    uint32_t intermediateBufferWarmupMs_ = 20;
    uint32_t intermediateBufferMaxMs_ = 200;
    double paceAdjustStepMs_ = 0.01;
    uint32_t paceAdjustIntervalFrames_ = 1;
    double paceMinMs_ = 9.4;
    double paceMaxMs_ = 11.8;
    uint32_t paceHardMinFillMs_ = 5;
    uint32_t paceHardMaxFillMs_ = 40;
    uint32_t paceHardStepMultiplier_ = 5;
    uint32_t paceVelocityDeadbandMs_ = 1;
    uint32_t opusSampleRate_ = 48000;
    uint16_t opusFrameSamples_ = 480;
    uint32_t sbcSampleRate_ = 32000;
    uint16_t sbcFrameSamples_ = 512;
    uint8_t stateBlockId_ = 0x90;
    uint8_t timingBlockId_ = 0x91;
    uint8_t hapticBlockId_ = 0x92;
    uint8_t audioBlockId_ = 0x95;
    uint16_t opusPayloadBytes_ = 200;
    uint16_t sbcPayloadBytes_ = 448;
    uint8_t sbcBitpool_ = 25;
    uint8_t sbcBlocks_ = 16;
    uint8_t sbcSubbands_ = 8;
    uint8_t sbcChannelMode_ = 1; // SBC dual-channel.
    uint8_t sbcAllocation_ = 0; // SBC loudness.
    uint8_t audioLatency_ = 0x60;
    bool logMappedInput_ = false;

    uint8_t reportSequence_ = 0;
    uint8_t mapCounter_ = 1;
    uint8_t audioSequence_ = 0;
    uint16_t audioFrameCounter_ = 0;
    uint8_t speakerVolume_ = 0x54;
    uint8_t headsetVolume_ = 0x0e;
    uint8_t audioMute_ = 0xff;
    int16_t speakerVolumeRaw_ = -25600;
    uint64_t isoFrames_ = 0;
    uint64_t streamReports_ = 0;

    std::vector<int16_t> speakerPcm_;
    std::vector<int16_t> speakerPcmResampled_;
    std::vector<int16_t> hapticPcm_;
    std::vector<int16_t> hapticPcmDownsampled_;
    std::vector<int8_t> hapticS8_;
    std::vector<uint8_t> opusPayload_;
    std::vector<uint8_t> sbcPayload_;
    std::vector<uint8_t> outputEffectsPayload_;
    std::vector<uint8_t> stateBlock_;
    std::vector<uint8_t> timingBlock_;
    std::vector<uint8_t> audioBlock_;
    std::vector<uint8_t> hapticBlock_;
    std::vector<std::string> streamOps_;
    size_t streamAppendOffset_ = 2;

    OpusEncoder *opus_ = nullptr;
    uint32_t opusEncoderSampleRate_ = 0;
    AVCodecContext *sbc_ = nullptr;
    AVFrame *sbcFrame_ = nullptr;
    AVPacket *sbcPacket_ = nullptr;
    uint32_t sbcEncoderSampleRate_ = 0;
    uint8_t sbcEncoderBitpool_ = 0;
    uint8_t sbcEncoderBlocks_ = 0;
    uint8_t sbcEncoderSubbands_ = 0;
    uint8_t sbcEncoderChannelMode_ = 0;
    uint8_t sbcEncoderAllocation_ = 0;
};
