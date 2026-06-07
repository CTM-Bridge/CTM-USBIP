enum class InputReplyKind {
    None,
    Fresh,
    Stale,
    Zero
};

struct CtmSubmitInfo {
    uint8_t endpointAddress = 0;
    bool endpointIso = false;
    bool endpointInterrupt = false;
    InputReplyKind inputReply = InputReplyKind::None;
    uint32_t inputWaitUs = 0;
};

struct CtmAudioStats {
    uint64_t isoEvents = 0;
    uint64_t isoBytes = 0;
    uint64_t inputFrames = 0;
    uint64_t reservoirFrames = 0;
    uint64_t chunksBuilt = 0;
    uint64_t buildFails = 0;
    uint64_t sendFails = 0;
    uint64_t trailingBytes = 0;
    uint64_t reservoirDrops = 0;
    uint64_t reservoirWaits = 0;
    uint32_t reservoirFillMs = 0;
};

class CtmUsbipDevice {
public:
    ~CtmUsbipDevice()
    {
        stop_audio_stream();
    }

    bool load(
        const std::wstring &profilePath,
        const std::wstring &mapPath,
        uint8_t audioLatency,
        bool hasAudioBlockOverride,
        uint8_t audioBlockOverride,
        std::wstring *error)
    {
        CtmDescriptorProfile loadedProfile;
        if (!ctm_load_descriptor_profile(profilePath, &loadedProfile, error)) {
            return false;
        }
        if (!load_map(mapPath, audioLatency, hasAudioBlockOverride, audioBlockOverride, error)) {
            return false;
        }
        if (!set_profile(loadedProfile, error)) {
            return false;
        }
        return true;
    }

    bool load_map(
        const std::wstring &mapPath,
        uint8_t audioLatency,
        bool hasAudioBlockOverride,
        uint8_t audioBlockOverride,
        std::wstring *error)
    {
        if (!map_.load(mapPath, error)) {
            return false;
        }
        map_.set_audio_latency(audioLatency);
        if (hasAudioBlockOverride) {
            map_.set_audio_block_id(audioBlockOverride);
        }
        configure_audio_stream_from_map();
        return true;
    }

    bool set_profile(const CtmDescriptorProfile &profile, std::wstring *error)
    {
        if (profile.device_descriptor.empty() || profile.configuration_descriptor.empty()) {
            if (error) *error = L"descriptor profile is empty";
            return false;
        }
        profile_ = profile;
        info_ = parse_usb_info(profile_);
        return true;
    }

    double bt_audio_pace_ms() const
    {
        return map_.bt_audio_pace_ms();
    }

    uint32_t iso_out_completion_delay_us() const
    {
        const uint32_t delay = map_.iso_out_completion_delay_us();
        return delay == 0 ? 10000 : delay;
    }

    uint32_t iso_out_audio_duration_us(uint8_t endpointAddress, uint32_t byteCount) const
    {
        if (endpointAddress != audioIsoEndpoint_ || audioChannels_ == 0 || audioInputSampleRate_ == 0) {
            return 0;
        }
        const uint32_t bytesPerFrame = static_cast<uint32_t>(audioChannels_) * sizeof(int16_t);
        if (bytesPerFrame == 0) {
            return 0;
        }
        const uint32_t frames = byteCount / bytesPerFrame;
        if (frames == 0) {
            return 0;
        }
        const uint64_t durationUs =
            (static_cast<uint64_t>(frames) * 1000000ULL + audioInputSampleRate_ - 1) /
            audioInputSampleRate_;
        const double scale = map_.iso_out_completion_delay_scale();
        const uint64_t scaledDurationUs = static_cast<uint64_t>(
            static_cast<double>(durationUs) * scale + 0.5);
        return static_cast<uint32_t>((std::min<uint64_t>)(
            (std::max<uint64_t>)(scaledDurationUs, 1ULL),
            100000ULL));
    }

    uint32_t iso_out_audio_frames(uint8_t endpointAddress, uint32_t byteCount) const
    {
        if (endpointAddress != audioIsoEndpoint_ || audioChannels_ == 0) {
            return 0;
        }
        const uint32_t bytesPerFrame = static_cast<uint32_t>(audioChannels_) * sizeof(int16_t);
        return bytesPerFrame == 0 ? 0 : byteCount / bytesPerFrame;
    }

    uint32_t iso_out_expected_rate_hz(uint8_t endpointAddress) const
    {
        if (endpointAddress != audioIsoEndpoint_) {
            return 0;
        }
        return audioInputSampleRate_;
    }

    bool attach_backend(CtmBackend *backend, std::wstring *error)
    {
        backend_ = backend;
        const BackendCaps caps = backend_->caps();
        std::wstring virtualSerial;
        std::wstring requestedSerial = caps.serial;
        if (requestedSerial.empty()) {
            requestedSerial = L"CTMUSBIP";
        }
        if (!apply_virtual_serial_to_profile(&profile_, requestedSerial, &virtualSerial, error)) {
            return false;
        }
        info_ = parse_usb_info(profile_);
        std::wcout << L"virtual USB serial: " << virtualSerial << L"\n";
        if (!preload_features(error)) {
            return false;
        }
        start_audio_stream();
        return true;
    }

    void stop()
    {
        stop_audio_stream();
    }

    CtmAudioStats audio_stats() const
    {
        CtmAudioStats stats;
        stats.isoEvents = audioIsoEvents_.load(std::memory_order_relaxed);
        stats.isoBytes = audioIsoBytes_.load(std::memory_order_relaxed);
        stats.inputFrames = audioInputFrames_.load(std::memory_order_relaxed);
        stats.reservoirFrames = audioReservoirFrames_.load(std::memory_order_relaxed);
        stats.chunksBuilt = audioChunksBuilt_.load(std::memory_order_relaxed);
        stats.buildFails = audioBuildFails_.load(std::memory_order_relaxed);
        stats.sendFails = audioSendFails_.load(std::memory_order_relaxed);
        stats.trailingBytes = audioTrailingBytes_.load(std::memory_order_relaxed);
        stats.reservoirDrops = audioReservoir_.dropOldestEvents.load(std::memory_order_relaxed);
        stats.reservoirWaits = audioReservoir_.consumerWaits.load(std::memory_order_relaxed);
        stats.reservoirFillMs = audioReservoir_.fill_ms();
        return stats;
    }

    void record_unknown_report(const char *kind, uint8_t reportId)
    {
        std::ostringstream key;
        key << kind << ":0x" << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<unsigned int>(reportId);

        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(unknownLogMutex_);
        if (unknownLogLastFlush_.time_since_epoch().count() == 0) {
            unknownLogLastFlush_ = now;
        }
        unknownLogCounts_[key.str()]++;
        if (now - unknownLogLastFlush_ < std::chrono::seconds(5)) {
            return;
        }

        std::cout << "unknown reports";
        for (const auto &entry : unknownLogCounts_) {
            std::cout << " " << entry.first << "=" << entry.second;
        }
        std::cout << std::endl;
        unknownLogCounts_.clear();
        unknownLogLastFlush_ = now;
    }

