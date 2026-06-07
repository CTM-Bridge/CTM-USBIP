static int read_hid_report(
    HANDLE handle,
    OVERLAPPED *ov,
    uint8_t *buffer,
    DWORD bufferLength,
    DWORD *readBytes,
    DWORD timeoutMs)
{
    if (ov == nullptr || ov->hEvent == nullptr || buffer == nullptr || readBytes == nullptr) {
        return -1;
    }
    ResetEvent(ov->hEvent);
    *readBytes = 0;
    BOOL ok = ReadFile(handle, buffer, bufferLength, readBytes, ov);
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && err == ERROR_IO_PENDING) {
        DWORD wait = WaitForSingleObject(ov->hEvent, timeoutMs);
        if (wait == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(handle, ov, readBytes, FALSE);
            if (!ok && GetLastError() == ERROR_OPERATION_ABORTED) {
                return 1;
            }
        } else {
            CancelIoEx(handle, ov);
            if (GetOverlappedResult(handle, ov, readBytes, TRUE)) {
                return 0;
            }
            return 1;
        }
    }
    if (!ok && err == ERROR_OPERATION_ABORTED) {
        return 1;
    }
    return ok ? 0 : -1;
}

static bool write_hid_report(
    HANDLE handle,
    const std::vector<uint8_t> &report,
    unsigned short outputReportLength,
    std::wstring *error)
{
    // NOTE: the paragraph below is INCORRECT and kept only for history.
    // Windows HIDClass WriteFile requires the write buffer to be exactly
    // OutputReportByteLength bytes (report[0] = report ID); we pad to that
    // just before the WriteFile call. Stale original note follows:
    // Write exactly the bytes the map produced. The map already sizes
    // destination buffers to the per-report-id declared length (78 for
    // 0x31, 398 for 0x36, etc.) including the CRC32 tail; padding the
    // buffer up to `outputReportLength` (the device's max across all
    // output report IDs) is a USB-HID-convention leftover from when
    // alternate write methods (HidD_SetOutputReport, IOCTL_HID_SET_*)
    // required fixed-size buffers. WriteFile to a BT HID handle does
    // not require it, and the bridge path (Linux hidraw on the TV)
    // writes exact bytes successfully â€” the controller expects the
    // wire size to match the report ID's declared length.
    // A collection that declares no output report has OutputReportByteLength
    // == 0 and rejects every WriteFile with ERROR_INVALID_PARAMETER ("The
    // parameter is incorrect"). Surface that as a clear error instead of a
    // cryptic OS code (e.g. an Xbox controller whose BLE HID interface is
    // input-only on Windows).
    if (outputReportLength == 0) {
        if (error) *error = L"device exposes no HID output report (OutputReportByteLength=0)";
        return false;
    }
    if (report.size() > outputReportLength) {
        if (error) *error = L"BT HID output report exceeds device max length";
        return false;
    }
    // Pad the map-produced report up to the device's OutputReportByteLength;
    // Windows HIDClass requires the write buffer to match it exactly.
    std::vector<uint8_t> framed(report);
    framed.resize(outputReportLength, 0);

    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) {
        if (error) *error = last_error_message(L"CreateEventW failed");
        return false;
    }

    DWORD written = 0;
    BOOL ok = WriteFile(handle, framed.data(), static_cast<DWORD>(framed.size()), nullptr, &ov);
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && err == ERROR_IO_PENDING) {
        DWORD wait = WaitForSingleObject(ov.hEvent, 1000);
        if (wait == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(handle, &ov, &written, FALSE);
        } else {
            CancelIo(handle);
            CloseHandle(ov.hEvent);
            if (error) *error = L"BT HID write timed out";
            return false;
        }
    }
    CloseHandle(ov.hEvent);
    if (!ok) {
        if (error) *error = last_error_message(L"BT HID WriteFile failed");
        return false;
    }
    return true;
}

class LocalBtBackend final : public CtmBackend {
public:
    LocalBtBackend(unsigned long index, double btPaceMs)
        : index_(index), btPaceMs_(btPaceMs > 0.0 ? btPaceMs : 10.0)
    {
    }

