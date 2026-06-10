// ctm-usbdisplay — spike: emulate a DisplayLink gen1 USB graphics device over
// USB/IP so the stock Windows DisplayLink driver binds to it, then decode the
// pixel stream it pushes and show it in a resizable preview window.
//
//   ctm-usbdisplay [port] [busid]      run server + preview window, auto-attach
//   ctm-usbdisplay --selftest <png>    decode a synthetic DL stream -> PNG (no driver)
//
// See docs/displaylink_protocol.md.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <unknwn.h>     // IUnknown / IStream for gdiplus
#include <objidl.h>     // PROPID and the COM type chain gdiplus needs
#include <gdiplus.h>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "dl.h"
#include "usbip_server.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdiplus.lib")

static DlFramebuffer g_fb;
static DlDecoder g_decoder(&g_fb);

// ---- synthetic stream (self-test) ---------------------------------------
static void synth_set_register(std::vector<uint8_t> *s, uint8_t reg, uint8_t val)
{
    s->push_back(0xAF); s->push_back(0x20); s->push_back(reg); s->push_back(val);
}

// Build a DL command stream: set mode WxH, gradient top half (raw spans),
// solid green bottom half (run/repeat spans). Exercises both decode paths.
static std::vector<uint8_t> synth_stream(int w, int h)
{
    std::vector<uint8_t> s;
    synth_set_register(&s, 0xFF, 0x00);                 // lock
    synth_set_register(&s, 0x00, 0x00);                 // color depth
    synth_set_register(&s, 0x0F, uint8_t(w >> 8)); synth_set_register(&s, 0x10, uint8_t(w));
    synth_set_register(&s, 0x17, uint8_t(h >> 8)); synth_set_register(&s, 0x18, uint8_t(h));
    synth_set_register(&s, 0x1F, 0x00);                 // unblank
    synth_set_register(&s, 0xFF, 0xFF);                 // unlock -> resize

    auto put_be16 = [&](uint16_t p) { s.push_back(uint8_t(p >> 8)); s.push_back(uint8_t(p)); };
    for (int y = 0; y < h; ++y) {
        const uint32_t addr = uint32_t(y) * uint32_t(w) * 2;   // base16 = 0
        s.push_back(0xAF); s.push_back(0x6B);
        s.push_back(uint8_t(addr >> 16)); s.push_back(uint8_t(addr >> 8)); s.push_back(uint8_t(addr));
        if (y < h / 2) {
            // raw span: one literal per pixel (cmd=w, raw=w; w<=256 here)
            s.push_back(uint8_t(w & 0xFF)); s.push_back(uint8_t(w & 0xFF));
            for (int x = 0; x < w; ++x) {
                const uint16_t r5 = uint16_t(x * 31 / (w - 1));
                const uint16_t g6 = uint16_t(y * 63 / (h - 1));
                put_be16(uint16_t((r5 << 11) | (g6 << 5)));
            }
        } else {
            // run span: 1 literal + (w-1) repeats of solid green
            s.push_back(uint8_t(w & 0xFF));              // cmd = w pixels
            s.push_back(0x01);                            // raw = 1 literal
            put_be16(0x07E0);                             // green
            s.push_back(uint8_t((w - 1) & 0xFF));         // repeat = w-1 extra
        }
    }
    return s;
}

static int GetEncoderClsid(const WCHAR *format, CLSID *clsid)
{
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    std::vector<uint8_t> buf(size);
    auto *info = reinterpret_cast<Gdiplus::ImageCodecInfo *>(buf.data());
    Gdiplus::GetImageEncoders(num, size, info);
    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(info[j].MimeType, format) == 0) { *clsid = info[j].Clsid; return int(j); }
    }
    return -1;
}