    void log_input31_research(const uint8_t *data, size_t length)
    {
        if (data == nullptr || length == 0 || data[0] != 0x31) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(input31LogMutex_);
        if (input31Baseline_.size() != length) {
            input31Baseline_.assign(data, data + length);
            input31Last_.assign(data, data + length);
            input31LastLog_ = now;
            input31FramesSinceLog_ = 0;
            input31ChangedFramesSinceLog_ = 0;
            input31DeltaCounts_.fill(0);
            std::cout << "input31 baseline"
                      << " len=" << length
                      << " head=" << hex_span(data, (std::min<size_t>)(length, 32))
                      << std::endl;
            return;
        }

        ++input31FramesSinceLog_;
        bool changed = false;
        const size_t count = (std::min<size_t>)(length, input31DeltaCounts_.size());
        for (size_t i = 0; i < count; ++i) {
            if (i >= input31Last_.size() || input31Last_[i] != data[i]) {
                input31DeltaCounts_[i]++;
                changed = true;
            }
        }
        if (changed) {
            ++input31ChangedFramesSinceLog_;
        }
        input31Last_.assign(data, data + length);

        if (now - input31LastLog_ < std::chrono::seconds(2)) {
            return;
        }

        size_t baselineDiffCount = 0;
        std::ostringstream baselineDiff;
        for (size_t i = 0; i < count; ++i) {
            if (input31Baseline_[i] == data[i]) {
                continue;
            }
            ++baselineDiffCount;
            if (baselineDiffCount <= 24) {
                baselineDiff << " "
                             << std::hex << std::setw(2) << std::setfill('0') << i
                             << ":" << std::setw(2) << static_cast<unsigned int>(input31Baseline_[i])
                             << ">" << std::setw(2) << static_cast<unsigned int>(data[i])
                             << std::dec << std::setfill(' ');
            }
        }

        size_t changedOffsetCount = 0;
        std::ostringstream changedOffsets;
        for (size_t i = 0; i < count; ++i) {
            if (input31DeltaCounts_[i] == 0) {
                continue;
            }
            ++changedOffsetCount;
            if (changedOffsetCount <= 24) {
                changedOffsets << " "
                               << std::hex << std::setw(2) << std::setfill('0') << i
                               << "=" << std::dec << input31DeltaCounts_[i];
            }
        }

        std::cout << "input31 research"
                  << " frames=" << input31FramesSinceLog_
                  << " changed_frames=" << input31ChangedFramesSinceLog_
                  << " baseline_diff=" << baselineDiffCount << baselineDiff.str()
                  << " changing_offsets=" << changedOffsetCount << changedOffsets.str()
                  << std::endl;

        input31FramesSinceLog_ = 0;
        input31ChangedFramesSinceLog_ = 0;
        input31DeltaCounts_.fill(0);
        input31LastLog_ = now;
    }

    void on_physical_input(const uint8_t *data, size_t length, uint8_t endpoint)
    {
        if (data == nullptr || length == 0) {
            return;
        }
        if (!compInLogged_[endpoint]) {
            compInLogged_[endpoint] = true;
            std::cout << "composite input first report ep=0x" << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned int>(endpoint) << std::dec << std::setfill(' ')
                      << " len=" << length
                      << " head=" << hex_span(data, (std::min<size_t>)(length, 12)) << std::endl;
        }
        log_input31_research(data, length);

        // Composite (puck): each interface's HID report is forwarded verbatim,
        // tagged with its physical IN endpoint (carried in the bridge INPUT
        // message's request_id). Deliver the raw bytes straight to that endpoint
        // with no map translation -- the virtual device IS the physical device,
        // so the map runtime (meant for protocol-translating bridges like
        // DS4-BT->USB) must not reshape it. Per-endpoint delivery
        // (handle_endpoint_in) routes each report to the matching composite child.
        if (endpoint != 0 && !profile_.interface_report_descriptors.empty()) {
            CTM_INPUT_REPORT raw = {};
            raw.length = static_cast<uint16_t>((std::min<size_t>)(length, sizeof(raw.data)));
            memcpy(raw.data, data, raw.length);
            raw.endpoint_address = endpoint;
            enqueue_input_report(raw);
            return;
        }

        CTM_INPUT_REPORT report = {};
        bool logMappedInput = false;
        {
            std::lock_guard<std::mutex> guard(mapMutex_);
            if (!map_.translate_controller_input(data, length, &report)) {
                record_unknown_report("input", data[0]);
                if (ctm_verbose_logs()) {
                    std::cout << "input unmapped report=0x" << std::hex << std::setw(2) << std::setfill('0')
                              << static_cast<unsigned int>(data[0])
                              << std::dec << std::setfill(' ')
                              << " len=" << length
                              << " head=" << hex_span(data, (std::min<size_t>)(length, 24))
                              << std::endl;
                }
                return;
            }
            logMappedInput = map_.log_mapped_input();
        }
        if (logMappedInput) {
            log_mapped_input_debug(data, length, report);
        }
        enqueue_input_report(report);
    }

    void wake_input_waiters()
    {
        inputCv_.notify_all();
    }

    const UsbDeviceInfo &info() const { return info_; }
    bool is_composite() const { return info_.interfaces.size() > 1; }
    const CtmDescriptorProfile &profile() const { return profile_; }
    bool is_iso_endpoint(uint8_t address) const { return endpoint_is_iso(info_, address); }
    bool is_interrupt_endpoint(uint8_t address) const { return endpoint_is_interrupt(info_, address); }
    uint32_t endpoint_interval_us(uint8_t address, uint32_t fallbackUs) const
    {
        return ::endpoint_interval_us(info_, address, fallbackUs);
    }

    int handle_submit(
        uint32_t direction,
        uint32_t ep,
        const uint8_t setup[8],
        const std::vector<uint8_t> &outData,
        uint32_t transferLength,
        std::vector<uint8_t> *inData,
        CtmSubmitInfo *info)
    {
        if (inData == nullptr) {
            return kStatusStall;
        }
        inData->clear();
        const uint8_t endpointAddress = static_cast<uint8_t>((ep & 0x0f) | (direction == kUsbipDirIn ? 0x80 : 0x00));
        if (info != nullptr) {
            info->endpointAddress = endpointAddress;
            info->endpointIso = endpoint_is_iso(info_, endpointAddress);
            info->endpointInterrupt = endpoint_is_interrupt(info_, endpointAddress);
        }
        if ((ep & 0x0f) == 0) {
            return handle_control(setup, outData, transferLength, inData);
        }
        if (direction == kUsbipDirIn) {
            return handle_endpoint_in(endpointAddress, transferLength, inData, info);
        }
        return handle_endpoint_out(endpointAddress, outData);
    }

