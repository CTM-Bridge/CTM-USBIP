namespace CtmBridgeProtocol {
    constexpr uint32_t kMagic = 0x54424d43u;
    constexpr uint16_t kVersion = 1;
    constexpr uint32_t kFlagOk = 0x00000001u;
    constexpr uint32_t kFlagPaced = 0x00000002u;
    constexpr size_t kMaxPayload = 65536;

    enum MessageType : uint16_t {
        MsgHello = 1,
        MsgHostConfig = 2,
        MsgInputReport = 3,
        MsgOutputReport = 4,
        MsgFeatureGet = 5,
        MsgFeatureReport = 6,
        MsgLog = 7,
        MsgError = 8,
        MsgFeatureSet = 9,
        MsgEnum = 10,           // forwarded composite USB enumeration (puck)
    };

#pragma pack(push, 1)
    struct Header {
        uint32_t magic;
        uint16_t version;
        uint16_t type;
        uint32_t flags;
        uint32_t sequence;
        uint64_t timestamp_us;
        uint32_t request_id;
        uint32_t payload_len;
    };

    struct DeviceCaps {
        uint16_t vendor_id;
        uint16_t product_id;
        uint16_t version;
        uint16_t bus;
        uint16_t input_report_len;
        uint16_t output_report_len;
        uint16_t feature_report_len;
        uint16_t flags;
        char path[64];
        char serial[64];
        char product[64];
        char manufacturer[64];
    };

    struct HidDescriptorInfo {
        uint32_t report_descriptor_len;
        uint8_t reserved[28];
    };

    struct HostConfig {
        uint32_t bt_pace_us;
        uint16_t input_report_len;
        uint16_t output_report_len;
        uint16_t feature_report_len;
        uint8_t paced_report_count;
        uint8_t paced_report_ids[16];
        uint8_t reserved[31];
    };

    // CTMB_MSG_ENUM payload (puck composite): the device's own enumeration,
    // forwarded verbatim by the TV and replayed here. Layout:
    //   [EnumInfo][descriptors blob: descriptors_len][ iface_count x (EnumIface + report_desc) ]
    // full_speed=1 => present the virtual device as full-speed (legal 64B CDC bulk).
    struct EnumInfo {
        uint16_t descriptors_len;
        uint8_t  iface_count;
        uint8_t  full_speed;
        uint8_t  reserved[28];
    };

    struct EnumIface {
        uint8_t  interface_number;
        uint8_t  iface_class;
        uint16_t report_desc_len;
    };
#pragma pack(pop)
}

struct CtmBridgeMessage {
    CtmBridgeProtocol::Header header = {};
    std::vector<uint8_t> payload;
};

static uint64_t monotonic_us()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

class BridgeBackend final : public CtmBackend {
public:
    BridgeBackend(uint16_t port, double btPaceMs)
        : port_(port), btPaceMs_(btPaceMs)
    {
    }

