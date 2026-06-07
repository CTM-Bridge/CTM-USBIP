#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include "ctm/hid.h"
#include "ctm/map/runtime.h"
#include "ctm/profile.h"

#include <hidsdi.h>

#include <enet/enet.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "ws2_32.lib")

namespace {

#include "app/common.inl"
#include "usb/descriptors.inl"
#include "audio/reservoir.inl"
#include "map/diagnostics.inl"
#include "backend/backend.inl"
#include "backend/bt.inl"
#include "backend/bridge.inl"
#include "backend/bridge_enet.inl"
#include "usbip/device.inl"
#include "usbip/server.inl"
#include "app/cli.inl"
#include "app/agent.inl"

} // namespace

int wmain(int argc, wchar_t **argv)
{
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    EnetGlobalGuard enetGuard;
    if (!enetGuard.ok) {
        std::wcerr << L"enet_initialize failed\n";
        return 2;
    }
    if (argc < 2) {
        print_usage();
        return 2;
    }

    std::wstring mode = argv[1];
    bool noAttach = false;
    std::wstring profileOverride;
    std::wstring mapOverride;
    std::wstring busId = kDefaultBusId;
    uint8_t audioLatency = 0x60;
    bool hasAudioBlockOverride = false;
    uint8_t audioBlockOverride = 0;
    uint16_t usbipPort = kDefaultUsbipPort;
    unsigned long number = 0;

    if (mode == L"agent") {
        unsigned long port = kAgentDefaultPort;
        int argIndex = 2;
        if (argc >= 3 && argv[2][0] != L'-') {
            if (!parse_uint_arg(argv[2], 65535, &port)) {
                print_usage();
                return 2;
            }
            argIndex = 3;
        }
        if (port < 1024) {
            print_usage();
            return 2;
        }
        for (int i = argIndex; i < argc; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"--enet") {
                g_use_enet.store(true);
            } else {
                print_usage();
                return 2;
            }
        }
        return run_agent(static_cast<uint16_t>(port));
    }

    if (mode == L"bt") {
        if (argc < 3 || !parse_uint_arg(argv[2], 31, &number)) {
            print_usage();
            return 2;
        }
    } else if (mode == L"bridge") {
        if (argc < 3 || !parse_uint_arg(argv[2], 65535, &number) || number < 1024) {
            print_usage();
            return 2;
        }
    } else {
        print_usage();
        return 2;
    }

    for (int i = 3; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--no-attach") {
            noAttach = true;
        } else if (arg == L"--enet") {
            g_use_enet.store(true);
        } else if (arg == L"--profile" && i + 1 < argc) {
            profileOverride = argv[++i];
        } else if (arg == L"--map" && i + 1 < argc) {
            mapOverride = argv[++i];
        } else if (arg == L"--busid" && i + 1 < argc) {
            busId = argv[++i];
        } else if (arg == L"--audio-latency" && i + 1 < argc) {
            unsigned long value = 0;
            if (!parse_uint_arg(argv[++i], 255, &value)) {
                std::wcerr << L"invalid --audio-latency\n";
                return 2;
            }
            audioLatency = static_cast<uint8_t>(value);
        } else if (arg == L"--audio-block" && i + 1 < argc) {
            unsigned long value = 0;
            if (!parse_uint_arg(argv[++i], 255, &value)) {
                std::wcerr << L"invalid --audio-block\n";
                return 2;
            }
            hasAudioBlockOverride = true;
            audioBlockOverride = static_cast<uint8_t>(value);
        } else if (arg == L"--usbip-port" && i + 1 < argc) {
            unsigned long value = 0;
            if (!parse_uint_arg(argv[++i], 65535, &value) || value < 1024) {
                std::wcerr << L"invalid --usbip-port (1024..65535)\n";
                return 2;
            }
            usbipPort = static_cast<uint16_t>(value);
        } else {
            std::wcerr << L"invalid argument: " << arg << L"\n";
            return 2;
        }
    }

    std::wstring error;
    CtmUsbipDevice device;
    const bool dynamicBridgeProfile =
        mode == L"bridge" && (profileOverride.empty() || profileOverride == L"auto");
    const std::wstring profilePath = dynamicBridgeProfile
        ? L"auto"
        : (profileOverride.empty() ? find_ds5_descriptor_profile() : profileOverride);
    const std::wstring mapPath = mapOverride.empty()
        ? (dynamicBridgeProfile ? find_hid_identity_map_file() : find_ds5_map_file())
        : mapOverride;
    if (dynamicBridgeProfile) {
        if (!device.load_map(mapPath, audioLatency, hasAudioBlockOverride, audioBlockOverride, &error)) {
            std::wcerr << L"load failed: " << error << L"\n";
            return 3;
        }
    } else if (!device.load(profilePath, mapPath, audioLatency, hasAudioBlockOverride, audioBlockOverride, &error)) {
        std::wcerr << L"load failed: " << error << L"\n";
        return 3;
    }
    std::wcout << L"profile: " << profilePath << L"\n";
    std::wcout << L"map: " << mapPath << L"\n";

    std::unique_ptr<CtmBackend> backend;
    EnetBridgeBackend *enetBackend = nullptr;
    if (mode == L"bt") {
        backend = std::make_unique<LocalBtBackend>(number, device.bt_audio_pace_ms());
    } else if (g_use_enet.load()) {
        auto enet = std::make_unique<EnetBridgeBackend>(static_cast<uint16_t>(number), device.bt_audio_pace_ms());
        enetBackend = enet.get();
        backend = std::move(enet);
        std::wcout << L"transport: ENet/UDP (--enet)\n";
    } else {
        backend = std::make_unique<BridgeBackend>(static_cast<uint16_t>(number), device.bt_audio_pace_ms());
    }

    if (!backend->start([&](const uint8_t *data, size_t length, uint8_t endpoint) {
            device.on_physical_input(data, length, endpoint);
        }, &error)) {
        std::wcerr << L"backend start failed: " << error << L"\n";
        return 4;
    }

    if (dynamicBridgeProfile) {
        CtmDescriptorProfile dynamicProfile;
        if (!make_dynamic_hid_profile(backend->caps(), &dynamicProfile, &error) ||
            !device.set_profile(dynamicProfile, &error)) {
            backend->stop();
            std::wcerr << L"dynamic profile failed: " << error << L"\n";
            return 3;
        }
    }

    if (!device.attach_backend(backend.get(), &error)) {
        backend->stop();
        std::wcerr << L"backend attach failed: " << error << L"\n";
        return 5;
    }

    const std::string busIdAscii = narrow_ascii(busId);
    if (busIdAscii.empty() || busIdAscii.size() > 31) {
        std::wcerr << L"invalid --busid: must be non-empty ASCII up to 31 bytes\n";
        backend->stop();
        return 2;
    }

    CtmUsbipServer server(&device, busIdAscii);
    if (!server.start(usbipPort, &error)) {
        backend->stop();
        std::wcerr << L"usbip server start failed: " << error << L"\n";
        return 6;
    }

    // Explicit plug-out / plug-in for the ENet transport on link transitions:
    // on link loss detach the virtual USB device (Windows sees an unplug) and
    // log the unplugged state; on reconnect re-attach it. The TCP BridgeBackend
    // keeps its existing reconnect-in-place behavior and sets no callbacks.
    if (enetBackend != nullptr) {
        const std::wstring attachBusId = busId;
        const uint16_t attachPort = usbipPort;
        const std::string detachBusId = busIdAscii;
        const bool autoAttach = !noAttach;
        enetBackend->set_disconnect_callback([&server, detachBusId]() {
            const bool detached = server.detach_device(detachBusId);
            std::wcout << L"bridge link down: virtual device UNPLUGGED busid="
                       << widen_ascii(detachBusId.c_str(), detachBusId.size())
                       << (detached ? L" (usb/ip client detached)" : L" (no active import)") << L"\n";
        });
        enetBackend->set_reconnect_callback([attachBusId, attachPort, autoAttach]() {
            std::wcout << L"bridge link up: virtual device PLUGGED IN busid=" << attachBusId << L"\n";
            if (autoAttach && !run_usbip_attach(attachBusId, attachPort)) {
                std::wcerr << L"re-attach failed; server remains running for manual attach\n";
            }
        });
    }

    if (!noAttach) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (!run_usbip_attach(busId, usbipPort)) {
            std::wcerr << L"attach failed; server remains running for manual attach\n";
        }
    }

    std::cout << "ctm-usbip running; press Ctrl+C to stop" << std::endl;
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    server.stop();
    device.stop();
    backend->stop();
    SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
    return 0;
}