    int handle_interrupt_in(
        uint8_t endpointAddress,
        uint32_t transferLength,
        std::vector<uint8_t> *inData,
        CtmSubmitInfo *submitInfo,
        const std::atomic_bool *sessionActive)
    {
        if (inData == nullptr || !endpoint_is_interrupt(info_, endpointAddress)) {
            return kStatusStall;
        }
        if (!compPollLogged_[endpointAddress]) {
            compPollLogged_[endpointAddress] = true;
            std::cout << "interrupt-IN first poll ep=0x" << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned int>(endpointAddress) << std::dec << std::setfill(' ') << std::endl;
        }
        if (submitInfo != nullptr) {
            submitInfo->endpointAddress = endpointAddress;
            submitInfo->endpointInterrupt = true;
        }

        using clock = std::chrono::steady_clock;
        const auto waitStart = clock::now();
        std::unique_lock<std::mutex> lock(inputMutex_);
        InputEndpointState &state = inputEndpointStates_[endpointAddress];
        auto pendingForEndpoint = [&]() {
            return std::find_if(
                pendingInputReports_.begin(),
                pendingInputReports_.end(),
                [&](const QueuedInputReport &item) {
                    return item.report.endpoint_address == endpointAddress &&
                        item.sequence > state.deliveredSequence;
                });
        };
        inputCv_.wait(lock, [&]() {
            const bool hasPending = pendingForEndpoint() != pendingInputReports_.end();
            const bool hasLatest =
                hasInput_ &&
                latestInput_.endpoint_address == endpointAddress &&
                inputSequence_ > state.deliveredSequence;
            return g_stop.load() ||
                (sessionActive != nullptr && !sessionActive->load()) ||
                hasPending ||
                hasLatest;
        });
        CTM_INPUT_REPORT report = {};
        uint32_t deliveredSequence = 0;
        auto pendingIt = pendingForEndpoint();
        if (pendingIt != pendingInputReports_.end()) {
            report = pendingIt->report;
            deliveredSequence = pendingIt->sequence;
            pendingInputReports_.erase(pendingIt);
        } else if (hasInput_ &&
            latestInput_.endpoint_address == endpointAddress &&
            inputSequence_ > state.deliveredSequence) {
            report = latestInput_;
            deliveredSequence = inputSequence_;
        } else {
            return kStatusStall;
        }

        const size_t copy = (std::min<size_t>)(report.length, transferLength);
        inData->assign(report.data, report.data + copy);
        state.deliveredSequence = deliveredSequence;
        if (submitInfo != nullptr) {
            submitInfo->inputReply = InputReplyKind::Fresh;
            submitInfo->inputWaitUs = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - waitStart).count());
        }
        return kStatusOk;
    }

