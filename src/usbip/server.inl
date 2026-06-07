class CtmUsbipServer {
    struct UsbipExportedDevice {
        std::shared_ptr<CtmUsbipDevice> device;
        std::string busId;
        std::string devicePath;
    };

public:
    CtmUsbipServer() = default;

    CtmUsbipServer(CtmUsbipDevice *device, std::string busId)
    {
        std::wstring ignored;
        (void)add_device(device, std::move(busId), &ignored);
    }

    bool add_device(CtmUsbipDevice *device, std::string busId, std::wstring *error)
    {
        return add_device(std::shared_ptr<CtmUsbipDevice>(device, [](CtmUsbipDevice *) {}), std::move(busId), error);
    }

    bool add_device(std::shared_ptr<CtmUsbipDevice> device, std::string busId, std::wstring *error)
    {
        if (!device || busId.empty() || busId.size() > 31) {
            if (error) *error = L"invalid USB/IP device export";
            return false;
        }
        std::lock_guard<std::mutex> lock(devicesMutex_);
        for (UsbipExportedDevice &slot : devices_) {
            if (slot.busId == busId) {
                slot.device = std::move(device);
                slot.devicePath = "/ctm/usbip/" + busId;
                return true;
            }
        }
        UsbipExportedDevice slot;
        slot.device = std::move(device);
        slot.busId = std::move(busId);
        slot.devicePath = "/ctm/usbip/" + slot.busId;
        devices_.push_back(std::move(slot));
        return true;
    }

    void remove_device(const std::string &busId)
    {
        {
            std::lock_guard<std::mutex> lock(devicesMutex_);
            devices_.erase(
                std::remove_if(devices_.begin(), devices_.end(), [&](const UsbipExportedDevice &slot) {
                    return slot.busId == busId;
                }),
                devices_.end());
        }
        shutdown_active_clients(busId);
    }

    // Force any active USB/IP import of this busid to disconnect (Windows sees
    // an unplug) WITHOUT removing the export, so a later re-attach succeeds.
    // Used by the ENet transport to surface an explicit plug-out on link loss
    // while keeping the device available for reconnect. Returns true if a live
    // client was torn down.
    bool detach_device(const std::string &busId)
    {
        bool hadClient;
        {
            std::lock_guard<std::mutex> lock(activeClientsMutex_);
            hadClient = activeClients_.find(busId) != activeClients_.end();
        }
        shutdown_active_clients(busId);
        return hadClient;
    }

    bool start(uint16_t port, std::wstring *error)
    {
        WSADATA data = {};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            if (error) *error = wsa_error_message(L"WSAStartup failed");
            return false;
        }
        wsaStarted_ = true;
        listenSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket_ == INVALID_SOCKET) {
            if (error) *error = wsa_error_message(L"usbip socket failed");
            return false;
        }
        BOOL reuse = TRUE;
        setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (bind(listenSocket_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            if (error) *error = wsa_error_message(L"usbip bind failed");
            return false;
        }
        if (listen(listenSocket_, 8) == SOCKET_ERROR) {
            if (error) *error = wsa_error_message(L"usbip listen failed");
            return false;
        }
        running_.store(true);
        thread_ = std::thread([this]() { accept_loop(); });
        std::wcout << L"usbip server listening on 127.0.0.1:" << port
                   << L" exports=" << snapshot_devices().size() << L"\n";
        return true;
    }

    void stop()
    {
        running_.store(false);
        if (listenSocket_ != INVALID_SOCKET) {
            closesocket(listenSocket_);
            listenSocket_ = INVALID_SOCKET;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        if (wsaStarted_) {
            WSACleanup();
            wsaStarted_ = false;
        }
    }

private:
    std::vector<UsbipExportedDevice> snapshot_devices()
    {
        std::lock_guard<std::mutex> lock(devicesMutex_);
        return devices_;
    }

    void register_active_client(const std::string &busId, SOCKET client)
    {
        std::lock_guard<std::mutex> lock(activeClientsMutex_);
        activeClients_[busId].push_back(client);
    }

    void unregister_active_client(const std::string &busId, SOCKET client)
    {
        std::lock_guard<std::mutex> lock(activeClientsMutex_);
        auto it = activeClients_.find(busId);
        if (it == activeClients_.end()) {
            return;
        }
        std::vector<SOCKET> &clients = it->second;
        clients.erase(std::remove(clients.begin(), clients.end(), client), clients.end());
        if (clients.empty()) {
            activeClients_.erase(it);
        }
    }

    void shutdown_active_clients(const std::string &busId)
    {
        std::vector<SOCKET> clients;
        {
            std::lock_guard<std::mutex> lock(activeClientsMutex_);
            auto it = activeClients_.find(busId);
            if (it == activeClients_.end()) {
                return;
            }
            clients = it->second;
            activeClients_.erase(it);
        }
        for (SOCKET client : clients) {
            shutdown(client, SD_BOTH);
        }
    }

    void accept_loop()
    {
        while (running_.load() && !g_stop.load()) {
            SOCKET client = accept(listenSocket_, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                if (running_.load() && !g_stop.load()) {
                    std::wcerr << L"usbip accept failed\n";
                }
                break;
            }
            int noDelay = 1;
            setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&noDelay), sizeof(noDelay));
            std::thread([this, client]() {
                handle_client(client);
                closesocket(client);
            }).detach();
        }
    }

    void append_usbip_device(std::vector<uint8_t> *out, const UsbipExportedDevice &slot, bool includeInterfaces)
    {
        const UsbDeviceInfo &info = slot.device->info();
        append_fixed_string(out, slot.devicePath.c_str(), 256);
        append_fixed_string(out, slot.busId.c_str(), 32);
        append_be32(out, 1); // busnum
        append_be32(out, 1); // devnum
        append_be32(out, usb_speed_for(info));
        append_be16(out, info.vid);
        append_be16(out, info.pid);
        append_be16(out, info.bcdDevice);
        out->push_back(info.deviceClass);
        out->push_back(info.deviceSubClass);
        out->push_back(info.deviceProtocol);
        out->push_back(info.configurationValue);
        out->push_back(info.numConfigurations);
        out->push_back(info.numInterfaces);
        if (includeInterfaces) {
            for (const InterfaceInfo &intf : info.interfaces) {
                out->push_back(intf.cls);
                out->push_back(intf.subClass);
                out->push_back(intf.protocol);
                out->push_back(0);
            }
        }
    }

    bool send_devlist(SOCKET client)
    {
        const std::vector<UsbipExportedDevice> devices = snapshot_devices();
        std::vector<uint8_t> out;
        append_be16(&out, kUsbipVersion);
        append_be16(&out, kOpRepDevlist);
        append_be32(&out, 0);
        append_be32(&out, static_cast<uint32_t>(devices.size()));
        for (const UsbipExportedDevice &slot : devices) {
            append_usbip_device(&out, slot, true);
        }
        return send_all(client, out.data(), out.size());
    }

    bool send_import(SOCKET client, const UsbipExportedDevice *slot)
    {
        std::vector<uint8_t> out;
        append_be16(&out, kUsbipVersion);
        append_be16(&out, kOpRepImport);
        append_be32(&out, slot ? 0 : 1);
        if (slot) {
            append_usbip_device(&out, *slot, false);
        }
        return send_all(client, out.data(), out.size());
    }

    void handle_client(SOCKET client)
    {
        uint8_t op[8] = {};
        if (!recv_all(client, op, sizeof(op))) {
            return;
        }
        const uint16_t version = read_be16(op);
        const uint16_t code = read_be16(op + 2);
        if (version != kUsbipVersion) {
            return;
        }
        if (code == kOpReqDevlist) {
            (void)send_devlist(client);
            return;
        }
        if (code == kOpReqImport) {
            uint8_t busid[32] = {};
            if (!recv_all(client, busid, sizeof(busid))) {
                return;
            }
            UsbipExportedDevice match;
            bool ok = false;
            for (const UsbipExportedDevice &slot : snapshot_devices()) {
                if (strncmp(reinterpret_cast<const char *>(busid), slot.busId.c_str(), sizeof(busid)) == 0) {
                    match = slot;
                    ok = true;
                    break;
                }
            }
            if (!send_import(client, ok ? &match : nullptr) || !ok) {
                return;
            }
            std::cout << "usbip imported busid=" << match.busId << std::endl;
            register_active_client(match.busId, client);
            urb_loop(client, match.device, match.busId);
            unregister_active_client(match.busId, client);
        }
    }

    void append_iso_response_descriptors(
        std::vector<uint8_t> *out,
        const std::vector<uint8_t> &isoDescriptors,
        uint32_t packets,
        uint32_t actualLength,
        int32_t status)
    {
        if (packets == kNonIsoPackets || isoDescriptors.size() < static_cast<size_t>(packets) * 16) {
            return;
        }
        for (uint32_t i = 0; i < packets; ++i) {
            const uint8_t *src = isoDescriptors.data() + i * 16;
            const uint32_t offset = read_be32(src);
            const uint32_t length = read_be32(src + 4);
            uint32_t packetActual = 0;
            if (status == 0) {
                if (actualLength >= offset + length) {
                    packetActual = length;
                } else if (actualLength > offset) {
                    packetActual = actualLength - offset;
                }
            }
            append_be32(out, offset);
            append_be32(out, length);
            append_be32(out, packetActual);
            append_be32(out, static_cast<uint32_t>(status));
        }
    }

    bool send_ret_submit(
        SOCKET client,
        uint32_t seqnum,
        int32_t status,
        uint32_t actualLength,
        uint32_t startFrame,
        const std::vector<uint8_t> &payload,
        uint32_t packets,
        const std::vector<uint8_t> &isoDescriptors)
    {
        std::vector<uint8_t> out;
        append_be32(&out, kRetSubmit);
        append_be32(&out, seqnum);
        append_be32(&out, 0);
        append_be32(&out, 0);
        append_be32(&out, 0);
        append_be32(&out, static_cast<uint32_t>(status));
        append_be32(&out, status == 0 ? actualLength : 0);
        append_be32(&out, startFrame);
        append_be32(&out, packets);
        append_be32(&out, 0);
        append_be32(&out, 0);
        append_be32(&out, 0);
        if (!payload.empty()) {
            out.insert(out.end(), payload.begin(), payload.end());
        }
        append_iso_response_descriptors(&out, isoDescriptors, packets, actualLength, status);
        return send_all(client, out.data(), out.size());
    }

    bool send_ret_unlink(SOCKET client, uint32_t seqnum, int32_t status)
    {
        std::vector<uint8_t> out;
        append_be32(&out, kRetUnlink);
        append_be32(&out, seqnum);
        append_be32(&out, 0);
        append_be32(&out, 0);
        append_be32(&out, 0);
        append_be32(&out, static_cast<uint32_t>(status));
        for (int i = 0; i < 6; ++i) {
            append_be32(&out, 0);
        }
        return send_all(client, out.data(), out.size());
    }

    void urb_loop(SOCKET client, std::shared_ptr<CtmUsbipDevice> device, const std::string &busId)
    {
        using clock = std::chrono::steady_clock;
        uint64_t urbCount = 0;
        uint64_t controlCount = 0;
        uint64_t isoInCount = 0;
        uint64_t isoOutCount = 0;
        uint64_t intInCount = 0;
        uint64_t intOutCount = 0;
        uint64_t ep01OutCount = 0;
        uint64_t ep82InCount = 0;
        uint64_t ep84InCount = 0;
        uint64_t ep03OutCount = 0;
        uint64_t inputFreshCount = 0;
        uint64_t inputStaleCount = 0;
        uint64_t inputZeroCount = 0;
        uint64_t inputWaitUsTotal = 0;
        uint32_t inputWaitUsMax = 0;
        uint64_t isoOutAckWaitUsTotal = 0;
        uint32_t isoOutAckWaitUsMax = 0;
        uint64_t isoOutAckPacedCount = 0;
        double isoOutRegulatorMultiplier = 1.0;
        double isoOutRegulatorError = 0.0;
        double isoOutRegulatorActualRate = 0.0;
        double isoOutRegulatorExpectedRate = 0.0;
        uint64_t errorCount = 0;
        uint64_t lastUrb = 0;
        uint64_t lastControl = 0;
        uint64_t lastIsoIn = 0;
        uint64_t lastIsoOut = 0;
        uint64_t lastIntIn = 0;
        uint64_t lastIntOut = 0;
        uint64_t lastEp01Out = 0;
        uint64_t lastEp82In = 0;
        uint64_t lastEp84In = 0;
        uint64_t lastEp03Out = 0;
        uint64_t lastInputFresh = 0;
        uint64_t lastInputStale = 0;
        uint64_t lastInputZero = 0;
        uint64_t lastInputWaitUsTotal = 0;
        uint64_t lastIsoOutAckWaitUsTotal = 0;
        uint64_t lastIsoOutAckPaced = 0;
        uint64_t lastError = 0;
        CtmAudioStats lastAudioStats;
        auto lastStatus = clock::now();

        struct PendingInterruptIn {
            uint32_t seqnum = 0;
            uint8_t endpointAddress = 0;
            uint32_t transferLength = 0;
        };

        struct PendingIsoOutAck {
            uint32_t seqnum = 0;
            int32_t status = kStatusOk;
            uint32_t actualLength = 0;
            uint32_t startFrame = 0;
            std::vector<uint8_t> payload;
            uint32_t packets = kNonIsoPackets;
            std::vector<uint8_t> isoDescriptors;
            uint32_t durationUs = 0;
            uint32_t audioFrames = 0;
            uint32_t expectedRateHz = 0;
        };

        std::mutex sendMutex;
        // Per-endpoint interrupt-IN workers: one worker thread per IN endpoint,
        // each blocking only on its OWN endpoint. A single shared worker blocks
        // indefinitely in handle_interrupt_in() on an idle endpoint (e.g. an unused
        // puck slot) and starves the active endpoint -- that head-of-line block was
        // why input never reached Windows. Spawned lazily on the first URB per ep.
        struct EndpointInWorker {
            std::deque<PendingInterruptIn> queue;
            std::mutex mutex;
            std::condition_variable cv;
            std::thread thread;
        };
        std::mutex interruptInWorkersMutex;
        std::map<uint8_t, std::unique_ptr<EndpointInWorker>> interruptInWorkers;
        std::mutex isoOutAckMutex;
        std::condition_variable isoOutAckCv;
        std::deque<PendingIsoOutAck> pendingIsoOutAck;
        std::mutex statsMutex;
        std::atomic_bool sessionActive(true);

        auto close_session = [&]() {
            const bool wasActive = sessionActive.exchange(false);
            if (wasActive) {
                shutdown(client, SD_BOTH);
                device->wake_input_waiters();
                {
                    std::lock_guard<std::mutex> lock(interruptInWorkersMutex);
                    for (auto &kv : interruptInWorkers) {
                        kv.second->cv.notify_all();
                    }
                }
                isoOutAckCv.notify_all();
            }
        };

        auto send_submit = [&](uint32_t seq, int32_t status, uint32_t actualLength, uint32_t frame,
                               const std::vector<uint8_t> &payload, uint32_t packetCount,
                               const std::vector<uint8_t> &descriptors) -> bool {
            std::lock_guard<std::mutex> lock(sendMutex);
            return send_ret_submit(client, seq, status, actualLength, frame, payload, packetCount, descriptors);
        };

        auto record_input_reply = [&](const CtmSubmitInfo &submitInfo) {
            if (submitInfo.inputReply == InputReplyKind::None) {
                return;
            }
            std::lock_guard<std::mutex> lock(statsMutex);
            if (submitInfo.inputReply == InputReplyKind::Fresh) {
                ++inputFreshCount;
            } else if (submitInfo.inputReply == InputReplyKind::Stale) {
                ++inputStaleCount;
            } else if (submitInfo.inputReply == InputReplyKind::Zero) {
                ++inputZeroCount;
            }
            inputWaitUsTotal += submitInfo.inputWaitUs;
            inputWaitUsMax = (std::max<uint32_t>)(inputWaitUsMax, submitInfo.inputWaitUs);
        };

        auto record_iso_ack_wait = [&](uint32_t waitedUs) {
            std::lock_guard<std::mutex> lock(statsMutex);
            isoOutAckWaitUsTotal += waitedUs;
            isoOutAckWaitUsMax = (std::max<uint32_t>)(isoOutAckWaitUsMax, waitedUs);
            ++isoOutAckPacedCount;
        };

        auto record_iso_regulator = [&](double multiplier, double error, double actualRate, double expectedRate) {
            std::lock_guard<std::mutex> lock(statsMutex);
            isoOutRegulatorMultiplier = multiplier;
            isoOutRegulatorError = error;
            isoOutRegulatorActualRate = actualRate;
            isoOutRegulatorExpectedRate = expectedRate;
        };

        auto record_error = [&]() {
            std::lock_guard<std::mutex> lock(statsMutex);
            ++errorCount;
        };

        auto interrupt_in_worker_loop = [&](EndpointInWorker *w) {
            while (sessionActive.load() && running_.load() && !g_stop.load()) {
                PendingInterruptIn item;
                {
                    std::unique_lock<std::mutex> lock(w->mutex);
                    w->cv.wait(lock, [&]() {
                        return !w->queue.empty() ||
                            !sessionActive.load() ||
                            !running_.load() ||
                            g_stop.load();
                    });
                    if (w->queue.empty()) {
                        break;
                    }
                    item = w->queue.front();
                    w->queue.pop_front();
                }

                std::vector<uint8_t> inData;
                CtmSubmitInfo submitInfo;
                int status = device->handle_interrupt_in(
                    item.endpointAddress,
                    item.transferLength,
                    &inData,
                    &submitInfo,
                    &sessionActive);
                record_input_reply(submitInfo);

                std::vector<uint8_t> payload;
                uint32_t actualLength = 0;
                if (status == kStatusOk) {
                    payload = std::move(inData);
                    if (payload.size() > item.transferLength) {
                        payload.resize(item.transferLength);
                    }
                    actualLength = static_cast<uint32_t>(payload.size());
                } else if (sessionActive.load() && !g_stop.load()) {
                    record_error();
                }

                if (!send_submit(item.seqnum, status, actualLength, 0, payload, kNonIsoPackets, std::vector<uint8_t>())) {
                    close_session();
                    break;
                }
            }
        };

        std::thread isoOutAckWorker([&]() {
            auto nextIsoOutAck = clock::time_point{};
            auto regulatorWindowStart = clock::time_point{};
            uint64_t regulatorWindowFrames = 0;
            double regulatorMultiplier = 1.0;
            constexpr double regulatorGain = 0.005;
            constexpr double regulatorMaxStep = 0.00025;
            constexpr double regulatorMin = 0.98;
            constexpr double regulatorMax = 1.02;
            while (sessionActive.load() && running_.load() && !g_stop.load()) {
                PendingIsoOutAck item;
                {
                    std::unique_lock<std::mutex> lock(isoOutAckMutex);
                    isoOutAckCv.wait(lock, [&]() {
                        return !pendingIsoOutAck.empty() ||
                            !sessionActive.load() ||
                            !running_.load() ||
                            g_stop.load();
                    });
                    if (pendingIsoOutAck.empty() ||
                        !sessionActive.load() ||
                        !running_.load() ||
                        g_stop.load()) {
                        break;
                    }
                    item = std::move(pendingIsoOutAck.front());
                    pendingIsoOutAck.pop_front();
                }

                const auto paceStart = clock::now();
                if (item.durationUs != 0) {
                    uint32_t effectiveDurationUs = item.durationUs;
                    if (item.audioFrames != 0 && item.expectedRateHz != 0 && regulatorMultiplier > 0.0) {
                        const double adjusted = static_cast<double>(item.durationUs) / regulatorMultiplier;
                        effectiveDurationUs = static_cast<uint32_t>((std::max<double>)(1.0, adjusted + 0.5));
                    }
                    if (nextIsoOutAck == clock::time_point{} ||
                        paceStart > nextIsoOutAck + std::chrono::milliseconds(50)) {
                        nextIsoOutAck = paceStart;
                    }
                    nextIsoOutAck += std::chrono::microseconds(effectiveDurationUs);
                    if (nextIsoOutAck > paceStart) {
                        std::this_thread::sleep_until(nextIsoOutAck);
                    }
                }
                const auto completionTime = clock::now();
                if (item.audioFrames != 0 && item.expectedRateHz != 0) {
                    if (regulatorWindowStart == clock::time_point{}) {
                        regulatorWindowStart = completionTime;
                    }
                    regulatorWindowFrames += item.audioFrames;
                    const double windowSeconds =
                        std::chrono::duration<double>(completionTime - regulatorWindowStart).count();
                    if (windowSeconds >= 0.500) {
                        const double actualRate =
                            static_cast<double>(regulatorWindowFrames) / windowSeconds;
                        const double expectedRate = static_cast<double>(item.expectedRateHz);
                        const double error = expectedRate > 0.0
                            ? (actualRate - expectedRate) / expectedRate
                            : 0.0;
                        double adjustment = -error * regulatorGain;
                        if (adjustment < -regulatorMaxStep) adjustment = -regulatorMaxStep;
                        if (adjustment > regulatorMaxStep) adjustment = regulatorMaxStep;
                        regulatorMultiplier *= 1.0 + adjustment;
                        if (regulatorMultiplier < regulatorMin) regulatorMultiplier = regulatorMin;
                        if (regulatorMultiplier > regulatorMax) regulatorMultiplier = regulatorMax;
                        record_iso_regulator(regulatorMultiplier, error, actualRate, expectedRate);
                        regulatorWindowFrames = 0;
                        regulatorWindowStart = completionTime;
                    }
                }
                const uint32_t waitedUs = static_cast<uint32_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        completionTime - paceStart).count());
                record_iso_ack_wait(waitedUs);

                if (!send_submit(
                        item.seqnum,
                        item.status,
                        item.actualLength,
                        item.startFrame,
                        item.payload,
                        item.packets,
                        item.isoDescriptors)) {
                    close_session();
                    break;
                }
            }
        });

        while (running_.load() && !g_stop.load() && sessionActive.load()) {
            uint8_t header[48] = {};
            if (!recv_all(client, header, sizeof(header))) {
                break;
            }
            const uint32_t command = read_be32(header);
            const uint32_t seqnum = read_be32(header + 4);
            const uint32_t direction = read_be32(header + 12);
            const uint32_t ep = read_be32(header + 16);
            if (command == kCmdUnlink) {
                {
                    std::lock_guard<std::mutex> lock(sendMutex);
                    (void)send_ret_unlink(client, seqnum, kStatusOk);
                }
                continue;
            }
            if (command != kCmdSubmit) {
                break;
            }
            const uint32_t transferLength = read_be32(header + 24);
            const uint32_t startFrame = read_be32(header + 28);
            const uint32_t packets = read_be32(header + 32);
            const uint32_t interval = read_be32(header + 36);
            uint8_t setup[8] = {};
            memcpy(setup, header + 40, sizeof(setup));
            ++urbCount;
            const bool isControl = (ep & 0x0f) == 0;
            const uint8_t endpointAddress = static_cast<uint8_t>(
                (ep & 0x0f) | (direction == kUsbipDirIn ? 0x80 : 0x00));
            const bool endpointIso = !isControl && device->is_iso_endpoint(endpointAddress);
            const bool isIso = packets != kNonIsoPackets || endpointIso;
            if (isControl) {
                ++controlCount;
            } else if (isIso && direction == kUsbipDirIn) {
                ++isoInCount;
            } else if (isIso) {
                ++isoOutCount;
            } else if (direction == kUsbipDirIn) {
                ++intInCount;
            } else {
                ++intOutCount;
            }
            if (endpointAddress == 0x01 && direction == kUsbipDirOut) {
                ++ep01OutCount;
            } else if (endpointAddress == 0x82 && direction == kUsbipDirIn) {
                ++ep82InCount;
            } else if (endpointAddress == 0x84 && direction == kUsbipDirIn) {
                ++ep84InCount;
            } else if (endpointAddress == 0x03 && direction == kUsbipDirOut) {
                ++ep03OutCount;
            }

            std::vector<uint8_t> outData;
            if (direction == kUsbipDirOut && transferLength != 0) {
                outData.resize(transferLength);
                if (!recv_all(client, outData.data(), outData.size())) {
                    break;
                }
            }
            std::vector<uint8_t> isoDescriptors;
            if (packets != kNonIsoPackets && packets < 1024) {
                isoDescriptors.resize(static_cast<size_t>(packets) * 16);
                if (!recv_all(client, isoDescriptors.data(), isoDescriptors.size())) {
                    break;
                }
            }

            const bool asyncInterruptIn =
                !isControl &&
                !isIso &&
                direction == kUsbipDirIn &&
                device->is_interrupt_endpoint(endpointAddress);
            if (asyncInterruptIn) {
                EndpointInWorker *w = nullptr;
                {
                    std::lock_guard<std::mutex> lock(interruptInWorkersMutex);
                    std::unique_ptr<EndpointInWorker> &slot = interruptInWorkers[endpointAddress];
                    if (!slot) {
                        slot.reset(new EndpointInWorker());
                        w = slot.get();
                        w->thread = std::thread([&, w]() { interrupt_in_worker_loop(w); });
                    } else {
                        w = slot.get();
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(w->mutex);
                    w->queue.push_back(PendingInterruptIn{seqnum, endpointAddress, transferLength});
                }
                w->cv.notify_one();
                continue;
            }

            std::vector<uint8_t> inData;
            CtmSubmitInfo submitInfo;
            int status = device->handle_submit(
                direction,
                ep,
                setup,
                outData,
                transferLength,
                &inData,
                &submitInfo);
            record_input_reply(submitInfo);
            std::vector<uint8_t> payload;
            uint32_t actualLength = 0;
            if (status == kStatusOk) {
                if (direction == kUsbipDirIn) {
                    payload = std::move(inData);
                    if (payload.size() > transferLength) {
                        payload.resize(transferLength);
                    }
                    actualLength = static_cast<uint32_t>(payload.size());
                } else {
                    // USB/IP RET_SUBMIT carries transfer_buffer bytes only for
                    // IN URBs. OUT URBs report completion through actual_length
                    // but must not echo the OUT payload, or the client stream
                    // becomes desynchronized.
                    actualLength = transferLength;
                }
            }

            const uint32_t isoOutDurationUs =
                (status == kStatusOk && direction == kUsbipDirOut && isIso)
                    ? device->iso_out_audio_duration_us(endpointAddress, actualLength)
                    : 0;
            const uint32_t isoOutAudioFrames =
                (isoOutDurationUs != 0)
                    ? device->iso_out_audio_frames(endpointAddress, actualLength)
                    : 0;
            const uint32_t isoOutExpectedRateHz =
                (isoOutDurationUs != 0)
                    ? device->iso_out_expected_rate_hz(endpointAddress)
                    : 0;

            if (status != kStatusOk) {
                record_error();
                if (ctm_verbose_logs()) {
                    std::cout << "usbip issue"
                              << " seq=" << seqnum
                              << " status=" << status
                              << " dir=" << (direction == kUsbipDirIn ? "in" : "out")
                              << " ep=0x" << std::hex << std::setw(2) << std::setfill('0')
                              << static_cast<unsigned int>((ep & 0x0f) | (direction == kUsbipDirIn ? 0x80 : 0x00))
                              << std::dec << std::setfill(' ')
                              << " len=" << transferLength
                              << " start_frame=" << startFrame
                              << " interval=" << interval;
                    if (isControl) {
                        std::cout << " setup=" << hex_span(setup, 8);
                    }
                    std::cout << std::endl;
                }
            }

            const auto now = clock::now();
            if (now - lastStatus >= std::chrono::seconds(2)) {
                const double seconds = std::chrono::duration<double>(now - lastStatus).count();
                uint64_t inputFreshSnapshot = 0;
                uint64_t inputStaleSnapshot = 0;
                uint64_t inputZeroSnapshot = 0;
                uint64_t inputWaitUsSnapshot = 0;
                uint32_t inputWaitUsMaxSnapshot = 0;
                uint64_t isoOutAckWaitUsSnapshot = 0;
                uint32_t isoOutAckWaitUsMaxSnapshot = 0;
                uint64_t isoOutAckPacedSnapshot = 0;
                double isoOutRegulatorMultiplierSnapshot = 1.0;
                double isoOutRegulatorErrorSnapshot = 0.0;
                double isoOutRegulatorActualRateSnapshot = 0.0;
                double isoOutRegulatorExpectedRateSnapshot = 0.0;
                uint64_t errorSnapshot = 0;
                {
                    std::lock_guard<std::mutex> lock(statsMutex);
                    inputFreshSnapshot = inputFreshCount;
                    inputStaleSnapshot = inputStaleCount;
                    inputZeroSnapshot = inputZeroCount;
                    inputWaitUsSnapshot = inputWaitUsTotal;
                    inputWaitUsMaxSnapshot = inputWaitUsMax;
                    inputWaitUsMax = 0;
                    isoOutAckWaitUsSnapshot = isoOutAckWaitUsTotal;
                    isoOutAckWaitUsMaxSnapshot = isoOutAckWaitUsMax;
                    isoOutAckWaitUsMax = 0;
                    isoOutAckPacedSnapshot = isoOutAckPacedCount;
                    isoOutRegulatorMultiplierSnapshot = isoOutRegulatorMultiplier;
                    isoOutRegulatorErrorSnapshot = isoOutRegulatorError;
                    isoOutRegulatorActualRateSnapshot = isoOutRegulatorActualRate;
                    isoOutRegulatorExpectedRateSnapshot = isoOutRegulatorExpectedRate;
                    errorSnapshot = errorCount;
                }
                const uint64_t inputReplies =
                    (inputFreshSnapshot - lastInputFresh) +
                    (inputStaleSnapshot - lastInputStale) +
                    (inputZeroSnapshot - lastInputZero);
                const uint64_t inputWaitDelta = inputWaitUsSnapshot - lastInputWaitUsTotal;
                const double inputWaitAvgMs = inputReplies == 0
                    ? 0.0
                    : static_cast<double>(inputWaitDelta) / 1000.0 / static_cast<double>(inputReplies);
                const uint64_t isoOutAckPacedDelta = isoOutAckPacedSnapshot - lastIsoOutAckPaced;
                const uint64_t isoOutAckWaitDelta = isoOutAckWaitUsSnapshot - lastIsoOutAckWaitUsTotal;
                const double isoOutAckWaitAvgMs = isoOutAckPacedDelta == 0
                    ? 0.0
                    : static_cast<double>(isoOutAckWaitDelta) / 1000.0 /
                        static_cast<double>(isoOutAckPacedDelta);
                const CtmAudioStats audioStats = device->audio_stats();
                const double urbHz = static_cast<double>(urbCount - lastUrb) / seconds;
                const double controlHz = static_cast<double>(controlCount - lastControl) / seconds;
                const double isoOutHz = static_cast<double>(isoOutCount - lastIsoOut) / seconds;
                const double isoInHz = static_cast<double>(isoInCount - lastIsoIn) / seconds;
                const double intInHz = static_cast<double>(intInCount - lastIntIn) / seconds;
                const double intOutHz = static_cast<double>(intOutCount - lastIntOut) / seconds;
                const double ep01OutHz = static_cast<double>(ep01OutCount - lastEp01Out) / seconds;
                const double ep82InHz = static_cast<double>(ep82InCount - lastEp82In) / seconds;
                const double ep84InHz = static_cast<double>(ep84InCount - lastEp84In) / seconds;
                const double ep03OutHz = static_cast<double>(ep03OutCount - lastEp03Out) / seconds;
                const double inputFreshHz =
                    static_cast<double>(inputFreshSnapshot - lastInputFresh) / seconds;
                const double inputStaleHz =
                    static_cast<double>(inputStaleSnapshot - lastInputStale) / seconds;
                const double inputZeroHz =
                    static_cast<double>(inputZeroSnapshot - lastInputZero) / seconds;
                const uint64_t errorDelta = errorSnapshot - lastError;

                if (ctm_verbose_logs()) {
                    std::cout << "usbip summary"
                              << " win_urb_hz=" << std::fixed << std::setprecision(1)
                              << urbHz
                              << " win_control_hz=" << controlHz
                              << " win_iso_out_hz=" << isoOutHz
                              << " win_iso_in_hz=" << isoInHz
                              << " win_int_in_hz=" << intInHz
                              << " win_int_out_hz=" << intOutHz
                              << " ep01_out_hz=" << ep01OutHz
                              << " ep82_in_hz=" << ep82InHz
                              << " ep84_in_hz=" << ep84InHz
                              << " ep03_out_hz=" << ep03OutHz
                              << " input_fresh_hz=" << inputFreshHz
                              << " input_stale_hz=" << inputStaleHz
                              << " input_zero_hz=" << inputZeroHz
                              << " input_wait_avg_ms=" << std::fixed << std::setprecision(3)
                              << inputWaitAvgMs
                              << " input_wait_max_ms=" << static_cast<double>(inputWaitUsMaxSnapshot) / 1000.0
                              << " iso_ack_wait_avg_ms=" << isoOutAckWaitAvgMs
                              << " iso_ack_wait_max_ms=" << static_cast<double>(isoOutAckWaitUsMaxSnapshot) / 1000.0
                              << " iso_reg_cm=" << std::fixed << std::setprecision(6)
                              << isoOutRegulatorMultiplierSnapshot
                              << " iso_reg_error=" << isoOutRegulatorErrorSnapshot
                              << " iso_reg_ar=" << std::fixed << std::setprecision(1)
                              << isoOutRegulatorActualRateSnapshot
                              << " iso_reg_er=" << isoOutRegulatorExpectedRateSnapshot
                              << " audio_in_kib_s=" << (static_cast<double>(audioStats.isoBytes - lastAudioStats.isoBytes) / 1024.0) / seconds
                              << " audio_resampled_hz="
                              << static_cast<double>(audioStats.reservoirFrames - lastAudioStats.reservoirFrames) / seconds
                              << " audio_chunks_hz="
                              << static_cast<double>(audioStats.chunksBuilt - lastAudioStats.chunksBuilt) / seconds
                              << " audio_fill_ms=" << audioStats.reservoirFillMs
                              << " audio_drops=" << (audioStats.reservoirDrops - lastAudioStats.reservoirDrops)
                              << " audio_waits=" << (audioStats.reservoirWaits - lastAudioStats.reservoirWaits)
                              << " audio_build_fails=" << (audioStats.buildFails - lastAudioStats.buildFails)
                              << " audio_send_fails=" << (audioStats.sendFails - lastAudioStats.sendFails)
                              << " audio_tail_bytes=" << (audioStats.trailingBytes - lastAudioStats.trailingBytes)
                              << " errors=" << errorDelta
                              << std::defaultfloat
                              << std::endl;
                } else {
                    std::cout << "input poll/fresh=" << std::fixed << std::setprecision(1)
                              << intInHz << "/" << inputFreshHz
                              << "Hz wait=" << std::fixed << std::setprecision(2)
                              << inputWaitAvgMs << "/" << static_cast<double>(inputWaitUsMaxSnapshot) / 1000.0
                              << "ms | audio out=" << std::fixed << std::setprecision(1)
                              << isoOutHz
                              << "Hz ack=" << std::fixed << std::setprecision(2)
                              << isoOutAckWaitAvgMs << "/" << static_cast<double>(isoOutAckWaitUsMaxSnapshot) / 1000.0
                              << "ms rate=" << std::fixed << std::setprecision(0)
                              << isoOutRegulatorActualRateSnapshot << "/" << isoOutRegulatorExpectedRateSnapshot
                              << " err=" << std::fixed << std::setprecision(2)
                              << isoOutRegulatorErrorSnapshot * 100.0
                              << "% cm=" << std::fixed << std::setprecision(6)
                              << isoOutRegulatorMultiplierSnapshot
                              << " errors=" << errorDelta
                              << std::defaultfloat
                              << std::endl;
                }
                lastUrb = urbCount;
                lastControl = controlCount;
                lastIsoIn = isoInCount;
                lastIsoOut = isoOutCount;
                lastIntIn = intInCount;
                lastIntOut = intOutCount;
                lastEp01Out = ep01OutCount;
                lastEp82In = ep82InCount;
                lastEp84In = ep84InCount;
                lastEp03Out = ep03OutCount;
                lastInputFresh = inputFreshSnapshot;
                lastInputStale = inputStaleSnapshot;
                lastInputZero = inputZeroSnapshot;
                lastInputWaitUsTotal = inputWaitUsSnapshot;
                lastIsoOutAckWaitUsTotal = isoOutAckWaitUsSnapshot;
                lastIsoOutAckPaced = isoOutAckPacedSnapshot;
                lastError = errorSnapshot;
                lastAudioStats = audioStats;
                lastStatus = now;
            }
            if (isoOutDurationUs != 0) {
                {
                    std::lock_guard<std::mutex> lock(isoOutAckMutex);
                    pendingIsoOutAck.push_back(PendingIsoOutAck{
                        seqnum,
                        status,
                        actualLength,
                        startFrame,
                        std::move(payload),
                        packets,
                        std::move(isoDescriptors),
                        isoOutDurationUs,
                        isoOutAudioFrames,
                        isoOutExpectedRateHz});
                }
                isoOutAckCv.notify_one();
                continue;
            }
            if (!send_submit(
                    seqnum,
                    status,
                    actualLength,
                    isIso ? startFrame : 0,
                    payload,
                    packets,
                    isoDescriptors)) {
                close_session();
                break;
            }
        }
        close_session();
        {
            std::lock_guard<std::mutex> lock(interruptInWorkersMutex);
            for (auto &kv : interruptInWorkers) {
                kv.second->cv.notify_all();
            }
        }
        for (auto &kv : interruptInWorkers) {
            if (kv.second->thread.joinable()) {
                kv.second->thread.join();
            }
        }
        if (isoOutAckWorker.joinable()) {
            isoOutAckWorker.join();
        }
        std::cout << "usbip import connection closed busid=" << busId << std::endl;
    }

    std::mutex devicesMutex_;
    std::vector<UsbipExportedDevice> devices_;
    std::mutex activeClientsMutex_;
    std::map<std::string, std::vector<SOCKET>> activeClients_;
    bool wsaStarted_ = false;
    SOCKET listenSocket_ = INVALID_SOCKET;
    std::atomic_bool running_{false};
    std::thread thread_;
};