    bool start(RawInputCallback callback, std::wstring *error) override
    {
        callback_ = std::move(callback);
        WSADATA data = {};
        if (!wsaStarted_) {
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
                if (error) *error = wsa_error_message(L"WSAStartup failed");
                return false;
            }
            wsaStarted_ = true;
        }
        listenSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket_ == INVALID_SOCKET) {
            if (error) *error = wsa_error_message(L"socket failed");
            return false;
        }
        BOOL reuse = TRUE;
        setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port_);
        if (bind(listenSocket_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            if (error) *error = wsa_error_message(L"bridge bind failed");
            return false;
        }
        if (listen(listenSocket_, 1) == SOCKET_ERROR) {
            if (error) *error = wsa_error_message(L"bridge listen failed");
            return false;
        }
        if (!accept_client(true, error)) {
            return false;
        }

        running_.store(true);
        inputWorker_ = std::thread([this]() { input_worker_loop(); });
        reader_ = std::thread([this]() { reader_loop(); });
        return true;
    }

    void stop() override
    {
        running_.store(false);
        close_client_socket();
        if (listenSocket_ != INVALID_SOCKET) {
            closesocket(listenSocket_);
            listenSocket_ = INVALID_SOCKET;
        }
        if (reader_.joinable()) {
            reader_.join();
        }
        inputCv_.notify_all();
        if (inputWorker_.joinable()) {
            inputWorker_.join();
        }
        if (wsaStarted_) {
            WSACleanup();
            wsaStarted_ = false;
        }
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

    const std::vector<uint8_t> &enum_payload() const override { return enumPayload_; }

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
        if (clientSocket_.load() == INVALID_SOCKET) {
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
        return clientSocket_.load() == INVALID_SOCKET;
    }

    bool send_output_report_ep(const std::vector<uint8_t> &report, uint8_t endpoint, bool paced, std::wstring *error) override
    {
        if (clientSocket_.load() == INVALID_SOCKET) {
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
        return clientSocket_.load() == INVALID_SOCKET;
    }

    // Composite: forward a SET/GET_REPORT verbatim to one interface's hidraw.
    // The interface is encoded in the high byte of request_id; the low 24 bits
    // are the reply correlation id. The TV routes by interface and echoes the id.
    bool remote_interface_feature(uint8_t interface, bool get, const std::vector<uint8_t> &request,
                                  std::vector<uint8_t> *reply, unsigned int timeoutMs) override
    {
        if (request.empty() || reply == nullptr) {
            return false;
        }
        const uint32_t reqId = (static_cast<uint32_t>(interface) << 24) |
                               (nextRequestId_.fetch_add(1) & 0x00ffffffu);
        std::wstring error;
        if (!send_message(
                get ? CtmBridgeProtocol::MsgFeatureGet : CtmBridgeProtocol::MsgFeatureSet,
                0, reqId, request.data(), request.size(), &error)) {
            return false;
        }
        std::unique_lock<std::mutex> lock(featureMutex_);
        const bool signaled = featureCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&]() {
            return featureReplies_.find(reqId) != featureReplies_.end() ||
                   g_stop.load() || clientSocket_.load() == INVALID_SOCKET;
        });
        if (!signaled) {
            featureReplies_.erase(reqId);
            return false;
        }
        auto it = featureReplies_.find(reqId);
        if (it == featureReplies_.end()) {
            return false;
        }
        const bool ok = it->second.first;
        if (ok && get) {
            *reply = std::move(it->second.second);
        }
        featureReplies_.erase(it);
        return ok;
    }