static int run_selftest(const std::wstring &pngPath)
{
    const int w = 256, h = 128;
    const std::vector<uint8_t> stream = synth_stream(w, h);
    g_decoder.decode(stream.data(), stream.size());

    std::lock_guard<std::mutex> lock(g_fb.mutex);
    if (g_fb.width != w || g_fb.height != h) {
        std::wcerr << L"selftest: decode produced " << g_fb.width << L"x" << g_fb.height
                   << L", expected " << w << L"x" << h << L"\n";
        return 2;
    }
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartupInput in;
    Gdiplus::GdiplusStartup(&token, &in, nullptr);
    {
        Gdiplus::Bitmap bmp(w, h, PixelFormat32bppARGB);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const uint32_t rgb = dl_565_to_rgb888(g_fb.pixels[size_t(y) * w + x]);
                bmp.SetPixel(x, y, Gdiplus::Color(0xFF000000 | rgb));
            }
        CLSID png;
        if (GetEncoderClsid(L"image/png", &png) < 0) { Gdiplus::GdiplusShutdown(token); return 3; }
        bmp.Save(pngPath.c_str(), &png, nullptr);
    }
    Gdiplus::GdiplusShutdown(token);
    std::wcout << L"selftest: decoded " << w << L"x" << h << L" -> " << pngPath << L"\n";
    return 0;
}

// ---- preview window ------------------------------------------------------
static uint64_t g_lastGen = ~0ull;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER: {
        uint64_t gen;
        { std::lock_guard<std::mutex> lock(g_fb.mutex); gen = g_fb.generation; }
        if (gen != g_lastGen) { g_lastGen = gen; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    }
    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        int w, h;
        std::vector<uint32_t> argb;
        {
            std::lock_guard<std::mutex> lock(g_fb.mutex);
            w = g_fb.width; h = g_fb.height;
            if (w > 0 && h > 0) {
                argb.resize(size_t(w) * h);
                for (size_t k = 0; k < argb.size(); ++k)
                    argb[k] = 0xFF000000 | dl_565_to_rgb888(g_fb.pixels[k]);
            }
        }
        RECT rc; GetClientRect(hwnd, &rc);
        if (w > 0 && h > 0) {
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = -h;          // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, HALFTONE);
            StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, w, h,
                          argb.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        } else {
            FillRect(dc, &rc, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ---- usbip attach (best effort) -----------------------------------------
static std::wstring resolve_usbip_exe()
{
    wchar_t self[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) > 0) {
        std::wstring s = self;
        const size_t slash = s.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            std::wstring sib = s.substr(0, slash + 1) + L"usbip.exe";
            if (GetFileAttributesW(sib.c_str()) != INVALID_FILE_ATTRIBUTES) return sib;
        }
    }
    const std::wstring legacy = L"C:\\Program Files\\USBip\\usbip.exe";
    if (GetFileAttributesW(legacy.c_str()) != INVALID_FILE_ATTRIBUTES) return legacy;
    return L"usbip.exe";
}

static void run_usbip_attach(uint16_t port, const std::wstring &busId)
{
    std::wstring cmd = L"\"" + resolve_usbip_exe() + L"\" -t " + std::to_wstring(port) +
                       L" attach -r 127.0.0.1 -b " + busId;
    std::vector<wchar_t> mut(cmd.begin(), cmd.end());
    mut.push_back(0);
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::wcout << L"running: " << cmd << L"\n";
    if (CreateProcessW(nullptr, mut.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 30000);
        DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        if (code != 0) std::wcerr << L"usbip attach exited " << code << L" (server stays up for manual attach)\n";
    } else {
        std::wcerr << L"could not launch usbip.exe (attach manually)\n";
    }
}

int wmain(int argc, wchar_t **argv)
{
    if (argc >= 3 && std::wstring(argv[1]) == L"--selftest") {
        return run_selftest(argv[2]);
    }

    uint16_t port = 3242;                 // own port; 3240 is the HID agent's
    std::wstring busId = L"ctm-display";
    if (argc >= 2) port = uint16_t(_wtoi(argv[1]));
    if (argc >= 3) busId = argv[2];

    std::string busAscii;
    for (wchar_t ch : busId) busAscii.push_back(char(ch));

    dlusbip::DlUsbipServer server(&g_decoder, &g_fb, busAscii);
    if (!server.start(port)) { std::wcerr << L"usbip server failed to start\n"; return 4; }

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    run_usbip_attach(port, busId);

    const wchar_t *cls = L"CtmUsbDisplayPreview";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(cls, L"CTM USB Display (preview)", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1000, 600,
                              nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    SetTimer(hwnd, 1, 33, nullptr);     // ~30fps repaint poll

    MSG m;
    while (GetMessage(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    server.stop();
    return 0;
}
