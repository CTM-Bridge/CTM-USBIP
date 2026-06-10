// Windows service wrapper for `ctm-usbip agent`. The agent loop (run_agent) is
// already a daemon; this adds SCM plumbing so it can run as an auto-start
// LocalSystem service, plus install/uninstall and a bounded logfile tee (there
// is no console in Session 0). Interactive agent/bridge/bt modes are untouched.
//
// Self-restart: a hard restart (RESTART hard over the control channel, via
// request_service_restart) signals the loop to stop and flags a failure exit so
// the SCM restart recovery action fires. That recovery only triggers for a
// "non-crash" stop when the failure-actions flag is set, which install() does.

static const wchar_t *kServiceName = L"ctm-usbip";
static const wchar_t *kServiceDisplayName = L"CTM USB/IP Bridge Agent";
static const wchar_t *kServiceDescription =
    L"Presents controllers forwarded from the CTM webOS TV app as virtual USB "
    L"devices through the usbip-win2 virtual host controller.";
static const wchar_t *kFirewallRuleName = L"CTM USB-IP Bridge";

static SERVICE_STATUS_HANDLE g_service_status_handle = nullptr;
static SERVICE_STATUS g_service_status = {};
static uint16_t g_service_port = kAgentDefaultPort;

// std::wstreambuf that writes wcout/wcerr to a logfile, truncated on start and
// wrapped (truncated from the top) once it passes maxBytes so the file stays
// bounded over a long-running service. Flushed per write for crash-safety;
// guarded by a mutex because bridge worker threads log concurrently.
class CappedFileLog : public std::wstreambuf {
public:
    CappedFileLog(std::wstring path, std::streamoff maxBytes)
        : path_(std::move(path)), maxBytes_(maxBytes)
    {
        reopen();
    }

protected:
    int_type overflow(int_type c) override
    {
        if (traits_type::eq_int_type(c, traits_type::eof())) {
            return traits_type::not_eof(c);
        }
        const wchar_t ch = traits_type::to_char_type(c);
        write_chars(&ch, 1);
        return c;
    }

    std::streamsize xsputn(const wchar_t *s, std::streamsize n) override
    {
        write_chars(s, n);
        return n;
    }

    int sync() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        file_.flush();
        return 0;
    }

private:
    void reopen()
    {
        file_.close();
        file_.clear();
        file_.open(path_.c_str(), std::ios::out | std::ios::trunc);
        written_ = 0;
    }

    void write_chars(const wchar_t *s, std::streamsize n)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!file_.is_open()) {
            return;
        }
        const std::streamoff bytes = static_cast<std::streamoff>(n) * static_cast<std::streamoff>(sizeof(wchar_t));
        if (written_ + bytes > maxBytes_) {
            reopen();
        }
        file_.write(s, n);
        file_.flush();
        written_ += bytes;
    }

    std::wstring path_;
    std::streamoff maxBytes_;
    std::streamoff written_ = 0;
    std::wofstream file_;
    std::mutex mutex_;
};

// Logfile path for service mode: %ProgramData%\CTM Bridge\ctm-usbip.log (the
// exe may live under Program Files, which we don't want to write into). Creates
// the directory; falls back to beside-the-exe if ProgramData is unavailable.
static std::wstring service_log_path()
{
    wchar_t programData[MAX_PATH] = {};
    const DWORD len = GetEnvironmentVariableW(L"ProgramData", programData, ARRAYSIZE(programData));
    if (len == 0 || len >= ARRAYSIZE(programData)) {
        return module_directory() + L"\\ctm-usbip.log";
    }
    const std::wstring dir = std::wstring(programData) + L"\\CTM Bridge";
    CreateDirectoryW(dir.c_str(), nullptr); // succeeds or ERROR_ALREADY_EXISTS
    return dir + L"\\ctm-usbip.log";
}

static void set_service_state(DWORD state, DWORD win32Exit = NO_ERROR, DWORD specificExit = 0)
{
    static DWORD checkpoint = 1;
    g_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_service_status.dwCurrentState = state;
    g_service_status.dwWin32ExitCode = win32Exit;
    g_service_status.dwServiceSpecificExitCode = specificExit;
    g_service_status.dwWaitHint =
        (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) ? 5000 : 0;
    g_service_status.dwControlsAccepted =
        (state == SERVICE_RUNNING || state == SERVICE_STOP_PENDING)
            ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN)
            : 0;
    g_service_status.dwCheckPoint =
        (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;
    if (g_service_status_handle) {
        SetServiceStatus(g_service_status_handle, &g_service_status);
    }
}

// Requested over the control channel (RESTART hard). No-op in interactive agent
// mode (returns false so the caller can report "service mode only"). Forward
// declared in app/common.inl so agent.inl can call it.
static bool request_service_restart()
{
    if (!g_running_as_service.load()) {
        return false;
    }
    g_restart_requested.store(true);
    g_stop.store(true);
    return true;
}

static void WINAPI service_ctrl_handler(DWORD ctrl)
{
    switch (ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        set_service_state(SERVICE_STOP_PENDING);
        g_stop.store(true);
        break;
    default:
        break;
    }
}

