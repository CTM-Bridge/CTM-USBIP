constexpr uint16_t kAgentDefaultPort = 48054;

struct AgentBridgeSession {
    std::string kind;
    std::wstring busId;
    std::string busIdAscii;
    uint16_t port = 0;
    std::shared_ptr<CtmUsbipDevice> device;
    std::unique_ptr<CtmBackend> backend;
    // Non-owning view of backend when it is the ENet transport, used to wire the
    // plug-out/plug-in callbacks. nullptr for the TCP BridgeBackend.
    EnetBridgeBackend *enetBackend = nullptr;
    std::thread worker;
    std::atomic_bool stopping{false};
    std::atomic_bool ready{false};
    std::mutex mutex;
    std::wstring lastError;
};

static std::mutex g_agent_sessions_mutex;
static std::vector<std::unique_ptr<AgentBridgeSession>> g_agent_sessions;
static std::unique_ptr<CtmUsbipServer> g_agent_usbip_server;

static std::wstring find_relative_asset(const std::wstring &relative)
{
    const std::wstring exeDir = module_directory();
    const std::vector<std::wstring> candidates = {
        relative,
        L"..\\" + relative,
        exeDir + L"\\" + relative,
        exeDir + L"\\..\\..\\..\\" + relative
    };
    for (const std::wstring &candidate : candidates) {
        if (file_exists(candidate)) return candidate;
    }
    return candidates[0];
}

static std::wstring bridge_profile_for_kind(const std::string &kind)
{
    if (kind == "ds4") {
        return find_relative_asset(L"profiles\\descriptors\\ds4_composite.profile");
    }
    if (kind == "ds5") {
        return find_ds5_descriptor_profile();
    }
    if (kind == "puck") {
        return find_relative_asset(L"profiles\\descriptors\\steam_puck.profile");
    }
    if (kind == "xbox") {
        return find_relative_asset(L"profiles\\descriptors\\xbox_gip_usb.profile");
    }
    return L"auto";
}

static std::wstring bridge_map_for_kind(const std::string &kind)
{
    if (kind == "ds4") {
        return find_relative_asset(L"maps\\ds4_usb_over_ds4_bt.map");
    }
    if (kind == "ds5") {
        return find_ds5_map_file();
    }
    if (kind == "puck") {
        return find_relative_asset(L"maps\\steam_puck_identity.map");
    }
    if (kind == "xbox") {
        return find_relative_asset(L"maps\\xbox_gip_usb_over_xbox_bt.map");
    }
    return find_hid_identity_map_file();
}

static AgentBridgeSession *find_bridge_session_locked(const std::wstring &busId)
{
    for (auto &session : g_agent_sessions) {
        if (session->busId == busId) {
            return session.get();
        }
    }
    return nullptr;
}

static void set_bridge_session_error(AgentBridgeSession *session, const std::wstring &error)
{
    if (!session) {
        return;
    }
    std::lock_guard<std::mutex> lock(session->mutex);
    session->lastError = error;
}

