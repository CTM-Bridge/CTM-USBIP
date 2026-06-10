static std::wstring resolve_usbip_exe_path()
{
    // 1. Next to ctm-usbip.exe (CTM Bridge install layout — bundled in
    //    the Sunshine installer next to ctm-usbip).
    wchar_t selfBuf[MAX_PATH] = {0};
    DWORD selfLen = GetModuleFileNameW(nullptr, selfBuf, MAX_PATH);
    if (selfLen > 0 && selfLen < MAX_PATH) {
        std::wstring self = selfBuf;
        const size_t slash = self.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            std::wstring sibling = self.substr(0, slash + 1) + L"usbip.exe";
            if (file_exists(sibling)) return sibling;
        }
    }
    // 2. usbip-win2 default install location.
    const std::wstring legacy = L"C:\\Program Files\\USBip\\usbip.exe";
    if (file_exists(legacy)) return legacy;
    // 3. PATH (let CreateProcess search).
    return L"usbip.exe";
}

static bool run_usbip_attach_to(const std::wstring &remote, const std::wstring &busId,
                                uint16_t usbipPort)
{
    const std::wstring usbip = resolve_usbip_exe_path();
    if (usbip != L"usbip.exe" && !file_exists(usbip)) {
        std::wcerr << L"usbip.exe not found: " << usbip << L"\n";
        return false;
    }
    // -t/--tcp-port is a GLOBAL option (before the subcommand) on usbip-win2.
    // Each ctm-usbip instance runs its own USB/IP server on a distinct port
    // so multiple controllers don't fight over 3240.
    std::wstring command = L"\"" + usbip + L"\" -t " + std::to_wstring(usbipPort) +
                           L" attach -r " + remote + L" -b " + busId;
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::wcout << L"running: " << command << L"\n";
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        std::wcerr << last_error_message(L"CreateProcess usbip attach failed") << L"\n";
        return false;
    }
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0) {
        std::wcerr << L"usbip attach exited with code " << code << L"\n";
        return false;
    }
    return true;
}

static bool run_usbip_attach(const std::wstring &busId, uint16_t usbipPort)
{
    return run_usbip_attach_to(L"127.0.0.1", busId, usbipPort);
}

static void print_usage()
{
    std::wcout
        << L"usage:\n"
        << L"  ctm-usbip bt <index> [--no-attach] [--profile <file>] [--map <file>] [--busid <id>] [--audio-latency <byte>] [--audio-block <byte>]\n"
        << L"  ctm-usbip bridge <listen-port> [--enet] [--no-attach] [--profile auto|<file>] [--map <file>] [--busid <id>] [--audio-latency <byte>] [--audio-block <byte>]\n"
        << L"  ctm-usbip agent [control-port] [--enet]\n"
        << L"  ctm-usbip install [control-port] [--enet]    (register + start the Windows service)\n"
        << L"  ctm-usbip uninstall                          (stop + remove the Windows service)\n"
        << L"  ctm-usbip service-run [control-port] [--enet] (internal: launched by the SCM)\n"
        << L"  ctm-usbip version\n"
        << L"  (--enet selects the additive ENet/UDP transport on the same port; without it the TCP transport is used.)\n";
}