    bool start(RawInputCallback callback, std::wstring *error) override
    {
        std::vector<CtmBtDevice> devices = ctm_list_bluetooth_hid_devices();
        if (index_ >= devices.size()) {
            if (error) *error = L"Bluetooth controller index out of range";
            return false;
        }
        device_ = devices[index_];
        if (device_.path.empty()) {
            if (error) *error = L"selected controller has no HID path";
            return false;
        }
        handle_ = CreateFileW(
            device_.path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
            nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            if (error) *error = last_error_message(L"open BT HID failed");
            return false;
        }
        callback_ = std::move(callback);
        running_.store(true);
        reader_ = std::thread([this]() { reader_loop(); });
        writer_ = std::thread([this]() { paced_writer_loop(); });

        std::wcout << L"bt backend"
                   << L" index=" << index_
                   << L" product=" << device_.product
                   << L" serial=" << virtual_usb_serial_from_bt_device(device_)
                   << L" hid_serial=" << device_.serial
                   << L" input_len=" << device_.input_report_length
                   << L" output_len=" << device_.output_report_length
                   << L" feature_len=" << device_.feature_report_length
                   << L" pace_ms=" << std::fixed << std::setprecision(3) << btPaceMs_
                   << std::defaultfloat << L"\n";
        // Extra serial-source dump so the chain of fallbacks for
        // virtual_usb_serial_from_bt_device is auditable on a per-device basis.
        std::wcout << L"bt backend serial sources"
                   << L" hid_serial=[" << device_.serial << L"]"
                   << L" instance_id=[" << device_.instance_id << L"]"
                   << L" parent_instance_id=[" << device_.parent_instance_id << L"]"
                   << L" path=[" << device_.path << L"]"
                   << L"\n";
        return true;
    }

    void stop() override
    {
        running_.store(false);
        if (handle_ != INVALID_HANDLE_VALUE) {
            CancelIoEx(handle_, nullptr);
        }
        pacedCv_.notify_all();
        if (reader_.joinable()) {
            reader_.join();
        }
        if (writer_.joinable()) {
            writer_.join();
        }
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    BackendCaps caps() const override
    {
        BackendCaps caps;
        caps.vendorId = device_.vendor_id ? device_.vendor_id : 0x054c;
        caps.productId = device_.product_id ? device_.product_id : 0x0ce6;
        caps.version = device_.version_number ? device_.version_number : 0x0100;
        caps.inputReportLength = device_.input_report_length ? device_.input_report_length : 64;
        caps.outputReportLength = device_.output_report_length ? device_.output_report_length : 64;
        caps.featureReportLength = device_.feature_report_length ? device_.feature_report_length : 64;
        caps.serial = virtual_usb_serial_from_bt_device(device_);
        caps.product = device_.product;
        caps.path = device_.path;
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
        (void)timeoutMs;
        if (scratch == nullptr || lastGetResponse == nullptr || lastGetResponseLength == nullptr || actions.empty()) {
            return false;
        }
        *lastGetResponse = nullptr;
        *lastGetResponseLength = 0;
        std::lock_guard<std::mutex> guard(ioMutex_);
        for (size_t i = 0; i < actions.size(); ++i) {
            const auto &action = actions[i];
            if (action.length == 0 || 1 + action.payload.size() > action.length) {
                return false;
            }
            if (scratch->size() < action.length) {
                scratch->resize(action.length);
            }
            std::fill(scratch->begin(), scratch->begin() + action.length, static_cast<uint8_t>(0));
            (*scratch)[0] = action.report;
            if (!action.payload.empty()) {
                memcpy(scratch->data() + 1, action.payload.data(), action.payload.size());
            }
            BOOL ok = action.operation == CtmMapRuntime::PhysicalFeatureOperation::SetFeature
                ? HidD_SetFeature(handle_, scratch->data(), action.length)
                : HidD_GetFeature(handle_, scratch->data(), action.length);
            if (!ok) {
                std::cout << "bt feature issue reason=" << reason
                          << " op=" << (action.operation == CtmMapRuntime::PhysicalFeatureOperation::SetFeature ? "set" : "get")
                          << " report=0x" << std::hex << std::setw(2) << std::setfill('0')
                          << static_cast<unsigned int>(action.report)
                          << std::dec << std::setfill(' ')
                          << " best_effort=" << (action.bestEffort ? "yes" : "no")
                          << " head=" << hex_span(scratch->data(), (std::min<size_t>)(action.length, 24))
                          << std::endl;
                if (action.bestEffort) {
                    continue;
                }
                return false;
            }
            if (action.operation == CtmMapRuntime::PhysicalFeatureOperation::GetFeature) {
                *lastGetResponse = scratch->data();
                *lastGetResponseLength = action.length;
            }
        }
        return *lastGetResponse != nullptr || !actions.empty();
    }

    bool send_output_report(const std::vector<uint8_t> &report, bool paced, std::wstring *error) override
    {
        if (paced) {
            std::lock_guard<std::mutex> lock(pacedMutex_);
            constexpr size_t kMaxQueuedPacedReports = 8;
            if (pacedQueue_.size() >= kMaxQueuedPacedReports) {
                pacedQueue_.pop_front();
                pacedDropped_.fetch_add(1, std::memory_order_relaxed);
            }
            pacedQueue_.push_back(report);
            pacedQueued_.fetch_add(1, std::memory_order_relaxed);
            pacedCv_.notify_one();
            return true;
        }
        std::lock_guard<std::mutex> guard(ioMutex_);
        return write_hid_report(handle_, report, device_.output_report_length, error);
    }

private:
    void reader_loop()
    {
        using clock = std::chrono::steady_clock;
        const DWORD reportLength = device_.input_report_length ? device_.input_report_length : 64;
        std::vector<uint8_t> buffer(reportLength, 0);
        OVERLAPPED ov = {};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (ov.hEvent == nullptr) {
            return;
        }
        uint64_t rxCount = 0;
        uint64_t callbackCount = 0;
        uint64_t readErrors = 0;
        uint64_t gapUsTotal = 0;
        uint32_t gapUsMax = 0;
        uint64_t callbackUsTotal = 0;
        uint32_t callbackUsMax = 0;
        uint64_t lastRxCount = 0;
        uint64_t lastCallbackCount = 0;
        uint64_t lastReadErrors = 0;
        uint64_t lastGapUsTotal = 0;
        uint64_t lastCallbackUsTotal = 0;
        auto lastInput = clock::time_point{};
        auto lastStatus = clock::now();
        while (running_.load() && !g_stop.load()) {
            DWORD readBytes = 0;
            int rc = read_hid_report(handle_, &ov, buffer.data(), reportLength, &readBytes, INFINITE);
            if (rc == 0 && readBytes != 0 && callback_) {
                const auto received = clock::now();
                if (lastInput != clock::time_point{}) {
                    const uint32_t gapUs = static_cast<uint32_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(received - lastInput).count());
                    gapUsTotal += gapUs;
                    gapUsMax = (std::max<uint32_t>)(gapUsMax, gapUs);
                }
                lastInput = received;
                ++rxCount;
                const auto callbackStart = clock::now();
                callback_(buffer.data(), readBytes, 0);
                const uint32_t callbackUs = static_cast<uint32_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        clock::now() - callbackStart).count());
                callbackUsTotal += callbackUs;
                callbackUsMax = (std::max<uint32_t>)(callbackUsMax, callbackUs);
                ++callbackCount;
            } else if (rc < 0) {
                ++readErrors;
                std::wcerr << L"bt read failed\n";
                break;
            }