static void bridge_session_worker(AgentBridgeSession *session)
{
    std::wstring error;
    const std::wstring profile = bridge_profile_for_kind(session->kind);
    const std::wstring map = bridge_map_for_kind(session->kind);
    const bool dynamicProfile = profile == L"auto";
    const uint8_t audioLatency = 0x60;
    const bool hasAudioBlockOverride = false;
    const uint8_t audioBlockOverride = 0;

    if (dynamicProfile) {
        if (!session->device->load_map(map, audioLatency, hasAudioBlockOverride, audioBlockOverride, &error)) {
            set_bridge_session_error(session, error);
            std::wcerr << L"agent bridge load failed busid=" << session->busId << L": " << error << L"\n";
            return;
        }
    } else if (!session->device->load(profile, map, audioLatency, hasAudioBlockOverride, audioBlockOverride, &error)) {
        set_bridge_session_error(session, error);
        std::wcerr << L"agent bridge load failed busid=" << session->busId << L": " << error << L"\n";
        return;
    }

    // Transport selection: ENet/UDP when the agent was started with --enet,
    // otherwise the unchanged TCP BridgeBackend. Both bind the same port number.
    CtmBackend *backendPtr = nullptr;
    EnetBridgeBackend *enetBackendPtr = nullptr;
    if (g_use_enet.load()) {
        auto backend = std::make_unique<EnetBridgeBackend>(session->port, session->device->bt_audio_pace_ms());
        enetBackendPtr = backend.get();
        backendPtr = backend.get();
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->stopping.load()) {
            return;
        }
        session->enetBackend = enetBackendPtr;
        session->backend = std::move(backend);
    } else {
        auto backend = std::make_unique<BridgeBackend>(session->port, session->device->bt_audio_pace_ms());
        backendPtr = backend.get();
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->stopping.load()) {
            return;
        }
        session->backend = std::move(backend);
    }

    const std::shared_ptr<CtmUsbipDevice> device = session->device;
    if (!backendPtr->start([device](const uint8_t *data, size_t length, uint8_t endpoint) {
            device->on_physical_input(data, length, endpoint);
        }, &error)) {
        set_bridge_session_error(session, error);
        if (!session->stopping.load()) {
            std::wcerr << L"agent bridge backend failed busid=" << session->busId << L": " << error << L"\n";
        }
        return;
    }

    if (dynamicProfile) {
        CtmDescriptorProfile dynamicDescriptor;
        if (!make_dynamic_hid_profile(backendPtr->caps(), &dynamicDescriptor, &error) ||
            !session->device->set_profile(dynamicDescriptor, &error)) {
            set_bridge_session_error(session, error);
            backendPtr->stop();
            std::wcerr << L"agent bridge dynamic profile failed busid=" << session->busId << L": " << error << L"\n";
            return;
        }
    }

    // Composite puck: replace the static fallback profile with one built from
    // the forwarded enumeration (device + configuration descriptors + each HID
    // interface's report descriptor), presented full-speed. The enum arrives
    // during the handshake (CTMB_MSG_ENUM, before HELLO) and is stored in the
    // backend. If it did not arrive, the static profile loaded above stands.
    if (session->kind == "puck" && !backendPtr->enum_payload().empty()) {
        CtmDescriptorProfile compositeDescriptor;
        std::wstring compositeError;
        if (make_composite_profile_from_enum(backendPtr->enum_payload(), &compositeDescriptor, &compositeError) &&
            session->device->set_profile(compositeDescriptor, &compositeError)) {
            std::wcerr << L"agent bridge composite profile built busid=" << session->busId
                       << L" hid_ifaces=" << compositeDescriptor.interface_report_descriptors.size()
                       << L" full_speed=" << (compositeDescriptor.full_speed ? 1 : 0) << L"\n";
        } else {
            std::wcerr << L"agent bridge composite build failed busid=" << session->busId
                       << L" (keeping static profile): " << compositeError << L"\n";
        }
    }

    if (!session->device->attach_backend(backendPtr, &error)) {
        set_bridge_session_error(session, error);
        backendPtr->stop();
        std::wcerr << L"agent bridge attach failed busid=" << session->busId << L": " << error << L"\n";
        return;
    }

    if (!g_agent_usbip_server ||
        !g_agent_usbip_server->add_device(session->device, session->busIdAscii, &error)) {
        set_bridge_session_error(session, error);
        backendPtr->stop();
        std::wcerr << L"agent USB/IP export failed busid=" << session->busId << L": " << error << L"\n";
        return;
    }

    // For the ENet transport, surface an explicit plug-out on link loss and a
    // plug-in on reconnect. Detach mirrors BRIDGE_STOP: remove the export from
    // the shared USB/IP server (Windows sees an unplug). Reconnect re-exports
    // and re-attaches the same device. The TCP path sets no callbacks and keeps
    // its reconnect-in-place behavior unchanged.
    if (enetBackendPtr != nullptr) {
        const std::wstring busId = session->busId;
        const std::string busIdAscii = session->busIdAscii;
        const std::shared_ptr<CtmUsbipDevice> sessionDevice = session->device;
        enetBackendPtr->set_disconnect_callback([busId, busIdAscii]() {
            std::wcout << L"agent bridge link down busid=" << busId
                       << L" -> virtual device UNPLUGGED\n";
            if (g_agent_usbip_server) {
                g_agent_usbip_server->remove_device(busIdAscii);
            }
        });
        enetBackendPtr->set_reconnect_callback([busId, busIdAscii, sessionDevice]() {
            std::wcout << L"agent bridge link up busid=" << busId
                       << L" -> virtual device PLUGGED IN\n";
            std::wstring reAddError;
            if (g_agent_usbip_server &&
                g_agent_usbip_server->add_device(sessionDevice, busIdAscii, &reAddError)) {
                if (!run_usbip_attach(busId, kDefaultUsbipPort)) {
                    std::wcerr << L"agent re-attach failed busid=" << busId << L"\n";
                }
            } else {
                std::wcerr << L"agent re-export failed busid=" << busId << L": " << reAddError << L"\n";
            }
        });
    }

    session->ready.store(true);
    std::wcout << L"agent bridge ready kind=" << widen_ascii(session->kind.c_str(), session->kind.size())
               << L" port=" << session->port << L" busid=" << session->busId << L"\n";
    if (!run_usbip_attach(session->busId, kDefaultUsbipPort)) {
        std::wcerr << L"agent local attach failed busid=" << session->busId << L"\n";
    }
}

