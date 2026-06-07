// Additive ENet/UDP transport for the CTM bridge. Selected at runtime via the
// --enet flag; when it is off the TCP BridgeBackend (bridge.inl) is used and
// nothing here runs. This backend speaks the exact same CtmBridgeProtocol
// (header + message enum + DeviceCaps/HostConfig) as the TCP path, but carries
// one protocol message per reliable ENet packet on a single channel instead of
// a length-prefixed TCP stream. It satisfies the same CtmBackend interface the
// device/agent already drives (start/stop/caps/send_output_report/
// execute_feature_actions), so the rest of ctm-usbip is unchanged.
//
// Requires bridge.inl (CtmBridgeProtocol, CtmBridgeMessage, monotonic_us) to be
// included first, and enet/enet.h to be available. enet_initialize() /
// enet_deinitialize() are owned by the process (main.cpp), not this class.

class EnetBridgeBackend final : public CtmBackend {
public:
    static constexpr enet_uint8 kChannel = 0;
    static constexpr size_t kChannelCount = 1;

    EnetBridgeBackend(uint16_t port, double btPaceMs)
        : port_(port), btPaceMs_(btPaceMs)
    {
    }

    bool start(RawInputCallback callback, std::wstring *error) override
    {
        callback_ = std::move(callback);

        ENetAddress address = {};
        address.host = ENET_HOST_ANY;
        address.port = port_;
        // 1 peer: a single bridge client at a time, mirroring the TCP server's
        // listen(1)/single-client model. kChannelCount reliable channel(s).
        // Retry the bind briefly: when a session is replaced (the agent stops
        // the old session and starts a new one on the same port) or a prior
        // instance is still exiting, the UDP port can stay held for a few ms
        // while the old socket finishes closing. enet_host_create has no
        // REUSEADDR option, so poll for the port (~3s) rather than failing the
        // whole session on a transient WSAEADDRINUSE.
        host_ = enet_host_create(&address, 1, kChannelCount, 0, 0);
        for (int attempt = 1; host_ == nullptr && attempt < 30 && !g_stop.load(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            host_ = enet_host_create(&address, 1, kChannelCount, 0, 0);
        }
        if (host_ == nullptr) {
            if (error) {
                *error = L"enet bridge host create failed (udp port " +
                         std::to_wstring(port_) + L", wsa=" +
                         std::to_wstring(WSAGetLastError()) + L")";
            }
            return false;
        }

        if (!accept_client(true, error)) {
            enet_host_destroy(host_);
            host_ = nullptr;
            return false;
        }

        running_.store(true);
        inputWorker_ = std::thread([this]() { input_worker_loop(); });
        serviceThread_ = std::thread([this]() { service_loop(); });
        return true;
    }

    void stop() override
    {
        running_.store(false);
        if (serviceThread_.joinable()) {
            serviceThread_.join();
        }
        inputCv_.notify_all();
        if (inputWorker_.joinable()) {
            inputWorker_.join();
        }
        // The service thread owns the host; it is gone now, so tear the host
        // down here.
        if (host_ != nullptr) {
            if (peer_ != nullptr) {
                enet_peer_disconnect_now(peer_, 0);
                peer_ = nullptr;
            }
            enet_host_destroy(host_);
            host_ = nullptr;
        }
        featureCv_.notify_all();
    }

    BackendCaps caps() const override
    {
        BackendCaps caps;
        caps.vendorId = capsRaw_.vendor_id ? capsRaw_.vendor_id : 0x054c;
        caps.productId = capsRaw_.product_id ? capsRaw_.product_id : 0x0ce6;
        caps.version = capsRaw_.version ? capsRaw_.version : 0x0100;
        caps.inputReportLength = capsRaw_.input_report_len ? capsRaw_.input_report_len : 64;
        caps.outputReportLength = capsRaw_.output_report_len ? capsRaw_.output_report_len : 64;
        caps.featureReportLength = capsRaw_.feature_report_len ? capsRaw_.feature_report_len : 64;
        caps.serial = widen_ascii(capsRaw_.serial, sizeof(capsRaw_.serial));
        caps.product = widen_ascii(capsRaw_.product, sizeof(capsRaw_.product));
        caps.path = widen_ascii(capsRaw_.path, sizeof(capsRaw_.path));
        caps.hidReportDescriptor = hidReportDescriptor_;
        return caps;
    }

    bool execute_feature_actions(
        const std::vector<CtmMapRuntime::PhysicalFeatureAction> &actions,
        std::vector<uint8_t> *scratch,
        const uint8_t **lastGetResponse,
        size_t *lastGetResponseLength,
        const char *reason,
        unsigned int timeoutMs) override
    {
        if (scratch == nullptr || lastGetResponse == nullptr || lastGetResponseLength == nullptr || actions.empty()) {
            return false;
        }
        *lastGetResponse = nullptr;
        *lastGetResponseLength = 0;
        for (const auto &action : actions) {
            if (action.length == 0 || 1 + action.payload.size() > action.length) {
                return false;
            }
            scratch->assign(action.length, 0);
            (*scratch)[0] = action.report;
            if (!action.payload.empty()) {
                memcpy(scratch->data() + 1, action.payload.data(), action.payload.size());
            }
            if (action.operation == CtmMapRuntime::PhysicalFeatureOperation::SetFeature) {
                if (!remote_feature(false, scratch->data(), scratch->size(), timeoutMs, scratch, reason)) {
                    if (action.bestEffort) continue;
                    return false;
                }
            } else {
                if (!remote_feature(true, scratch->data(), scratch->size(), timeoutMs, scratch, reason)) {
                    if (action.bestEffort) continue;
                    return false;
                }
                *lastGetResponse = scratch->data();
                *lastGetResponseLength = scratch->size();
            }
        }
        return *lastGetResponse != nullptr || !actions.empty();
    }

    bool send_output_report(const std::vector<uint8_t> &report, bool paced, std::wstring *error) override
    {
        if (!connected_.load()) {
            return true;
        }
        if (send_message(
            CtmBridgeProtocol::MsgOutputReport,
            paced ? CtmBridgeProtocol::kFlagPaced : 0,
            0,
            report.data(),
            report.size(),
            error)) {
            return true;
        }
        return !connected_.load();
    }

    bool send_output_report_ep(const std::vector<uint8_t> &report, uint8_t endpoint, bool paced, std::wstring *error) override
    {
        if (!connected_.load()) {
            return true;
        }
        if (send_message(
            CtmBridgeProtocol::MsgOutputReport,
            paced ? CtmBridgeProtocol::kFlagPaced : 0,
            endpoint,
            report.data(),
            report.size(),
            error)) {
            return true;
        }
        return !connected_.load();
    }

private:
    // One queued outbound message handed from any thread to the single service
    // thread, which owns the ENetHost. ENet hosts are not thread-safe, so all
    // enet_host_*/enet_peer_* calls happen on serviceThread_.
    struct OutgoingMessage {
        CtmBridgeProtocol::Header header = {};
        std::vector<uint8_t> payload;
    };

    bool send_message(
        uint16_t type,
        uint32_t flags,
        uint32_t requestId,
        const uint8_t *payload,
        size_t payloadLength,
        std::wstring *error)
    {
        if (payloadLength > CtmBridgeProtocol::kMaxPayload) {
            if (error) *error = L"bridge payload too large";
            return false;
        }
        if (!connected_.load()) {
            if (error) *error = L"bridge enet peer closed";
            return false;
        }
        OutgoingMessage message;
        message.header.magic = CtmBridgeProtocol::kMagic;
        message.header.version = CtmBridgeProtocol::kVersion;
        message.header.type = type;
        message.header.flags = flags;
        message.header.sequence = sendSequence_.fetch_add(1) + 1;
        message.header.timestamp_us = monotonic_us();
        message.header.request_id = requestId;
        message.header.payload_len = static_cast<uint32_t>(payloadLength);
        if (payloadLength != 0 && payload != nullptr) {
            message.payload.assign(payload, payload + payloadLength);
        }
        {
            std::lock_guard<std::mutex> lock(outMutex_);
            outQueue_.push_back(std::move(message));
        }
        // The service thread (service_loop) owns the ENetHost and is not
        // thread-safe to touch from here. It flushes outQueue_ on its 1 ms
        // service tick, which bounds added output latency.
        return true;
    }

    // Serialize header+payload into one contiguous buffer and hand it to ENet as
    // a single reliable packet. Caller must be on the service thread.
    bool dispatch_outgoing_locked(const OutgoingMessage &message)
    {
        if (peer_ == nullptr) {
            return false;
        }
        std::vector<uint8_t> wire(sizeof(CtmBridgeProtocol::Header) + message.payload.size());
        memcpy(wire.data(), &message.header, sizeof(CtmBridgeProtocol::Header));
        if (!message.payload.empty()) {
            memcpy(wire.data() + sizeof(CtmBridgeProtocol::Header),
                   message.payload.data(),
                   message.payload.size());
        }
        ENetPacket *packet = enet_packet_create(wire.data(), wire.size(), ENET_PACKET_FLAG_RELIABLE);
        if (packet == nullptr) {
            return false;
        }
        if (enet_peer_send(peer_, kChannel, packet) < 0) {
            enet_packet_destroy(packet);
            return false;
        }
        return true;
    }

    // Drain the outbound queue onto the peer. Service-thread only.
    void flush_outgoing()
    {
        std::deque<OutgoingMessage> pending;
        {
            std::lock_guard<std::mutex> lock(outMutex_);
            pending.swap(outQueue_);
        }
        for (const OutgoingMessage &message : pending) {
            (void)dispatch_outgoing_locked(message);
        }
    }

    bool wait_for_message(CtmBridgeMessage *message, std::wstring *error, unsigned int timeoutMs)
    {
        // Service-thread-only blocking receive used during the handshake.
        const uint64_t startUs = monotonic_us();
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(inMutex_);
                if (!inQueue_.empty()) {
                    *message = std::move(inQueue_.front());
                    inQueue_.pop_front();
                    return true;
                }
            }
            if (!connected_.load() && handshakeDone_) {
                if (error) *error = L"bridge enet peer closed";
                return false;
            }
            ENetEvent event = {};
            int rc = enet_host_service(host_, &event, 5);
            if (rc < 0) {
                if (error) *error = L"enet host service failed";
                return false;
            }
            if (rc > 0) {
                handle_service_event(event);
            }
            if (timeoutMs != 0 && monotonic_us() - startUs >= static_cast<uint64_t>(timeoutMs) * 1000ull) {
                if (error) *error = L"bridge enet receive timeout";
                return false;
            }
            if (g_stop.load()) {
                if (error) *error = L"stopping";
                return false;
            }
        }
    }

    // Translate a raw ENet event into our protocol queues. Service-thread only.
    void handle_service_event(ENetEvent &event)
    {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT:
            peer_ = event.peer;
            connected_.store(true);
            break;
        case ENET_EVENT_TYPE_RECEIVE: {
            ingest_packet(event.packet->data, event.packet->dataLength);
            enet_packet_destroy(event.packet);
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT:
            connected_.store(false);
            peer_ = nullptr;
            break;
        default:
            break;
        }
    }

    void ingest_packet(const enet_uint8 *data, size_t length)
    {
        if (data == nullptr || length < sizeof(CtmBridgeProtocol::Header)) {
            return;
        }
        CtmBridgeMessage message;
        memcpy(&message.header, data, sizeof(CtmBridgeProtocol::Header));
        if (message.header.magic != CtmBridgeProtocol::kMagic ||
            message.header.version != CtmBridgeProtocol::kVersion ||
            message.header.payload_len > CtmBridgeProtocol::kMaxPayload) {
            return;
        }
        const size_t expected = sizeof(CtmBridgeProtocol::Header) + message.header.payload_len;
        if (length < expected) {
            return;
        }
        if (message.header.payload_len != 0) {
            message.payload.assign(
                data + sizeof(CtmBridgeProtocol::Header),
                data + sizeof(CtmBridgeProtocol::Header) + message.header.payload_len);
        }
        std::lock_guard<std::mutex> lock(inMutex_);
        inQueue_.push_back(std::move(message));
    }

    bool accept_client(bool initial, std::wstring *error)
    {
        for (;;) {
            if (!initial && (!running_.load() || g_stop.load())) {
                return false;
            }
            std::wcout << L"bridge waiting on ENet/UDP port " << port_ << L"\n";

            // Wait for a CONNECT.
            connected_.store(false);
            peer_ = nullptr;
            handshakeDone_ = false;
            {
                std::lock_guard<std::mutex> lock(inMutex_);
                inQueue_.clear();
            }
            while (!connected_.load()) {
                if (!initial && (!running_.load() || g_stop.load())) {
                    return false;
                }
                if (initial && g_stop.load()) {
                    if (error) *error = L"stopping";
                    return false;
                }
                ENetEvent event = {};
                int rc = enet_host_service(host_, &event, 100);
                if (rc < 0) {
                    if (error) *error = L"enet host service failed";
                    return false;
                }
                if (rc > 0) {
                    handle_service_event(event);
                }
            }

            // Expect a Hello as the first message.
            CtmBridgeMessage hello;
            std::wstring helloError;
            if (!wait_for_message(&hello, &helloError, 5000) ||
                hello.header.type != CtmBridgeProtocol::MsgHello ||
                hello.payload.size() < sizeof(CtmBridgeProtocol::DeviceCaps)) {
                reset_peer();
                if (initial) {
                    if (error) *error = helloError.empty() ? L"bridge enet hello failed" : helloError;
                    return false;
                }
                std::wcerr << L"bridge enet hello failed: "
                           << (helloError.empty() ? L"bad hello" : helloError) << L"\n";
                continue;
            }

            CtmBridgeProtocol::DeviceCaps peerCaps = {};
            memcpy(&peerCaps, hello.payload.data(), sizeof(peerCaps));
            if (initial) {
                capsRaw_ = peerCaps;
                hidReportDescriptor_.clear();
                if (hello.payload.size() >= sizeof(CtmBridgeProtocol::DeviceCaps) + sizeof(CtmBridgeProtocol::HidDescriptorInfo)) {
                    CtmBridgeProtocol::HidDescriptorInfo descInfo = {};
                    memcpy(
                        &descInfo,
                        hello.payload.data() + sizeof(CtmBridgeProtocol::DeviceCaps),
                        sizeof(descInfo));
                    const size_t descriptorOffset =
                        sizeof(CtmBridgeProtocol::DeviceCaps) + sizeof(CtmBridgeProtocol::HidDescriptorInfo);
                    if (descInfo.report_descriptor_len != 0 &&
                        descInfo.report_descriptor_len <= CtmBridgeProtocol::kMaxPayload &&
                        descriptorOffset + descInfo.report_descriptor_len <= hello.payload.size()) {
                        hidReportDescriptor_.assign(
                            hello.payload.begin() + static_cast<std::ptrdiff_t>(descriptorOffset),
                            hello.payload.begin() + static_cast<std::ptrdiff_t>(descriptorOffset + descInfo.report_descriptor_len));
                    }
                }
            }

            CtmBridgeProtocol::HostConfig hostConfig = {};
            hostConfig.bt_pace_us = static_cast<uint32_t>(btPaceMs_ * 1000.0 + 0.5);
            hostConfig.input_report_len = capsRaw_.input_report_len;
            hostConfig.output_report_len = capsRaw_.output_report_len;
            hostConfig.feature_report_len = capsRaw_.feature_report_len;
            hostConfig.paced_report_count = 2;
            hostConfig.paced_report_ids[0] = 0x36;
            hostConfig.paced_report_ids[1] = 0x15;

            // The host config goes out on the same reliable channel; flush it
            // immediately so the client can start sending input.
            OutgoingMessage configMessage;
            configMessage.header.magic = CtmBridgeProtocol::kMagic;
            configMessage.header.version = CtmBridgeProtocol::kVersion;
            configMessage.header.type = CtmBridgeProtocol::MsgHostConfig;
            configMessage.header.flags = CtmBridgeProtocol::kFlagOk;
            configMessage.header.sequence = sendSequence_.fetch_add(1) + 1;
            configMessage.header.timestamp_us = monotonic_us();
            configMessage.header.payload_len = sizeof(hostConfig);
            configMessage.payload.assign(
                reinterpret_cast<const uint8_t *>(&hostConfig),
                reinterpret_cast<const uint8_t *>(&hostConfig) + sizeof(hostConfig));
            if (!dispatch_outgoing_locked(configMessage)) {
                reset_peer();
                if (initial) {
                    if (error) *error = L"bridge enet host config send failed";
                    return false;
                }
                std::wcerr << L"bridge enet host config failed\n";
                continue;
            }
            enet_host_flush(host_);

            handshakeDone_ = true;
            std::wcout << L"bridge enet backend"
                       << (initial ? L"" : L" reconnected")
                       << L" product=" << widen_ascii(peerCaps.product, sizeof(peerCaps.product))
                       << L" serial=" << widen_ascii(peerCaps.serial, sizeof(peerCaps.serial)) << L"\n";
            if (!initial && reconnectCallback_) {
                reconnectCallback_();
            }
            return true;
        }
    }

    void reset_peer()
    {
        if (peer_ != nullptr) {
            enet_peer_reset(peer_);
            peer_ = nullptr;
        }
        connected_.store(false);
        handshakeDone_ = false;
        featureCv_.notify_all();
    }

    void service_loop()
    {
        uint64_t lastInputReceiveUs = 0;
        while (running_.load() && !g_stop.load()) {
            // Push any pending outbound work first, then pump the host. The 1 ms
            // timeout keeps output latency low while still parking the thread.
            flush_outgoing();
            ENetEvent event = {};
            int rc = enet_host_service(host_, &event, 1);
            if (rc < 0) {
                if (running_.load() && !g_stop.load()) {
                    std::wcerr << L"bridge enet service failed\n";
                }
                break;
            }
            bool linkLost = false;
            // enet_host_service() returns at most one event; drain the rest of
            // the events it queued this tick with enet_host_check_events so a
            // burst of input packets is not throttled to one-per-millisecond.
            while (rc > 0) {
                const bool wasConnected = connected_.load();
                handle_service_event(event);
                if (wasConnected && event.type == ENET_EVENT_TYPE_DISCONNECT) {
                    linkLost = true;
                    break;
                }
                rc = enet_host_check_events(host_, &event);
                if (rc < 0) {
                    break;
                }
            }
            if (linkLost) {
                on_link_lost();
                lastInputReceiveUs = 0;
                if (!accept_client(false, nullptr)) {
                    break;
                }
                continue;
            }

            // Drain decoded protocol messages.
            for (;;) {
                CtmBridgeMessage message;
                {
                    std::lock_guard<std::mutex> lock(inMutex_);
                    if (inQueue_.empty()) {
                        break;
                    }
                    message = std::move(inQueue_.front());
                    inQueue_.pop_front();
                }
                if (message.header.type == CtmBridgeProtocol::MsgInputReport) {
                    if (!message.payload.empty()) {
                        const uint64_t receivedUs = monotonic_us();
                        const uint64_t deltaUs = lastInputReceiveUs == 0 ? 0 : receivedUs - lastInputReceiveUs;
                        lastInputReceiveUs = receivedUs;
                        enqueue_input_report(std::move(message.payload),
                                             static_cast<uint8_t>(message.header.request_id),
                                             receivedUs, deltaUs);
                    }
                } else if (message.header.type == CtmBridgeProtocol::MsgFeatureReport) {
                    const bool ok = (message.header.flags & CtmBridgeProtocol::kFlagOk) != 0;
                    {
                        std::lock_guard<std::mutex> lock(featureMutex_);
                        featureReplies_[message.header.request_id] = std::make_pair(ok, std::move(message.payload));
                    }
                    featureCv_.notify_all();
                } else if (message.header.type == CtmBridgeProtocol::MsgLog ||
                           message.header.type == CtmBridgeProtocol::MsgError) {
                    std::string text(message.payload.begin(), message.payload.end());
                    std::cout << "bridge enet peer "
                              << (message.header.type == CtmBridgeProtocol::MsgError ? "error " : "log ")
                              << text << std::endl;
                }
            }
        }
    }

    // Explicit "unplugged" transition on link loss for the ENet transport. The
    // virtual USB device is detached by the owner (agent/main) via the supplied
    // callback, mirroring how BRIDGE_STOP detaches over the USB/IP server.
    void on_link_lost()
    {
        connected_.store(false);
        featureCv_.notify_all();
        std::wcout << L"bridge enet link lost port=" << port_
                   << L" -> virtual device UNPLUGGED (detaching)\n";
        if (disconnectCallback_) {
            disconnectCallback_();
        }
    }

    struct QueuedInputReport {
        std::vector<uint8_t> payload;
        uint8_t endpoint = 0;   // physical IN endpoint (composite puck); 0 = default
        uint64_t receivedUs = 0;
        uint64_t tcpDeltaUs = 0;
    };

    static void atomic_max_u64(std::atomic<uint64_t> *target, uint64_t value)
    {
        if (target == nullptr) {
            return;
        }
        uint64_t current = target->load(std::memory_order_relaxed);
        while (current < value &&
               !target->compare_exchange_weak(current, value, std::memory_order_relaxed)) {
        }
    }

    void enqueue_input_report(std::vector<uint8_t> &&payload, uint8_t endpoint, uint64_t receivedUs, uint64_t tcpDeltaUs)
    {
        if (payload.empty()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(inputMutex_);
            if (inputQueue_.size() >= kInputQueueCapacity) {
                inputQueue_.pop_front();
                inputDropped_.fetch_add(1, std::memory_order_relaxed);
            }
            QueuedInputReport item;
            item.payload = std::move(payload);
            item.endpoint = endpoint;
            item.receivedUs = receivedUs;
            item.tcpDeltaUs = tcpDeltaUs;
            inputQueue_.push_back(std::move(item));
        }
        inputQueued_.fetch_add(1, std::memory_order_relaxed);
        inputDeltaUsTotal_.fetch_add(tcpDeltaUs, std::memory_order_relaxed);
        atomic_max_u64(&inputDeltaUsMax_, tcpDeltaUs);
        inputCv_.notify_one();
    }

    void input_worker_loop()
    {
        using clock = std::chrono::steady_clock;
        auto lastStatus = clock::now();
        uint64_t lastQueued = 0;
        uint64_t lastProcessed = 0;
        uint64_t lastDropped = 0;
        uint64_t lastDeltaUsTotal = 0;
        uint64_t lastQueueDelayUsTotal = 0;
        uint64_t lastCallbackUsTotal = 0;

        while (running_.load() && !g_stop.load()) {
            QueuedInputReport item;
            {
                std::unique_lock<std::mutex> lock(inputMutex_);
                inputCv_.wait(lock, [&]() {
                    return !inputQueue_.empty() || !running_.load() || g_stop.load();
                });
                if (inputQueue_.empty()) {
                    break;
                }
                item = std::move(inputQueue_.front());
                inputQueue_.pop_front();
            }

            const uint64_t callbackStartUs = monotonic_us();
            const uint64_t queueDelayUs = callbackStartUs > item.receivedUs ? callbackStartUs - item.receivedUs : 0;
            inputQueueDelayUsTotal_.fetch_add(queueDelayUs, std::memory_order_relaxed);
            atomic_max_u64(&inputQueueDelayUsMax_, queueDelayUs);
            if (callback_) {
                callback_(item.payload.data(), item.payload.size(), item.endpoint);
            }
            const uint64_t callbackUs = monotonic_us() - callbackStartUs;
            inputCallbackUsTotal_.fetch_add(callbackUs, std::memory_order_relaxed);
            atomic_max_u64(&inputCallbackUsMax_, callbackUs);
            inputProcessed_.fetch_add(1, std::memory_order_relaxed);

            const auto now = clock::now();
            if (now - lastStatus >= std::chrono::seconds(2)) {
                const double seconds = std::chrono::duration<double>(now - lastStatus).count();
                const uint64_t queued = inputQueued_.load(std::memory_order_relaxed);
                const uint64_t processed = inputProcessed_.load(std::memory_order_relaxed);
                const uint64_t dropped = inputDropped_.load(std::memory_order_relaxed);
                const uint64_t deltaUsTotal = inputDeltaUsTotal_.load(std::memory_order_relaxed);
                const uint64_t queueDelayUsTotal = inputQueueDelayUsTotal_.load(std::memory_order_relaxed);
                const uint64_t callbackUsTotal = inputCallbackUsTotal_.load(std::memory_order_relaxed);
                const uint64_t queuedDelta = queued - lastQueued;
                const uint64_t processedDelta = processed - lastProcessed;
                const uint64_t droppedDelta = dropped - lastDropped;
                const uint64_t deltaUsDelta = deltaUsTotal - lastDeltaUsTotal;
                const uint64_t queueDelayUsDelta = queueDelayUsTotal - lastQueueDelayUsTotal;
                const uint64_t callbackUsDelta = callbackUsTotal - lastCallbackUsTotal;
                size_t depth = 0;
                {
                    std::lock_guard<std::mutex> lock(inputMutex_);
                    depth = inputQueue_.size();
                }
                const uint64_t gapMaxUs = inputDeltaUsMax_.exchange(0, std::memory_order_relaxed);
                const uint64_t queueMaxUs = inputQueueDelayUsMax_.exchange(0, std::memory_order_relaxed);
                const uint64_t callbackMaxUs = inputCallbackUsMax_.exchange(0, std::memory_order_relaxed);

                std::cout << "bridge enet input"
                          << " rx_hz=" << std::fixed << std::setprecision(1)
                          << static_cast<double>(queuedDelta) / seconds
                          << " callback_hz=" << static_cast<double>(processedDelta) / seconds
                          << " drops=" << droppedDelta
                          << " depth=" << depth
                          << " net_gap_avg_ms=" << std::fixed << std::setprecision(3)
                          << (queuedDelta == 0 ? 0.0 : static_cast<double>(deltaUsDelta) / 1000.0 / queuedDelta)
                          << " net_gap_max_ms=" << static_cast<double>(gapMaxUs) / 1000.0
                          << " queue_wait_avg_ms="
                          << (processedDelta == 0 ? 0.0 : static_cast<double>(queueDelayUsDelta) / 1000.0 / processedDelta)
                          << " queue_wait_max_ms=" << static_cast<double>(queueMaxUs) / 1000.0
                          << " callback_avg_us="
                          << (processedDelta == 0 ? 0.0 : static_cast<double>(callbackUsDelta) / processedDelta)
                          << " callback_max_us=" << static_cast<double>(callbackMaxUs)
                          << std::defaultfloat
                          << std::endl;

                lastQueued = queued;
                lastProcessed = processed;
                lastDropped = dropped;
                lastDeltaUsTotal = deltaUsTotal;
                lastQueueDelayUsTotal = queueDelayUsTotal;
                lastCallbackUsTotal = callbackUsTotal;
                lastStatus = now;
            }
        }
    }

    bool remote_feature(
        bool get,
        const uint8_t *request,
        size_t requestLength,
        unsigned int timeoutMs,
        std::vector<uint8_t> *response,
        const char *reason)
    {
        if (request == nullptr || requestLength == 0 || response == nullptr) {
            return false;
        }
        const uint32_t requestId = nextRequestId_.fetch_add(1);
        std::wstring error;
        if (!send_message(
                get ? CtmBridgeProtocol::MsgFeatureGet : CtmBridgeProtocol::MsgFeatureSet,
                0,
                requestId,
                request,
                requestLength,
                &error)) {
            std::wcerr << L"bridge enet feature send failed: " << error << L"\n";
            return false;
        }
        std::unique_lock<std::mutex> lock(featureMutex_);
        const bool signaled = featureCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&]() {
            return featureReplies_.find(requestId) != featureReplies_.end() ||
                   g_stop.load() ||
                   !connected_.load();
        });
        if (!signaled || g_stop.load() || !connected_.load()) {
            std::cout << "bridge enet feature issue reason=" << reason
                      << " op=" << (get ? "get-timeout" : "set-timeout")
                      << " report=0x" << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned int>(request[0])
                      << std::dec << std::setfill(' ') << std::endl;
            featureReplies_.erase(requestId);
            return false;
        }
        auto it = featureReplies_.find(requestId);
        const bool ok = it->second.first;
        if (ok) {
            *response = std::move(it->second.second);
        }
        featureReplies_.erase(it);
        return ok;
    }