            const auto now = clock::now();
            if (now - lastStatus >= std::chrono::seconds(2)) {
                const double seconds = std::chrono::duration<double>(now - lastStatus).count();
                const uint64_t rxDelta = rxCount - lastRxCount;
                const uint64_t callbackDelta = callbackCount - lastCallbackCount;
                const uint64_t gapDelta = gapUsTotal - lastGapUsTotal;
                const uint64_t callbackUsDelta = callbackUsTotal - lastCallbackUsTotal;
                const double gapAvgMs = rxDelta <= 1
                    ? 0.0
                    : static_cast<double>(gapDelta) / 1000.0 / static_cast<double>(rxDelta - 1);
                const double callbackAvgUs = callbackDelta == 0
                    ? 0.0
                    : static_cast<double>(callbackUsDelta) / static_cast<double>(callbackDelta);
                std::cout << "bt input"
                          << " rx_hz=" << std::fixed << std::setprecision(1)
                          << static_cast<double>(rxDelta) / seconds
                          << " callback_hz=" << static_cast<double>(callbackDelta) / seconds
                          << " read_gap_avg_ms=" << std::fixed << std::setprecision(3)
                          << gapAvgMs
                          << " read_gap_max_ms=" << std::fixed << std::setprecision(3)
                          << static_cast<double>(gapUsMax) / 1000.0
                          << " callback_avg_us=" << std::fixed << std::setprecision(3)
                          << callbackAvgUs
                          << " callback_max_us=" << std::fixed << std::setprecision(3)
                          << static_cast<double>(callbackUsMax)
                          << " read_errors=" << (readErrors - lastReadErrors)
                          << std::defaultfloat
                          << std::endl;
                lastRxCount = rxCount;
                lastCallbackCount = callbackCount;
                lastReadErrors = readErrors;
                lastGapUsTotal = gapUsTotal;
                lastCallbackUsTotal = callbackUsTotal;
                gapUsMax = 0;
                callbackUsMax = 0;
                lastStatus = now;
            }
        }
        CloseHandle(ov.hEvent);
    }

    void paced_writer_loop()
    {
        using clock = std::chrono::steady_clock;
        const auto pace = std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<double, std::milli>(btPaceMs_));
        auto nextWrite = clock::now();
        auto lastStatus = clock::now();
        unsigned long lastQueued = 0;
        unsigned long lastSent = 0;
        unsigned long lastDropped = 0;

        while (running_.load() && !g_stop.load()) {
            std::vector<uint8_t> report;
            {
                std::unique_lock<std::mutex> lock(pacedMutex_);
                pacedCv_.wait(lock, [&]() {
                    return !pacedQueue_.empty() || !running_.load() || g_stop.load();
                });
                if (!running_.load() || g_stop.load()) {
                    break;
                }
                if (pacedQueue_.empty()) {
                    continue;
                }
                report = std::move(pacedQueue_.front());
                pacedQueue_.pop_front();
            }

            const auto now = clock::now();
            if (nextWrite > now) {
                std::this_thread::sleep_until(nextWrite);
            } else if (now - nextWrite > std::chrono::milliseconds(100)) {
                nextWrite = now;
            }

            std::wstring writeError;
            bool ok = false;
            {
                std::lock_guard<std::mutex> guard(ioMutex_);
                ok = write_hid_report(handle_, report, device_.output_report_length, &writeError);
            }
            if (ok) {
                pacedSent_.fetch_add(1, std::memory_order_relaxed);
            } else {
                const unsigned long failed = pacedFailures_.fetch_add(1, std::memory_order_relaxed) + 1;
                // Log the first few failures verbatim so the cause (size,
                // timeout, OS error) is visible. After that fall back to the
                // 2-second aggregate above.
                if (failed <= 3) {
                    const uint8_t reportId = report.empty() ? 0 : report.front();
                    std::wcerr << L"bt audio write failed"
                               << L" report=0x" << std::hex << std::setw(2) << std::setfill(L'0')
                               << static_cast<unsigned int>(reportId)
                               << std::dec << std::setfill(L' ')
                               << L" size=" << report.size()
                               << L" device_max=" << device_.output_report_length
                               << L" reason=" << writeError
                               << L"\n";
                }
            }
            nextWrite += pace;

            const auto statusNow = clock::now();
            if (statusNow - lastStatus >= std::chrono::seconds(2)) {
                const unsigned long queued = pacedQueued_.load(std::memory_order_relaxed);
                const unsigned long sent = pacedSent_.load(std::memory_order_relaxed);
                const unsigned long dropped = pacedDropped_.load(std::memory_order_relaxed);
                const unsigned long failed = pacedFailures_.load(std::memory_order_relaxed);
                size_t depth = 0;
                {
                    std::lock_guard<std::mutex> lock(pacedMutex_);
                    depth = pacedQueue_.size();
                }
                std::cout << "bt audio"
                          << " queued=" << (queued - lastQueued)
                          << " sent=" << (sent - lastSent)
                          << " dropped=" << (dropped - lastDropped)
                          << " failed_total=" << failed
                          << " depth=" << depth
                          << std::endl;
                lastQueued = queued;
                lastSent = sent;
                lastDropped = dropped;
                lastStatus = statusNow;
            }
        }
    }

    unsigned long index_ = 0;
    double btPaceMs_ = 10.0;
    CtmBtDevice device_ = {};
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    RawInputCallback callback_;
    std::atomic_bool running_{false};
    std::thread reader_;
    std::thread writer_;
    std::mutex ioMutex_;
    std::mutex pacedMutex_;
    std::condition_variable pacedCv_;
    std::deque<std::vector<uint8_t>> pacedQueue_;
    std::atomic<unsigned long> pacedQueued_{0};
    std::atomic<unsigned long> pacedSent_{0};
    std::atomic<unsigned long> pacedDropped_{0};
    std::atomic<unsigned long> pacedFailures_{0};
};