static bool start_bridge_session(const std::string &kind, uint16_t port, const std::wstring &busId, std::wstring *error)
{
    const std::string busIdAscii = narrow_ascii(busId);
    if (busIdAscii.empty() || busIdAscii.size() > 31) {
        if (error) *error = L"invalid bridge busid";
        return false;
    }

    std::lock_guard<std::mutex> lock(g_agent_sessions_mutex);
    if (find_bridge_session_locked(busId)) {
        return true;
    }

    auto session = std::make_unique<AgentBridgeSession>();
    session->kind = kind;
    session->busId = busId;
    session->busIdAscii = busIdAscii;
    session->port = port;
    session->device = std::make_shared<CtmUsbipDevice>();
    AgentBridgeSession *sessionPtr = session.get();
    session->worker = std::thread([sessionPtr]() { bridge_session_worker(sessionPtr); });
    g_agent_sessions.push_back(std::move(session));
    std::wcout << L"agent bridge starting kind=" << widen_ascii(kind.c_str(), kind.size())
               << L" port=" << port << L" busid=" << busId << L"\n";
    return true;
}

static bool stop_bridge_session(const std::wstring &busId)
{
    std::unique_ptr<AgentBridgeSession> session;
    {
        std::lock_guard<std::mutex> lock(g_agent_sessions_mutex);
        for (size_t i = 0; i < g_agent_sessions.size(); ++i) {
            if (g_agent_sessions[i]->busId != busId) {
                continue;
            }
            session = std::move(g_agent_sessions[i]);
            g_agent_sessions.erase(g_agent_sessions.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    if (!session) {
        return false;
    }
    session->stopping.store(true);
    if (g_agent_usbip_server) {
        g_agent_usbip_server->remove_device(session->busIdAscii);
    }
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->backend) {
            session->backend->stop();
        }
    }
    if (session->device) {
        session->device->wake_input_waiters();
        session->device->stop();
    }
    if (session->worker.joinable()) {
        session->worker.join();
    }
    std::wcout << L"agent bridge stopped busid=" << busId << L"\n";
    return true;
}

static void stop_all_bridge_sessions()
{
    std::vector<std::wstring> busIds;
    {
        std::lock_guard<std::mutex> lock(g_agent_sessions_mutex);
        for (const auto &session : g_agent_sessions) {
            busIds.push_back(session->busId);
        }
    }
    for (const std::wstring &busId : busIds) {
        (void)stop_bridge_session(busId);
    }
}

static void send_text(SOCKET sock, const std::string &text)
{
    send(sock, text.c_str(), static_cast<int>(text.size()), 0);
}

static void handle_agent_client(SOCKET client, const sockaddr_in &peer)
{
    char line[512] = {};
    int n = recv(client, line, sizeof(line) - 1, 0);
    if (n <= 0) {
        return;
    }
    line[n] = '\0';
    char *nl = strpbrk(line, "\r\n");
    if (nl) *nl = '\0';

    std::istringstream input(line);
    std::string command;
    input >> command;
    if (command == "STATUS") {
        send_text(client, "OK CTM_AGENT_V1\n");
        return;
    }

    if (command == "USBIP_ATTACH") {
        std::string busIdAscii;
        input >> busIdAscii;
        if (busIdAscii.empty() || busIdAscii.size() > 31) {
            send_text(client, "ERR bad busid\n");
            return;
        }
        char peerIp[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &peer.sin_addr, peerIp, sizeof(peerIp));
        if (run_usbip_attach_to(widen_ascii(peerIp, strlen(peerIp)), widen_ascii(busIdAscii.c_str(), busIdAscii.size()), kDefaultUsbipPort)) {
            send_text(client, "OK usbip attached\n");
        } else {
            send_text(client, "ERR usbip attach failed\n");
        }
        return;
    }

    if (command == "BRIDGE_START") {
        std::string kind;
        unsigned long port = 0;
        std::string busIdAscii;
        input >> kind >> port >> busIdAscii;
        if ((kind != "ds4" && kind != "ds5" && kind != "hid" && kind != "puck" && kind != "xbox") ||
            port < 1024 || port > 65535 ||
            busIdAscii.empty() || busIdAscii.size() > 31) {
            send_text(client, "ERR bad bridge args\n");
            return;
        }
        std::wstring error;
        if (start_bridge_session(kind, static_cast<uint16_t>(port), widen_ascii(busIdAscii.c_str(), busIdAscii.size()), &error)) {
            send_text(client, "OK bridge starting\n");
        } else {
            std::string err = narrow_ascii(error);
            send_text(client, "ERR " + (err.empty() ? std::string("bridge start failed") : err) + "\n");
        }
        return;
    }

    if (command == "BRIDGE_STOP") {
        std::string busIdAscii;
        input >> busIdAscii;
        if (busIdAscii.empty()) {
            send_text(client, "ERR bad busid\n");
            return;
        }
        // Actually tear the session down: removes the device from the shared
        // USB/IP server (Windows sees an unplug), stops the cmub_ backend,
        // joins the worker. This is what makes "controller off" cleanly
        // remove the virtual device instead of leaving a stale export.
        const std::wstring busId = widen_ascii(busIdAscii.c_str(), busIdAscii.size());
        if (stop_bridge_session(busId)) {
            send_text(client, "OK bridge stopped\n");
        } else {
            send_text(client, "OK no such bridge\n");
        }
        return;
    }

    send_text(client, "ERR unknown command\n");
}

static int run_agent(uint16_t port)
{
    WSADATA data = {};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        std::wcerr << wsa_error_message(L"WSAStartup failed") << L"\n";
        return 4;
    }

    SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    SOCKET tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (udp == INVALID_SOCKET || tcp == INVALID_SOCKET) {
        std::wcerr << wsa_error_message(L"agent socket failed") << L"\n";
        WSACleanup();
        return 4;
    }

    BOOL reuse = TRUE;
    setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
    setsockopt(tcp, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(udp, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        bind(tcp, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(tcp, 8) == SOCKET_ERROR) {
        std::wcerr << wsa_error_message(L"agent bind/listen failed") << L"\n";
        closesocket(udp);
        closesocket(tcp);
        WSACleanup();
        return 4;
    }

    g_agent_usbip_server = std::make_unique<CtmUsbipServer>();
    std::wstring usbipError;
    if (!g_agent_usbip_server->start(kDefaultUsbipPort, &usbipError)) {
        std::wcerr << L"agent USB/IP server failed: " << usbipError << L"\n";
        closesocket(udp);
        closesocket(tcp);
        WSACleanup();
        return 4;
    }

    std::wcout << L"ctm agent listening udp/tcp port " << port << L"\n";
    while (!g_stop.load()) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(udp, &readfds);
        FD_SET(tcp, &readfds);
        timeval timeout {};
        timeout.tv_sec = 1;
        int rc = select(0, &readfds, nullptr, nullptr, &timeout);
        if (rc <= 0) {
            continue;
        }
        if (FD_ISSET(udp, &readfds)) {
            char buf[128] = {};
            sockaddr_in from = {};
            int fromLen = sizeof(from);
            int n = recvfrom(udp, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr *>(&from), &fromLen);
            if (n > 0) {
                buf[n] = '\0';
                if (strncmp(buf, "CTM_DISCOVER_V1", 15) == 0) {
                    std::string reply = "CTM_AGENT_V1 port=" + std::to_string(port) + "\n";
                    sendto(udp, reply.c_str(), static_cast<int>(reply.size()), 0,
                           reinterpret_cast<sockaddr *>(&from), fromLen);
                }
            }
        }
        if (FD_ISSET(tcp, &readfds)) {
            sockaddr_in peer = {};
            int peerLen = sizeof(peer);
            SOCKET client = accept(tcp, reinterpret_cast<sockaddr *>(&peer), &peerLen);
            if (client != INVALID_SOCKET) {
                handle_agent_client(client, peer);
                closesocket(client);
            }
        }
    }

    stop_all_bridge_sessions();
    if (g_agent_usbip_server) {
        g_agent_usbip_server->stop();
        g_agent_usbip_server.reset();
    }
    closesocket(udp);
    closesocket(tcp);
    WSACleanup();
    return 0;
}