public:
    // Optional hooks the owner (agent/main.cpp) sets to drive the explicit
    // plug-out / plug-in of the virtual USB device on link transitions. Both
    // default to empty (no-op) so this stays additive.
    void set_disconnect_callback(std::function<void()> callback)
    {
        disconnectCallback_ = std::move(callback);
    }
    void set_reconnect_callback(std::function<void()> callback)
    {
        reconnectCallback_ = std::move(callback);
    }

private:
    uint16_t port_ = 0;
    double btPaceMs_ = 10.0;
    ENetHost *host_ = nullptr;
    ENetPeer *peer_ = nullptr;
    std::atomic_bool connected_{false};
    bool handshakeDone_ = false;
    CtmBridgeProtocol::DeviceCaps capsRaw_ = {};
    std::vector<uint8_t> hidReportDescriptor_;
    RawInputCallback callback_;
    std::function<void()> disconnectCallback_;
    std::function<void()> reconnectCallback_;
    std::atomic_bool running_{false};
    std::thread serviceThread_;
    std::thread inputWorker_;

    std::mutex outMutex_;
    std::deque<OutgoingMessage> outQueue_;
    std::mutex inMutex_;
    std::deque<CtmBridgeMessage> inQueue_;

    static constexpr size_t kInputQueueCapacity = 16;
    std::mutex inputMutex_;
    std::condition_variable inputCv_;
    std::deque<QueuedInputReport> inputQueue_;
    std::atomic<uint64_t> inputQueued_{0};
    std::atomic<uint64_t> inputProcessed_{0};
    std::atomic<uint64_t> inputDropped_{0};
    std::atomic<uint64_t> inputDeltaUsTotal_{0};
    std::atomic<uint64_t> inputDeltaUsMax_{0};
    std::atomic<uint64_t> inputQueueDelayUsTotal_{0};
    std::atomic<uint64_t> inputQueueDelayUsMax_{0};
    std::atomic<uint64_t> inputCallbackUsTotal_{0};
    std::atomic<uint64_t> inputCallbackUsMax_{0};

    std::atomic<uint32_t> sendSequence_{0};
    std::atomic<uint32_t> nextRequestId_{1};
    std::mutex featureMutex_;
    std::condition_variable featureCv_;
    std::map<uint32_t, std::pair<bool, std::vector<uint8_t>>> featureReplies_;
};