private:
    void close_client_socket()
    {
        SOCKET client = clientSocket_.exchange(INVALID_SOCKET);
        if (client != INVALID_SOCKET) {
            shutdown(client, SD_BOTH);
            closesocket(client);
        }
        featureCv_.notify_all();
    }

    bool accept_client(bool initial, std::wstring *error)
    {
        for (;;) {
            if (!initial && (!running_.load() || g_stop.load())) {
                return false;
            }
            std::wcout << L"bridge waiting on TCP port " << port_ << L"\n";
            SOCKET client = accept(listenSocket_, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                if (!initial && (!running_.load() || g_stop.load())) {
                    return false;
                }
                std::wstring acceptError = wsa_error_message(L"bridge accept failed");
                if (initial) {
                    if (error) *error = acceptError;
                    return false;
                }
                std::wcerr << L"bridge accept failed, retrying: " << acceptError << L"\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }
            if (!initial && (!running_.load() || g_stop.load())) {
                closesocket(client);
                return false;
            }
            int noDelay = 1;
            setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&noDelay), sizeof(noDelay));
            clientSocket_.store(client);

            CtmBridgeMessage hello;
            std::wstring helloError;
            // The TV may send CTMB_MSG_ENUM (composite enumeration) BEFORE HELLO;
            // accept and skip a leading ENUM so the handshake still completes. The
            // composite builder consumes the enumeration in a later stage.
            bool gotHello = false;
            for (int skip = 0; skip < 4; ++skip) {
                if (!recv_message(&hello, &helloError)) break;
                if (hello.header.type == CtmBridgeProtocol::MsgEnum) {
                    std::wcout << L"bridge received composite enumeration ("
                               << hello.payload.size() << L" bytes)\n";
                    if (initial) enumPayload_ = hello.payload;   // stored for the composite builder
                    continue;
                }
                gotHello = (hello.header.type == CtmBridgeProtocol::MsgHello &&
                            hello.payload.size() >= sizeof(CtmBridgeProtocol::DeviceCaps));
                break;
            }
            if (!gotHello) {
                close_client_socket();
                if (initial) {
                    if (error) *error = helloError.empty() ? L"bridge hello failed" : helloError;
                    return false;
                }
                std::wcerr << L"bridge hello failed: "
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
            std::wstring sendError;
            if (!send_message(
                    CtmBridgeProtocol::MsgHostConfig,
                    CtmBridgeProtocol::kFlagOk,
                    0,
                    reinterpret_cast<const uint8_t *>(&hostConfig),
                    sizeof(hostConfig),
                    &sendError)) {
                close_client_socket();
                if (initial) {
                    if (error) *error = sendError;
                    return false;
                }
                std::wcerr << L"bridge host config failed: " << sendError << L"\n";
                continue;
            }

            std::wcout << L"bridge backend"
                       << (initial ? L"" : L" reconnected")
                       << L" product=" << widen_ascii(peerCaps.product, sizeof(peerCaps.product))
                       << L" serial=" << widen_ascii(peerCaps.serial, sizeof(peerCaps.serial)) << L"\n";
            return true;
        }
    }

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
        CtmBridgeProtocol::Header header = {};
        header.magic = CtmBridgeProtocol::kMagic;
        header.version = CtmBridgeProtocol::kVersion;
        header.type = type;
        header.flags = flags;
        header.sequence = sendSequence_.fetch_add(1) + 1;
        header.timestamp_us = monotonic_us();
        header.request_id = requestId;
        header.payload_len = static_cast<uint32_t>(payloadLength);
        std::lock_guard<std::mutex> guard(sendMutex_);
        SOCKET s = clientSocket_.load();
        if (s == INVALID_SOCKET) {
            if (error) *error = L"bridge socket closed";
            return false;
        }
        if (!send_all(s, reinterpret_cast<const uint8_t *>(&header), sizeof(header))) {
            if (error) *error = wsa_error_message(L"bridge send header failed");
            close_client_socket();
            return false;
        }
        if (payloadLength != 0 && !send_all(s, payload, payloadLength)) {
            if (error) *error = wsa_error_message(L"bridge send payload failed");
            close_client_socket();
            return false;
        }
        return true;
    }

    bool recv_message(CtmBridgeMessage *message, std::wstring *error)
    {
        if (message == nullptr) return false;
        message->payload.clear();
        SOCKET s = clientSocket_.load();
        if (s == INVALID_SOCKET) {
            if (error) *error = L"bridge socket closed";
            return false;
        }
        if (!recv_all(s, reinterpret_cast<uint8_t *>(&message->header), sizeof(message->header))) {
            if (error) *error = wsa_error_message(L"bridge recv header failed");
            return false;
        }
        if (message->header.magic != CtmBridgeProtocol::kMagic ||
            message->header.version != CtmBridgeProtocol::kVersion ||
            message->header.payload_len > CtmBridgeProtocol::kMaxPayload) {
            if (error) *error = L"bridge protocol header rejected";
            return false;
        }
        if (message->header.payload_len != 0) {
            message->payload.resize(message->header.payload_len);
            if (!recv_all(s, message->payload.data(), message->payload.size())) {
                if (error) *error = wsa_error_message(L"bridge recv payload failed");
                return false;
            }
        }
        return true;
    }

    void reader_loop()
    {
        uint64_t lastInputReceiveUs = 0;
        while (running_.load() && !g_stop.load()) {
            CtmBridgeMessage message;
            std::wstring error;
            if (!recv_message(&message, &error)) {
                if (running_.load() && !g_stop.load()) {
                    std::wcerr << L"bridge client disconnected: " << error << L"\n";
                }
                close_client_socket();
                lastInputReceiveUs = 0;
                if (!accept_client(false, nullptr)) {
                    break;
                }
                continue;
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
                std::cout << "bridge peer "
                          << (message.header.type == CtmBridgeProtocol::MsgError ? "error " : "log ")
                          << text << std::endl;
            }
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
        inputTcpDeltaUsTotal_.fetch_add(tcpDeltaUs, std::memory_order_relaxed);
        atomic_max_u64(&inputTcpDeltaUsMax_, tcpDeltaUs);
        inputCv_.notify_one();
    }

    void input_worker_loop()
    {
        using clock = std::chrono::steady_clock;
        auto lastStatus = clock::now();
        uint64_t lastQueued = 0;
        uint64_t lastProcessed = 0;
        uint64_t lastDropped = 0;
        uint64_t lastTcpDeltaUsTotal = 0;
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
                const uint64_t tcpDeltaUsTotal = inputTcpDeltaUsTotal_.load(std::memory_order_relaxed);
                const uint64_t queueDelayUsTotal = inputQueueDelayUsTotal_.load(std::memory_order_relaxed);
                const uint64_t callbackUsTotal = inputCallbackUsTotal_.load(std::memory_order_relaxed);
                const uint64_t queuedDelta = queued - lastQueued;
                const uint64_t processedDelta = processed - lastProcessed;
                const uint64_t droppedDelta = dropped - lastDropped;
                const uint64_t tcpDeltaUsDelta = tcpDeltaUsTotal - lastTcpDeltaUsTotal;
                const uint64_t queueDelayUsDelta = queueDelayUsTotal - lastQueueDelayUsTotal;
                const uint64_t callbackUsDelta = callbackUsTotal - lastCallbackUsTotal;
                size_t depth = 0;
                {
                    std::lock_guard<std::mutex> lock(inputMutex_);
                    depth = inputQueue_.size();
                }
                const uint64_t tcpMaxUs = inputTcpDeltaUsMax_.exchange(0, std::memory_order_relaxed);
                const uint64_t queueMaxUs = inputQueueDelayUsMax_.exchange(0, std::memory_order_relaxed);
                const uint64_t callbackMaxUs = inputCallbackUsMax_.exchange(0, std::memory_order_relaxed);

                std::cout << "bridge input"
                          << " rx_hz=" << std::fixed << std::setprecision(1)
                          << static_cast<double>(queuedDelta) / seconds
                          << " callback_hz=" << static_cast<double>(processedDelta) / seconds
                          << " drops=" << droppedDelta
                          << " depth=" << depth
                          << " tcp_gap_avg_ms=" << std::fixed << std::setprecision(3)
                          << (queuedDelta == 0 ? 0.0 : static_cast<double>(tcpDeltaUsDelta) / 1000.0 / queuedDelta)
                          << " tcp_gap_max_ms=" << static_cast<double>(tcpMaxUs) / 1000.0
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
                lastTcpDeltaUsTotal = tcpDeltaUsTotal;
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
            std::wcerr << L"bridge feature send failed: " << error << L"\n";
            return false;
        }
        std::unique_lock<std::mutex> lock(featureMutex_);
        const bool signaled = featureCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&]() {
            return featureReplies_.find(requestId) != featureReplies_.end() ||
                   g_stop.load() ||
                   clientSocket_.load() == INVALID_SOCKET;
        });
        if (!signaled || g_stop.load() || clientSocket_.load() == INVALID_SOCKET) {
            std::cout << "bridge feature issue reason=" << reason
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

    uint16_t port_ = 0;
    double btPaceMs_ = 10.0;
    bool wsaStarted_ = false;
    SOCKET listenSocket_ = INVALID_SOCKET;
    std::atomic<SOCKET> clientSocket_{INVALID_SOCKET};
    CtmBridgeProtocol::DeviceCaps capsRaw_ = {};
    std::vector<uint8_t> hidReportDescriptor_;
    std::vector<uint8_t> enumPayload_;   // forwarded composite enumeration (CTMB_MSG_ENUM)
    RawInputCallback callback_;
    std::atomic_bool running_{false};
    std::thread reader_;
    std::thread inputWorker_;
    static constexpr size_t kInputQueueCapacity = 16;
    std::mutex inputMutex_;
    std::condition_variable inputCv_;
    std::deque<QueuedInputReport> inputQueue_;
    std::atomic<uint64_t> inputQueued_{0};
    std::atomic<uint64_t> inputProcessed_{0};
    std::atomic<uint64_t> inputDropped_{0};
    std::atomic<uint64_t> inputTcpDeltaUsTotal_{0};
    std::atomic<uint64_t> inputTcpDeltaUsMax_{0};
    std::atomic<uint64_t> inputQueueDelayUsTotal_{0};
    std::atomic<uint64_t> inputQueueDelayUsMax_{0};
    std::atomic<uint64_t> inputCallbackUsTotal_{0};
    std::atomic<uint64_t> inputCallbackUsMax_{0};
    std::mutex sendMutex_;
    std::atomic<uint32_t> sendSequence_{0};
    std::atomic<uint32_t> nextRequestId_{1};
    std::mutex featureMutex_;
    std::condition_variable featureCv_;
    std::map<uint32_t, std::pair<bool, std::vector<uint8_t>>> featureReplies_;
};