private:
    using FeatureCacheKey = std::pair<uint8_t, std::vector<uint8_t>>;

    struct QueuedInputReport {
        uint32_t sequence = 0;
        CTM_INPUT_REPORT report = {};
    };

    struct InputEndpointState {
        uint32_t deliveredSequence = 0;
    };

    void configure_audio_stream_from_map()
    {
        audioIsoEndpoint_ = map_.usb_iso_out_endpoint();
        audioInputSampleRate_ = map_.iso_out_sample_rate();
        if (audioInputSampleRate_ == 0) {
            audioInputSampleRate_ = 48000;
        }
        audioReservoirSampleRate_ = map_.reservoir_sample_rate();
        audioChannels_ = map_.iso_channels();
        audioFrameSamples_ = map_.iso_frame_samples();
        audioExpectedLength_ = map_.iso_expected_length();
        audioReservoir_.configure(
            audioReservoirSampleRate_,
            audioChannels_,
            map_.intermediate_buffer_max_ms(),
            map_.intermediate_buffer_warmup_ms());
        std::cout << "audio pipeline"
                  << " usb_rate=" << audioInputSampleRate_
                  << " reservoir_rate=" << audioReservoirSampleRate_
                  << " channels=" << static_cast<unsigned int>(audioChannels_)
                  << " chunk_frames=" << audioFrameSamples_
                  << " chunk_bytes=" << audioExpectedLength_
                  << " bt_pace_ms=" << std::fixed << std::setprecision(3) << map_.bt_audio_pace_ms()
                  << " builder_pace_ms=" << std::fixed << std::setprecision(3) << map_.audio_builder_pace_ms()
                  << std::defaultfloat
                  << std::endl;
    }

    void enqueue_input_report(const CTM_INPUT_REPORT &report)
    {
        if (report.length == 0 || report.length > sizeof(report.data)) {
            return;
        }
        std::lock_guard<std::mutex> lock(inputMutex_);
        latestInput_ = report;
        hasInput_ = true;
        ++inputSequence_;
        pendingInputReports_.push_back(QueuedInputReport{inputSequence_, report});
        while (pendingInputReports_.size() > 64) {
            pendingInputReports_.pop_front();
        }
        inputCv_.notify_all();
    }

    void queue_virtual_input_reports(const CTM_USB_EVENT &event)
    {
        std::vector<CTM_INPUT_REPORT> reports;
        {
            std::lock_guard<std::mutex> guard(mapMutex_);
            if (!map_.build_virtual_input_reports(event, &reports)) {
                return;
            }
        }
        for (const CTM_INPUT_REPORT &report : reports) {
            enqueue_input_report(report);
        }
        if (!reports.empty()) {
            std::cout << "virtual input queued"
                      << " trigger=" << event.event_type
                      << " count=" << reports.size()
                      << " ep=0x" << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned int>(reports.front().endpoint_address)
                      << std::dec << std::setfill(' ')
                      << std::endl;
        }
    }

    void log_mapped_input_debug(
        const uint8_t *source,
        size_t sourceLength,
        const CTM_INPUT_REPORT &report)
    {
        using clock = std::chrono::steady_clock;
        const auto now = clock::now();
        std::lock_guard<std::mutex> lock(mappedInputLogMutex_);
        ++mappedInputFramesSinceLog_;
        if (mappedInputLastLog_.time_since_epoch().count() != 0 &&
            now - mappedInputLastLog_ < std::chrono::milliseconds(500)) {
            return;
        }
        mappedInputLastLog_ = now;
        const uint64_t frames = mappedInputFramesSinceLog_;
        mappedInputFramesSinceLog_ = 0;
        std::cout << "mapped input"
                  << " frames=" << frames
                  << " src_len=" << sourceLength
                  << " src=" << hex_span(source, (std::min<size_t>)(sourceLength, 24))
                  << " dst_ep=0x" << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned int>(report.endpoint_address)
                  << std::dec << std::setfill(' ')
                  << " dst_len=" << report.length
                  << " dst=" << hex_span(report.data, (std::min<size_t>)(report.length, 48))
                  << std::endl;
    }

    void start_audio_stream()
    {
        std::lock_guard<std::mutex> lock(audioThreadMutex_);
        if (audioThreadRunning_.load(std::memory_order_relaxed) || backend_ == nullptr) {
            return;
        }
        audioThreadRunning_.store(true, std::memory_order_relaxed);
        audioBuilderThread_ = std::thread([this]() { audio_builder_loop(); });
    }

    void stop_audio_stream()
    {
        {
            std::lock_guard<std::mutex> lock(audioThreadMutex_);
            if (!audioThreadRunning_.load(std::memory_order_relaxed)) {
                return;
            }
            audioThreadRunning_.store(false, std::memory_order_relaxed);
            audioReservoir_.request_stop();
        }
        if (audioBuilderThread_.joinable()) {
            audioBuilderThread_.join();
        }
    }

    void audio_builder_loop()
    {
        const size_t chunkSamples =
            static_cast<size_t>(audioFrameSamples_) * static_cast<size_t>(audioChannels_);
        const size_t chunkBytes = chunkSamples * sizeof(int16_t);
        const size_t eventCapacity = sizeof(((CTM_USB_EVENT *)0)->data);
        if (chunkBytes == 0 || chunkBytes > eventCapacity) {
            std::cout << "audio issue reason=invalid-chunk"
                      << " chunk_bytes=" << chunkBytes
                      << " event_capacity=" << eventCapacity
                      << std::endl;
            return;
        }

        std::vector<int16_t> chunk(chunkSamples, 0);
        using clock = std::chrono::steady_clock;
        const double builderPaceMs = map_.audio_builder_pace_ms();
        const bool paceBuilder = builderPaceMs > 0.0;
        const auto builderPace = std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<double, std::milli>(builderPaceMs));
        auto nextBuild = clock::time_point{};
        while (audioThreadRunning_.load(std::memory_order_relaxed) && !g_stop.load()) {
            if (!audioReservoir_.pull(audioFrameSamples_, chunk.data())) {
                break;
            }

            CTM_USB_EVENT event = {};
            event.event_type = CTM_USB_EVENT_ISO_OUT;
            event.endpoint_address = audioIsoEndpoint_;
            event.length = static_cast<uint16_t>(chunkBytes);
            memcpy(event.data, chunk.data(), chunkBytes);

            std::vector<uint8_t> report;
            bool ok = false;
            {
                std::lock_guard<std::mutex> guard(mapMutex_);
                ok = map_.build_stream_output_report(event, &report);
            }
            if (!ok) {
                audioBuildFails_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            audioChunksBuilt_.fetch_add(1, std::memory_order_relaxed);

            std::wstring error;
            if (!backend_->send_output_report(report, true, &error)) {
                audioSendFails_.fetch_add(1, std::memory_order_relaxed);
                std::wcerr << L"backend ISO output failed: " << error << L"\n";
            }
            std::vector<uint8_t> controllerOutput;
            {
                std::lock_guard<std::mutex> guard(lastControllerOutputMutex_);
                controllerOutput = lastControllerOutput_;
            }
            if (!controllerOutput.empty() && controllerOutput[0] == 0x31) {
                std::wstring outputError;
                if (!backend_->send_output_report(controllerOutput, false, &outputError)) {
                    audioSendFails_.fetch_add(1, std::memory_order_relaxed);
                    std::wcerr << L"backend repeated HID output failed: " << outputError << L"\n";
                }
            }
            if (paceBuilder) {
                const auto now = clock::now();
                if (nextBuild == clock::time_point{} ||
                    now > nextBuild + std::chrono::milliseconds(50)) {
                    nextBuild = now;
                }
                nextBuild += builderPace;
                if (nextBuild > now) {
                    std::this_thread::sleep_until(nextBuild);
                }
            }
        }
    }

    bool preload_features(std::wstring *error)
    {
        if (backend_ == nullptr) {
            if (error) *error = L"backend not attached";
            return false;
        }
        const size_t physicalFeatureLength = (std::max<size_t>)(
            backend_->caps().featureReportLength == 0 ? 64 : backend_->caps().featureReportLength,
            CTM_SHARED_FEATURE_REPORT_BYTES);
        physicalFeatureScratch_.assign(physicalFeatureLength, 0);

        std::vector<std::vector<uint8_t>> connectRequests;
        if (!map_.build_connect_feature_requests(physicalFeatureLength, &connectRequests)) {
            if (error) *error = L"map connect feature request build failed";
            return false;
        }
        connectFeatureRequests_ = connectRequests;
        log_feature_probe_requests(connectFeatureRequests_, "connect-feature", 2000);

        std::vector<CtmMapRuntime::FeaturePreloadRequest> preloads;
        if (!map_.build_preload_feature_requests(physicalFeatureLength, &preloads)) {
            if (error) *error = L"map preload feature request build failed";
            return false;
        }
        for (CtmMapRuntime::FeaturePreloadRequest &preload : preloads) {
            const uint8_t *physicalResponse = nullptr;
            size_t physicalResponseLength = 0;
            if (!backend_->execute_feature_actions(
                    preload.actions,
                    &physicalFeatureScratch_,
                    &physicalResponse,
                    &physicalResponseLength,
                    "feature-preload",
                    2000)) {
                continue;
            }
            CTM_USB_EVENT fakeEvent = {};
            fakeEvent.event_type = CTM_USB_EVENT_FEATURE_GET;
            fakeEvent.request_id = 1;
            fakeEvent.report_id = preload.usbReport;
            CTM_USB_RESPONSE cachedResponse = {};
            std::lock_guard<std::mutex> guard(mapMutex_);
            if (map_.build_feature_response_from_physical(
                    fakeEvent,
                    preload.lastFeatureSet,
                    physicalResponse,
                    physicalResponseLength,
                    &cachedResponse)) {
                cachedResponse.request_id = 0;
                featureCache_[FeatureCacheKey(preload.usbReport, preload.cacheSelector)] = cachedResponse;
            }
        }
        return true;
    }

    void log_feature_probe_requests(
        const std::vector<std::vector<uint8_t>> &requests,
        const char *reason,
        unsigned int timeoutMs)
    {
        if (backend_ == nullptr) {
            return;
        }
        std::vector<uint8_t> scratch;
        for (const std::vector<uint8_t> &request : requests) {
            if (request.empty()) continue;
            CtmMapRuntime::PhysicalFeatureAction action;
            action.operation = CtmMapRuntime::PhysicalFeatureOperation::GetFeature;
            action.report = request[0];
            action.length = static_cast<uint16_t>(request.size());
            action.payload.assign(request.begin() + 1, request.end());
            std::vector<CtmMapRuntime::PhysicalFeatureAction> actions = { action };
            const uint8_t *response = nullptr;
            size_t responseLength = 0;
            const bool ok = backend_->execute_feature_actions(
                actions,
                &scratch,
                &response,
                &responseLength,
                reason,
                timeoutMs);
            std::cout << reason << " get report=0x"
                      << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned int>(action.report)
                      << std::dec << std::setfill(' ')
                      << " ok=" << (ok ? 1 : 0)
                      << " len=" << responseLength;
            if (ok && response != nullptr && responseLength > 0) {
                std::cout << " head=" << hex_span(response, (std::min<size_t>)(responseLength, 32));
            }
            std::cout << std::endl;
        }
    }

    int handle_control(
        const uint8_t setup[8],
        const std::vector<uint8_t> &outData,
        uint32_t transferLength,
        std::vector<uint8_t> *inData)
    {
        (void)transferLength;
        const uint8_t bmRequestType = setup[0];
        const uint8_t request = setup[1];
        const uint16_t value = read_le16(setup + 2);
        const uint16_t index = read_le16(setup + 4);
        const uint16_t requestedLength = read_le16(setup + 6);
        const bool dirIn = (bmRequestType & 0x80) != 0;
        const uint8_t requestType = static_cast<uint8_t>((bmRequestType >> 5) & 0x03);
        const uint8_t recipient = static_cast<uint8_t>(bmRequestType & 0x1f);

        if (requestType == 0 && request == 0x06 && dirIn) {
            const uint8_t descriptorType = static_cast<uint8_t>(value >> 8);
            const uint8_t descriptorIndex = static_cast<uint8_t>(value & 0xff);
            std::vector<uint8_t> descriptor;
            if (descriptorType == 0x01) {
                descriptor.assign(profile_.device_descriptor.begin(), profile_.device_descriptor.end());
            } else if (descriptorType == 0x02) {
                descriptor.assign(profile_.configuration_descriptor.begin(), profile_.configuration_descriptor.end());
            } else if (descriptorType == 0x03) {
                if (descriptorIndex == 0xee && !profile_.microsoft_os_string_descriptor.empty()) {
                    descriptor.assign(
                        profile_.microsoft_os_string_descriptor.begin(),
                        profile_.microsoft_os_string_descriptor.end());
                } else {
                    descriptor = string_descriptor_by_index(profile_, descriptorIndex);
                }
            } else if (descriptorType == 0x21) {
                size_t reportLen = profile_.hid_report_descriptor.size();
                if (!profile_.interface_report_descriptors.empty()) {
                    auto it = profile_.interface_report_descriptors.find(static_cast<uint8_t>(index & 0xff));
                    if (it != profile_.interface_report_descriptors.end()) reportLen = it->second.size();
                }
                descriptor = make_hid_descriptor(reportLen);
            } else if (descriptorType == 0x22) {
                const std::vector<unsigned char> *rd = &profile_.hid_report_descriptor;
                if (!profile_.interface_report_descriptors.empty()) {
                    auto it = profile_.interface_report_descriptors.find(static_cast<uint8_t>(index & 0xff));
                    if (it != profile_.interface_report_descriptors.end()) rd = &it->second;
                }
                descriptor.assign(rd->begin(), rd->end());
            } else if (descriptorType == 0x0f) {
                // BOS: the puck's bcdUSB 0x0201 makes USB 2.01+ hosts request it
                // (wValue 0x0f00). Mirror a 12-byte BOS (USB 2.0 Extension cap) so
                // Windows stops retry-stalling a BOS request during composite
                // enumeration. Harmless for the interrupt-only single-HID profiles.
                descriptor = {0x05, 0x0f, 0x0c, 0x00, 0x01,
                              0x07, 0x10, 0x02, 0x00, 0x00, 0x00, 0x00};
            }
            if (descriptor.empty()) {
                return kStatusStall;
            }
            const size_t copy = (std::min<size_t>)(descriptor.size(), requestedLength);
            inData->assign(descriptor.begin(), descriptor.begin() + static_cast<std::ptrdiff_t>(copy));
            return kStatusOk;
        }

        const uint8_t interfaceNumber = static_cast<uint8_t>(index & 0xff);
        const bool isHidInterfaceRequest =
            requestType == 1 &&
            recipient == 1 &&
            interface_is_class(info_, interfaceNumber, 0x03);

        if (isHidInterfaceRequest) {
            switch (request) {
            case 0x0a: // HID SET_IDLE
                if (dirIn) return kStatusStall;
                hidIdle_ = static_cast<uint8_t>(value >> 8);
                queue_control_state(setup, outData);
                return kStatusOk;
            case 0x0b: // HID SET_PROTOCOL
                if (dirIn) return kStatusStall;
                hidProtocol_ = static_cast<uint8_t>(value & 0xff);
                queue_control_state(setup, outData);
                return kStatusOk;
            case 0x02: // HID GET_IDLE
                if (!dirIn) return kStatusStall;
                inData->push_back(hidIdle_);
                return kStatusOk;
            case 0x03: // HID GET_PROTOCOL
                if (!dirIn) return kStatusStall;
                inData->push_back(hidProtocol_);
                return kStatusOk;
            case 0x09: // HID SET_REPORT
                if (dirIn) return kStatusStall;
                return handle_hid_set_report(value, outData, interfaceNumber);
            case 0x01: // HID GET_REPORT
                if (!dirIn) return kStatusStall;
                return handle_hid_get_report(value, setup, requestedLength, inData, interfaceNumber);
            default:
                break;
            }
        }

        // Composite: ACK class requests to NON-HID interfaces (the CDC COM port's
        // SET/GET_LINE_CODING, SET_CONTROL_LINE_STATE). We don't model the serial
        // function; acking stops usbser from retry-storming. HID interfaces are
        // handled above.
        if (requestType == 1 && recipient == 1 && is_composite() &&
            !interface_is_class(info_, interfaceNumber, 0x03)) {
            if (dirIn) {
                inData->assign((std::min<size_t>)(static_cast<size_t>(requestedLength), (size_t)64), 0);
            }
            return kStatusOk;
        }

        if (requestType == 0) {
            switch (request) {
            case 0x00: // GET_STATUS
                inData->assign(2, 0);
                if (recipient == 0) {
                    (*inData)[0] = 1;
                }
                return kStatusOk;
            case 0x08: // GET_CONFIGURATION
                inData->push_back(configuration_);
                return kStatusOk;
            case 0x09: // SET_CONFIGURATION
                configuration_ = static_cast<uint8_t>(value & 0xff);
                std::cout << "usb control set_configuration"
                          << " value=" << static_cast<unsigned int>(configuration_)
                          << " setup=" << hex_span(setup, 8)
                          << std::endl;
                queue_control_state(setup, outData);
                return kStatusOk;
            case 0x0a: // GET_INTERFACE
                if (recipient == 1 && (index & 0xff) < interfaceAlternate_.size()) {
                    inData->push_back(interfaceAlternate_[index & 0xff]);
                    return kStatusOk;
                }
                return kStatusStall;
            case 0x0b: // SET_INTERFACE
                if (recipient == 1 && (index & 0xff) < interfaceAlternate_.size()) {
                    interfaceAlternate_[index & 0xff] = static_cast<uint8_t>(value & 0xff);
                    std::cout << "usb control set_interface"
                              << " interface=" << static_cast<unsigned int>(index & 0xff)
                              << " alt=" << static_cast<unsigned int>(value & 0xff)
                              << " setup=" << hex_span(setup, 8)
                              << std::endl;
                    queue_control_state(setup, outData);
                    return kStatusOk;
                }
                return kStatusStall;
            case 0x01: // CLEAR_FEATURE
            case 0x03: // SET_FEATURE
                return kStatusOk;
            default:
                break;
            }
        }

        if (requestType == 2 &&
            dirIn &&
            profile_.microsoft_os_string_descriptor.size() >= 17 &&
            !profile_.microsoft_os_compatible_id_descriptor.empty()) {
            const uint8_t vendorCode = profile_.microsoft_os_string_descriptor[16];
            if (request == vendorCode && index == 0x0004) {
                const std::vector<unsigned char> &descriptor =
                    profile_.microsoft_os_compatible_id_descriptor;
                const size_t copy = (std::min<size_t>)(descriptor.size(), requestedLength);
                inData->assign(
                    descriptor.begin(),
                    descriptor.begin() + static_cast<std::ptrdiff_t>(copy));
                return kStatusOk;
            }
        }

        CTM_USB_EVENT event = {};
        event.event_type = CTM_USB_EVENT_CONTROL;
        event.request_id = nextRequestId_++;
        event.endpoint_address = 0;
        event.length = static_cast<uint16_t>((std::min<size_t>)(sizeof(event.data), 8 + outData.size()));
        memcpy(event.data, setup, 8);
        if (!outData.empty() && event.length > 8) {
            memcpy(event.data + 8, outData.data(), event.length - 8);
        }
        if (!dirIn || !outData.empty()) {
            std::cout << "usb control write"
                      << " setup=" << hex_span(setup, 8)
                      << " payload=" << hex_span(outData.data(), (std::min<size_t>)(outData.size(), 32))
                      << std::endl;
        }
        CTM_USB_RESPONSE response = {};
        bool handled = false;
        {
            std::lock_guard<std::mutex> guard(mapMutex_);
            handled = map_.build_control_response(event, &response);
        }
        if (!handled || response.status != CTM_USB_RESPONSE_SUCCESS) {
            std::cout << "usb control unmapped"
                      << " setup=" << hex_span(setup, 8)
                      << " payload=" << hex_span(outData.data(), (std::min<size_t>)(outData.size(), 16))
                      << std::endl;
            return kStatusStall;
        }
        const size_t copy = (std::min<size_t>)(response.length, requestedLength);
        inData->assign(response.data, response.data + copy);
        return kStatusOk;
    }

    void queue_control_state(const uint8_t setup[8], const std::vector<uint8_t> &outData)
    {
        CTM_USB_EVENT event = {};
        event.event_type = CTM_USB_EVENT_CONTROL;
        event.length = static_cast<uint16_t>((std::min<size_t>)(sizeof(event.data), 8 + outData.size()));
        memcpy(event.data, setup, 8);
        if (!outData.empty() && event.length > 8) {
            memcpy(event.data + 8, outData.data(), event.length - 8);
        }
        {
            std::lock_guard<std::mutex> guard(mapMutex_);
            (void)map_.apply_usb_control_state(event);
        }
        queue_virtual_input_reports(event);
    }

    int handle_hid_set_report(uint16_t value, const std::vector<uint8_t> &payload, uint8_t interfaceNumber)
    {
        const uint8_t reportType = static_cast<uint8_t>(value >> 8);
        const uint8_t reportId = static_cast<uint8_t>(value & 0xff);
        if (payload.empty()) {
            return kStatusOk;
        }
        if (is_composite() && reportType == 0x03) {
            // Composite identity: forward the feature SET verbatim to the
            // addressed interface's hidraw (no map). Best-effort ack so a
            // physical reject doesn't stall Windows init / Steam's config.
            std::vector<uint8_t> raw;
            if (reportId != 0 && payload[0] != reportId) raw.push_back(reportId);
            raw.insert(raw.end(), payload.begin(), payload.end());
            std::vector<uint8_t> reply;
            (void)backend_->remote_interface_feature(interfaceNumber, false, raw, &reply, 250);
            return kStatusOk;
        }
        CTM_USB_EVENT event = {};
        event.event_type = reportType == 0x02 ? CTM_USB_EVENT_HID_OUTPUT :
            (reportType == 0x03 ? CTM_USB_EVENT_FEATURE_SET : CTM_USB_EVENT_CONTROL);
        event.report_id = reportId;
        event.endpoint_address = 0;
        bool prependReportId = reportId != 0 && payload[0] != reportId;
        event.length = static_cast<uint16_t>((std::min<size_t>)(
            sizeof(event.data),
            payload.size() + (prependReportId ? 1 : 0)));
        size_t offset = 0;
        if (prependReportId && event.length != 0) {
            event.data[0] = reportId;
            offset = 1;
        }
        if (event.length > offset) {
            memcpy(event.data + offset, payload.data(), event.length - offset);
        }
        if (event.event_type == CTM_USB_EVENT_HID_OUTPUT) {
            std::cout << "usb hid set-output"
                      << " report=0x" << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned int>(reportId)
                      << std::dec << std::setfill(' ')
                      << " len=" << event.length
                      << " head=" << hex_span(event.data, (std::min<size_t>)(event.length, 32))
                      << std::endl;
        }
        if (event.event_type == CTM_USB_EVENT_FEATURE_SET) {
            lastFeatureSet_.assign(event.data, event.data + event.length);
            std::cout << "usb feature set"
                      << " report=0x" << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned int>(reportId)
                      << std::dec << std::setfill(' ')
                      << " len=" << event.length
                      << " head=" << hex_span(event.data, (std::min<size_t>)(event.length, 32))
                      << std::endl;
            bool handled = false;
            std::vector<CtmMapRuntime::PhysicalFeatureAction> actions;
            const size_t physicalFeatureLength = (std::max<size_t>)(
                backend_->caps().featureReportLength == 0 ? 64 : backend_->caps().featureReportLength,
                CTM_SHARED_FEATURE_REPORT_BYTES);
            {
                std::lock_guard<std::mutex> guard(mapMutex_);
                handled = map_.handle_feature_set(event);
                if (map_.build_physical_feature_actions(event, lastFeatureSet_, physicalFeatureLength, &actions)) {
                    handled = true;
                }
            }
            if (!actions.empty()) {
                const uint8_t *ignoredResponse = nullptr;
                size_t ignoredResponseLength = 0;
                if (!backend_->execute_feature_actions(
                        actions,
                        &physicalFeatureScratch_,
                        &ignoredResponse,
                        &ignoredResponseLength,
                        "feature-set",
                        250)) {
                    return kStatusStall;
                }
            }
            if (!handled) {
                record_unknown_report("feature-set", reportId);
                if (ctm_verbose_logs()) {
                    std::cout << "feature set unmapped report=0x" << std::hex << std::setw(2) << std::setfill('0')
                              << static_cast<unsigned int>(reportId)
                              << std::dec << std::setfill(' ')
                              << " head=" << hex_span(event.data, (std::min<size_t>)(event.length, 24))
                              << std::endl;
                }
            }
            return kStatusOk;
        }
        if (event.event_type == CTM_USB_EVENT_HID_OUTPUT) {
            return process_hid_output_event(event);
        }
        return kStatusOk;
    }

    int handle_hid_get_report(
        uint16_t value,
        const uint8_t setup[8],
        uint16_t requestedLength,
        std::vector<uint8_t> *inData,
        uint8_t interfaceNumber)
    {
        const uint8_t reportType = static_cast<uint8_t>(value >> 8);
        const uint8_t reportId = static_cast<uint8_t>(value & 0xff);
        if (reportType != 0x03) {
            return kStatusStall;
        }
        if (is_composite()) {
            // Composite identity: serve GET_REPORT from the addressed interface's
            // hidraw (HIDIOCGFEATURE) at the full requested length. The request
            // buffer is the report (id in byte 0) sized to wLength so the TV
            // ioctl reads the whole report, not just the id byte.
            const size_t want = requestedLength ? static_cast<size_t>(requestedLength) : 64;
            std::vector<uint8_t> req(want, 0);
            req[0] = reportId;
            std::vector<uint8_t> reply;
            if (backend_->remote_interface_feature(interfaceNumber, true, req, &reply, 250) && !reply.empty()) {
                const size_t copy = (std::min<size_t>)(reply.size(), want);
                inData->assign(reply.begin(), reply.begin() + static_cast<std::ptrdiff_t>(copy));
                return kStatusOk;
            }
            return kStatusStall;
        }
        CTM_USB_EVENT event = {};
        event.event_type = CTM_USB_EVENT_FEATURE_GET;
        event.request_id = nextRequestId_++;
        event.endpoint_address = 0;
        event.report_id = reportId;
        event.length = 8;
        memcpy(event.data, setup, 8);

        FeatureCacheKey cacheKey;
        {
            std::lock_guard<std::mutex> guard(mapMutex_);
            cacheKey = FeatureCacheKey(reportId, map_.cache_selector_for(event, lastFeatureSet_));
        }
        auto cached = featureCache_.find(cacheKey);
        if (cached != featureCache_.end()) {
            const CTM_USB_RESPONSE &response = cached->second;
            const size_t copy = (std::min<size_t>)(response.length, requestedLength);
            inData->assign(response.data, response.data + copy);
            return kStatusOk;
        }

        CTM_USB_RESPONSE staticResponse = {};
        {
            std::lock_guard<std::mutex> guard(mapMutex_);
            if (map_.build_static_feature_response(event, lastFeatureSet_, &staticResponse)) {
                if (map_.should_cache_usb_control_response(event)) {
                    CTM_USB_RESPONSE cachedResponse = staticResponse;
                    cachedResponse.request_id = 0;
                    featureCache_[cacheKey] = cachedResponse;
                }
                const size_t copy = (std::min<size_t>)(staticResponse.length, requestedLength);
                inData->assign(staticResponse.data, staticResponse.data + copy);
                return kStatusOk;
            }
        }

        const size_t physicalFeatureLength = (std::max<size_t>)(
            backend_->caps().featureReportLength == 0 ? 64 : backend_->caps().featureReportLength,
            CTM_SHARED_FEATURE_REPORT_BYTES);
        std::vector<CtmMapRuntime::PhysicalFeatureAction> actions;
        bool handled = false;
        {
            std::lock_guard<std::mutex> guard(mapMutex_);
            handled = map_.build_physical_feature_actions(event, lastFeatureSet_, physicalFeatureLength, &actions);
        }
        if (!handled) {
            record_unknown_report("feature-get", reportId);
            if (ctm_verbose_logs()) {
                std::cout << "feature get unmapped report=0x" << std::hex << std::setw(2) << std::setfill('0')
                          << static_cast<unsigned int>(reportId)
                          << std::dec << std::setfill(' ') << std::endl;
            }
            CtmMapRuntime::FeatureGetMissDiagnostic diagnostic;
            {
                std::lock_guard<std::mutex> guard(mapMutex_);
                if (map_.feature_get_miss_diagnostic(
                        event,
                        lastFeatureSet_,
                        physicalFeatureLength,
                        &diagnostic)) {
                    if (ctm_verbose_logs()) {
                        log_feature_get_unmapped_detail(event, lastFeatureSet_, diagnostic);
                    }
                }
            }
            return kStatusStall;
        }
        const uint8_t *physicalResponse = nullptr;
        size_t physicalResponseLength = 0;
        if (!backend_->execute_feature_actions(
                actions,
                &physicalFeatureScratch_,
                &physicalResponse,
                &physicalResponseLength,
                "feature-on-demand",
                250)) {
            return kStatusStall;
        }
        CTM_USB_RESPONSE response = {};
        {
            std::lock_guard<std::mutex> guard(mapMutex_);
            handled = map_.build_feature_response_from_physical(
                event,
                lastFeatureSet_,
                physicalResponse,
                physicalResponseLength,
                &response);
            if (handled && map_.should_cache_usb_control_response(event)) {
                CTM_USB_RESPONSE cachedResponse = response;
                cachedResponse.request_id = 0;
                featureCache_[cacheKey] = cachedResponse;
            }
        }
        if (!handled || response.status != CTM_USB_RESPONSE_SUCCESS) {
            CtmMapRuntime::FeatureResponseExpectation expectation;
            bool hasExpectation = false;
            {
                std::lock_guard<std::mutex> guard(mapMutex_);
                hasExpectation = map_.feature_response_expectation(event, lastFeatureSet_, &expectation);
            }
            if (hasExpectation) {
                log_feature_get_response_rejected(
                    event,
                    lastFeatureSet_,
                    expectation,
                    physicalResponse,
                    physicalResponseLength);
            }
            return kStatusStall;
        }
        const size_t copy = (std::min<size_t>)(response.length, requestedLength);
        inData->assign(response.data, response.data + copy);
        return kStatusOk;
    }

    int handle_endpoint_out(uint8_t endpointAddress, const std::vector<uint8_t> &data)
    {
        CTM_USB_EVENT event = {};
        event.event_type = endpoint_is_iso(info_, endpointAddress) ? CTM_USB_EVENT_ISO_OUT : CTM_USB_EVENT_HID_OUTPUT;
        event.endpoint_address = endpointAddress;
        if (event.event_type == CTM_USB_EVENT_ISO_OUT) {
            return process_iso_output(endpointAddress, data);
        }
        event.length = static_cast<uint16_t>((std::min<size_t>)(sizeof(event.data), data.size()));
        if (event.length != 0) {
            memcpy(event.data, data.data(), event.length);
        }
        std::cout << "usb endpoint out"
                  << " ep=0x" << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned int>(endpointAddress)
                  << std::dec << std::setfill(' ')
                  << " len=" << event.length
                  << " head=" << hex_span(event.data, (std::min<size_t>)(event.length, 32))
                  << std::endl;
        return process_hid_output_event(event);
    }

    int process_iso_output(uint8_t endpointAddress, const std::vector<uint8_t> &data)
    {
        if (endpointAddress != audioIsoEndpoint_) {
            return kStatusOk;
        }
        if (audioChannels_ == 0 || audioInputSampleRate_ == 0 || audioReservoirSampleRate_ == 0) {
            return kStatusOk;
        }
        const size_t bytesPerFrame = static_cast<size_t>(audioChannels_) * sizeof(int16_t);
        if (bytesPerFrame == 0) {
            return kStatusOk;
        }
        const size_t frames = data.size() / bytesPerFrame;
        const size_t trailing = data.size() - frames * bytesPerFrame;
        if (trailing != 0) {
            audioTrailingBytes_.fetch_add(trailing, std::memory_order_relaxed);
        }
        if (frames == 0) {
            return kStatusOk;
        }

        std::vector<int16_t> source(frames * audioChannels_);
        for (size_t i = 0; i < source.size(); ++i) {
            source[i] = read_s16_le(data.data() + i * sizeof(int16_t));
        }

        const uint64_t outFrameCap =
            (static_cast<uint64_t>(frames) * audioReservoirSampleRate_ + audioInputSampleRate_ - 1) /
            audioInputSampleRate_ + 1;
        std::vector<int16_t> resampled(static_cast<size_t>(outFrameCap) * audioChannels_, 0);
        const size_t outFrames = ctm_resample_interleaved_linear(
            source.data(),
            frames,
            audioInputSampleRate_,
            audioReservoirSampleRate_,
            audioChannels_,
            resampled.data());
        if (outFrames == 0) {
            return kStatusOk;
        }

        audioReservoir_.push(resampled.data(), outFrames * audioChannels_);
        audioIsoEvents_.fetch_add(1, std::memory_order_relaxed);
        audioIsoBytes_.fetch_add(data.size(), std::memory_order_relaxed);
        audioInputFrames_.fetch_add(frames, std::memory_order_relaxed);
        audioReservoirFrames_.fetch_add(outFrames, std::memory_order_relaxed);
        return kStatusOk;
    }

    int process_hid_output_event(const CTM_USB_EVENT &event)
    {
        // Composite (puck): forward the host's OUT bytes verbatim to the sibling
        // interface that owns this OUT endpoint (tagged via request_id on the
        // wire). No map translation -- the virtual device IS the physical device.
        if (event.endpoint_address != 0 && !profile_.interface_report_descriptors.empty()) {
            std::vector<uint8_t> raw(event.data, event.data + event.length);
            std::wstring epError;
            if (!backend_->send_output_report_ep(raw, event.endpoint_address, false, &epError) &&
                ctm_verbose_logs()) {
                std::cout << "composite output forward failed ep=0x" << std::hex
                          << static_cast<unsigned int>(event.endpoint_address)
                          << std::dec << " len=" << event.length << std::endl;
            }
            return kStatusOk;
        }

        queue_virtual_input_reports(event);

        std::vector<uint8_t> output;
        bool ok = false;
        {
            std::lock_guard<std::mutex> guard(mapMutex_);
            ok = map_.translate_controller_output(event, &outputSeq_, &output);
        }
        if (!ok) {
            record_unknown_report("hid-output", event.report_id);
            if (ctm_verbose_logs()) {
                std::cout << "hid output unmapped"
                          << " endpoint=0x" << std::hex << std::setw(2) << std::setfill('0')
                          << static_cast<unsigned int>(event.endpoint_address)
                          << std::dec << std::setfill(' ')
                          << " len=" << event.length << std::endl;
            }
            return kStatusOk;
        }
        if (!output.empty() && output[0] == 0x31) {
            std::lock_guard<std::mutex> guard(lastControllerOutputMutex_);
            lastControllerOutput_ = output;
        }
        std::wstring error;
        if (!backend_->send_output_report(output, false, &error)) {
            // Best-effort delivery: the host's OUT write is valid and must be
            // ACKed even when we cannot forward it to the physical controller.
            // Returning a STALL here makes the Windows host driver (e.g.
            // xboxgip) treat the OUT endpoint as broken and abandon device
            // init, which kills the input pipe entirely. Rumble/feedback is
            // non-essential; never break the virtual device over it.
            if (ctm_verbose_logs()) {
                std::wcerr << L"backend HID output failed (acked to host): " << error << L"\n";
            }
            return kStatusOk;
        }
        return kStatusOk;
    }

    int handle_endpoint_in(
        uint8_t endpointAddress,
        uint32_t transferLength,
        std::vector<uint8_t> *inData,
        CtmSubmitInfo *submitInfo)
    {
        if (endpoint_is_iso(info_, endpointAddress)) {
            if (submitInfo != nullptr) {
                submitInfo->endpointIso = true;
            }
            inData->assign(transferLength, 0);
            return kStatusOk;
        }

        const bool isInterrupt = endpoint_is_interrupt(info_, endpointAddress);
        if (submitInfo != nullptr) {
            submitInfo->endpointInterrupt = isInterrupt;
        }
        if (isInterrupt) {
            return handle_interrupt_in(endpointAddress, transferLength, inData, submitInfo, nullptr);
        }

        inData->assign((std::min<uint32_t>)(transferLength, 64), 0);
        if (submitInfo != nullptr) {
            submitInfo->inputReply = InputReplyKind::Zero;
            submitInfo->inputWaitUs = 0;
        }
        return kStatusOk;
    }

    CtmDescriptorProfile profile_;
    CtmMapRuntime map_;
    UsbDeviceInfo info_;
    CtmBackend *backend_ = nullptr;
    std::mutex mapMutex_;
    std::mutex inputMutex_;
    std::condition_variable inputCv_;
    bool hasInput_ = false;
    uint32_t inputSequence_ = 0;
    CTM_INPUT_REPORT latestInput_ = {};
    std::deque<QueuedInputReport> pendingInputReports_;
    std::map<uint8_t, InputEndpointState> inputEndpointStates_;
    std::array<bool, 256> compInLogged_ = {};    // diag: first input report seen per endpoint
    std::array<bool, 256> compPollLogged_ = {};  // diag: first interrupt-IN poll seen per endpoint
    uint32_t nextRequestId_ = 1;
    uint8_t hidIdle_ = 0;
    uint8_t hidProtocol_ = 1;
    uint8_t configuration_ = 1;
    std::array<uint8_t, 32> interfaceAlternate_ = {};
    uint8_t outputSeq_ = 0;
    std::mutex lastControllerOutputMutex_;
    std::vector<uint8_t> lastControllerOutput_;
    std::vector<uint8_t> lastFeatureSet_;
    std::vector<uint8_t> physicalFeatureScratch_;
    std::map<FeatureCacheKey, CTM_USB_RESPONSE> featureCache_;
    std::mutex unknownLogMutex_;
    std::map<std::string, uint64_t> unknownLogCounts_;
    std::chrono::steady_clock::time_point unknownLogLastFlush_;
    std::mutex input31LogMutex_;
    std::vector<uint8_t> input31Baseline_;
    std::vector<uint8_t> input31Last_;
    std::array<uint64_t, 512> input31DeltaCounts_ = {};
    std::chrono::steady_clock::time_point input31LastLog_;
    uint64_t input31FramesSinceLog_ = 0;
    uint64_t input31ChangedFramesSinceLog_ = 0;
    std::mutex mappedInputLogMutex_;
    std::chrono::steady_clock::time_point mappedInputLastLog_;
    uint64_t mappedInputFramesSinceLog_ = 0;
    uint8_t audioIsoEndpoint_ = 0x01;
    uint32_t audioInputSampleRate_ = 48000;
    uint32_t audioReservoirSampleRate_ = 48000;
    uint8_t audioChannels_ = 4;
    uint16_t audioFrameSamples_ = 480;
    uint16_t audioExpectedLength_ = 3840;
    CtmPcmReservoir audioReservoir_;
    std::mutex audioThreadMutex_;
    std::thread audioBuilderThread_;
    std::atomic_bool audioThreadRunning_{false};
    std::vector<std::vector<uint8_t>> connectFeatureRequests_;
    std::atomic<uint64_t> audioIsoEvents_{0};
    std::atomic<uint64_t> audioIsoBytes_{0};
    std::atomic<uint64_t> audioInputFrames_{0};
    std::atomic<uint64_t> audioReservoirFrames_{0};
    std::atomic<uint64_t> audioChunksBuilt_{0};
    std::atomic<uint64_t> audioBuildFails_{0};
    std::atomic<uint64_t> audioSendFails_{0};
    std::atomic<uint64_t> audioTrailingBytes_{0};
};