static void WINAPI service_main(DWORD /*argc*/, wchar_t ** /*argv*/)
{
    g_service_status_handle = RegisterServiceCtrlHandlerW(kServiceName, service_ctrl_handler);
    if (!g_service_status_handle) {
        return;
    }
    set_service_state(SERVICE_START_PENDING);

    // No console in Session 0: tee wcout/wcerr to a bounded logfile next to the
    // exe (CTM_USBIP_VERBOSE still controls per-endpoint detail).
    static CappedFileLog logBuf(service_log_path(), 5 * 1024 * 1024);
    std::wcout.rdbuf(&logBuf);
    std::wcerr.rdbuf(&logBuf);

    set_service_state(SERVICE_RUNNING);
    const int rc = run_agent(g_service_port);

    if (g_restart_requested.load()) {
        // Intentional hard restart: report a failure exit so the configured
        // SC_ACTION_RESTART recovery fires (install() enables it for non-crash
        // stops via SERVICE_CONFIG_FAILURE_ACTIONS_FLAG).
        set_service_state(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 1);
    } else if (rc != 0) {
        set_service_state(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, static_cast<DWORD>(rc));
    } else {
        set_service_state(SERVICE_STOPPED);
    }
}

static int run_service()
{
    SERVICE_TABLE_ENTRYW table[] = {
        { const_cast<wchar_t *>(kServiceName), service_main },
        { nullptr, nullptr }
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        const DWORD err = GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            std::wcerr << L"service-run must be launched by the SCM; use: ctm-usbip install\n";
        } else {
            std::wcerr << last_error_message(L"StartServiceCtrlDispatcher failed") << L"\n";
        }
        return 1;
    }
    return 0;
}

static DWORD run_command_wait(const std::wstring &command)
{
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return 0xFFFFFFFFu;
    }
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 0xFFFFFFFFu;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

// LAN-scoped (private profile) inbound rule for the agent. This is program-
// based, not port-based: besides the control/discovery port, each bridge opens
// a dynamic per-controller data port (e.g. 48055) that the TV must reach, so a
// single rule allowing the executable on any port is required.
static void add_firewall_rules()
{
    wchar_t self[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, self, ARRAYSIZE(self)) == 0) {
        return;
    }
    run_command_wait(std::wstring(L"netsh advfirewall firewall add rule name=\"") +
                     kFirewallRuleName + L"\" dir=in action=allow profile=private enable=yes program=\"" +
                     self + L"\"");
}

static void remove_firewall_rules()
{
    run_command_wait(std::wstring(L"netsh advfirewall firewall delete rule name=\"") +
                     kFirewallRuleName + L"\"");
}

static std::wstring service_image_path(uint16_t port, bool useEnet)
{
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
    std::wstring command = std::wstring(L"\"") + self + L"\" service-run " + std::to_wstring(port);
    if (useEnet) {
        command += L" --enet";
    }
    return command;
}

static int service_install(uint16_t port, bool useEnet)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT);
    if (!scm) {
        const DWORD err = GetLastError();
        std::wcerr << last_error_message(L"OpenSCManager failed") << L"\n";
        if (err == ERROR_ACCESS_DENIED) {
            std::wcerr << L"run from an elevated (Administrator) prompt\n";
        }
        return 1;
    }

    const std::wstring image = service_image_path(port, useEnet);
    SC_HANDLE svc = CreateServiceW(
        scm, kServiceName, kServiceDisplayName,
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL, image.c_str(),
        nullptr, nullptr, nullptr, nullptr /* LocalSystem */, nullptr);
    if (!svc) {
        const DWORD err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS) {
            std::wcerr << L"service '" << kServiceName << L"' already exists; uninstall first\n";
        } else {
            std::wcerr << last_error_message(L"CreateService failed") << L"\n";
        }
        CloseServiceHandle(scm);
        return 1;
    }

    SERVICE_DESCRIPTIONW desc = { const_cast<wchar_t *>(kServiceDescription) };
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

    // Restart on failure (and treat a non-crash failure exit as a failure, so
    // the RESTART-hard path actually restarts the service).
    SC_ACTION actions[] = {
        { SC_ACTION_RESTART, 5000 },
        { SC_ACTION_RESTART, 5000 },
        { SC_ACTION_RESTART, 5000 },
    };
    SERVICE_FAILURE_ACTIONSW failure = {};
    failure.dwResetPeriod = 86400;
    failure.cActions = ARRAYSIZE(actions);
    failure.lpsaActions = actions;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);
    SERVICE_FAILURE_ACTIONS_FLAG failureFlag = { TRUE };
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &failureFlag);

    remove_firewall_rules();
    add_firewall_rules();

    if (!StartServiceW(svc, 0, nullptr)) {
        std::wcerr << last_error_message(L"service installed but StartService failed") << L"\n";
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    std::wcout << L"installed and started service '" << kServiceName << L"' on port " << port
               << (useEnet ? L" (--enet)" : L"") << L"\n";
    return 0;
}

static int service_uninstall()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        const DWORD err = GetLastError();
        std::wcerr << last_error_message(L"OpenSCManager failed") << L"\n";
        if (err == ERROR_ACCESS_DENIED) {
            std::wcerr << L"run from an elevated (Administrator) prompt\n";
        }
        return 1;
    }

    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!svc) {
        const DWORD err = GetLastError();
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            std::wcout << L"service '" << kServiceName << L"' is not installed\n";
            CloseServiceHandle(scm);
            return 0;
        }
        std::wcerr << last_error_message(L"OpenService failed") << L"\n";
        CloseServiceHandle(scm);
        return 1;
    }

    SERVICE_STATUS status = {};
    ControlService(svc, SERVICE_CONTROL_STOP, &status); // best effort
    if (!DeleteService(svc)) {
        std::wcerr << last_error_message(L"DeleteService failed") << L"\n";
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return 1;
    }

    remove_firewall_rules();
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    std::wcout << L"uninstalled service '" << kServiceName << L"'\n";
    return 0;
}
