#pragma once
// Minimal USB/IP server presenting the emulated DisplayLink gen1 device. Handles
// only what this device needs: OP_DEVLIST / OP_IMPORT, EP0 control (descriptors,
// EDID readback, channel-select), and the bulk-OUT pixel stream (fed to the
// DlDecoder). Deliberately separate from the HID-oriented CtmUsbipServer.

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dl.h"

namespace dlusbip {

constexpr uint16_t kVersion = 0x0111;
constexpr uint16_t kOpReqDevlist = 0x8005, kOpRepDevlist = 0x0005;
constexpr uint16_t kOpReqImport = 0x8003, kOpRepImport = 0x0003;
constexpr uint32_t kCmdSubmit = 1, kCmdUnlink = 2, kRetSubmit = 3, kRetUnlink = 4;
constexpr uint32_t kDirIn = 1;
constexpr int32_t kOk = 0, kStall = -32;
constexpr uint32_t kSpeedHigh = 3;

inline void put16(std::vector<uint8_t> *o, uint16_t v) { o->push_back(v >> 8); o->push_back(v & 0xff); }
inline void put32(std::vector<uint8_t> *o, uint32_t v) { o->push_back(v >> 24); o->push_back(v >> 16); o->push_back(v >> 8); o->push_back(v); }
inline uint16_t rd16(const uint8_t *p) { return (uint16_t(p[0]) << 8) | p[1]; }
inline uint32_t rd32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

inline bool recv_all(SOCKET s, uint8_t *d, size_t n)
{
    size_t got = 0;
    while (got < n) {
        int r = recv(s, reinterpret_cast<char *>(d + got), int(n - got), 0);
        if (r <= 0) return false;
        got += size_t(r);
    }
    return true;
}
inline bool send_all(SOCKET s, const uint8_t *d, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        int r = send(s, reinterpret_cast<const char *>(d + sent), int(n - sent), 0);
        if (r <= 0) return false;
        sent += size_t(r);
    }
    return true;
}

class DlUsbipServer {
public:
    DlUsbipServer(DlDecoder *decoder, DlFramebuffer *fb, std::string busId)
        : decoder_(decoder), fb_(fb), busId_(std::move(busId))
    {
        edid_ = dl_build_edid();
        vendorDesc_ = dl_build_vendor_descriptor();
        build_descriptors();
    }

    bool start(uint16_t port)
    {
        WSADATA wsa = {};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        wsa_ = true;
        listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_ == INVALID_SOCKET) return false;
        BOOL reuse = TRUE;
        setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        if (bind(listen_, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == SOCKET_ERROR) return false;
        if (listen(listen_, 4) == SOCKET_ERROR) return false;
        char self[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, self, MAX_PATH);
        std::string dir(self);
        const size_t slash = dir.find_last_of("\\/");
        const std::string dump = (slash == std::string::npos ? std::string(".") : dir.substr(0, slash)) + "\\dl_bulk_dump.bin";
        fopen_s(&dumpFile_, dump.c_str(), "wb");
        if (dumpFile_) std::cout << "bulk dump (first 4MB) -> " << dump << "\n";

        running_ = true;
        thread_ = std::thread([this] { accept_loop(); });
        std::cout << "usbip-display listening on 127.0.0.1:" << port << " busid=" << busId_ << "\n";
        return true;
    }

    void stop()
    {
        running_ = false;
        if (listen_ != INVALID_SOCKET) { closesocket(listen_); listen_ = INVALID_SOCKET; }
        // Unblock the urb_loop's recv on the active client so the accept thread
        // can exit and join() returns (otherwise the process lingers on close).
        {
            std::lock_guard<std::mutex> lock(clientMutex_);
            if (activeClient_ != INVALID_SOCKET) shutdown(activeClient_, SD_BOTH);
        }
        if (thread_.joinable()) thread_.join();
        if (dumpFile_) { fclose(dumpFile_); dumpFile_ = nullptr; }
        if (wsa_) { WSACleanup(); wsa_ = false; }
    }

private:
    void build_descriptors()
    {
        device_ = {
            18, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 64,
            uint8_t(kDlVendorId), uint8_t(kDlVendorId >> 8),
            uint8_t(kDlProductId), uint8_t(kDlProductId >> 8),
            0x00, 0x01, 1, 2, 3, 1};
        const uint8_t cfg[] = {
            9, 0x02, 32, 0x00, 1, 1, 0, 0x80, 0xFA,        // config (wTotalLength=32)
            9, 0x04, 0, 0, 2, 0xFF, 0x00, 0x00, 0,         // interface: vendor class, 2 endpoints
            7, 0x05, kDlBulkOutEp, 0x02, 0x00, 0x02, 0x00, // bulk OUT ep1, 512B
            7, 0x05, 0x82, 0x03, 0x08, 0x00, 0x08,         // interrupt IN ep2, 8B, bInterval 8 (hot-plug)
        };
        config_.assign(cfg, cfg + sizeof(cfg));
    }

    std::vector<uint8_t> string_descriptor(uint8_t index)
    {
        std::vector<uint8_t> d;
        const char *s = nullptr;
        if (index == 0) { return {4, 0x03, 0x09, 0x04}; }   // langid en-US
        else if (index == 1) s = "DisplayLink";
        else if (index == 2) s = "CTM Virtual Display";
        else if (index == 3) s = "CTM0001";
        else return d;
        const size_t len = std::strlen(s);
        d.push_back(uint8_t(2 + len * 2));
        d.push_back(0x03);
        for (size_t k = 0; k < len; ++k) { d.push_back(uint8_t(s[k])); d.push_back(0); }
        return d;
    }

    // Returns control IN payload (empty for OUT/ack); sets *status.
    std::vector<uint8_t> handle_control(const uint8_t *setup, const std::vector<uint8_t> &out, int32_t *status)
    {
        *status = kOk;
        const uint8_t bmType = setup[0], bReq = setup[1];
        const uint16_t wValue = setup[2] | (uint16_t(setup[3]) << 8);
        const uint16_t wIndex = setup[4] | (uint16_t(setup[5]) << 8);
        const uint16_t wLength = setup[6] | (uint16_t(setup[7]) << 8);
        const uint8_t type = (bmType >> 5) & 3;       // 0=standard 2=vendor
        (void)out;

        auto clamp = [&](std::vector<uint8_t> v) {
            if (v.size() > wLength) v.resize(wLength);
            return v;
        };

        if (type == 0) {                               // standard
            if (bReq == 0x06) {                         // GET_DESCRIPTOR
                const uint8_t descType = wValue >> 8, descIndex = wValue & 0xff;
                if (descType == 0x01) return clamp(device_);
                if (descType == 0x02) return clamp(config_);
                if (descType == 0x03) return clamp(string_descriptor(descIndex));
                if (descType == 0x5f) return clamp(vendorDesc_);  // DL vendor descriptor
                *status = kStall;
                return {};
            }
            if (bReq == 0x00) return clamp({0x00, 0x00});   // GET_STATUS: not self-powered, no remote wakeup
            if (bReq == 0x08) return clamp({0x01});         // GET_CONFIGURATION: configured value 1
            if (bReq == 0x0A) return clamp({0x00});         // GET_INTERFACE: altsetting 0
            // SET_ADDRESS/SET_CONFIG/SET_INTERFACE/CLEAR_FEATURE/SET_FEATURE: ack
            if (bReq == 0x05 || bReq == 0x09 || bReq == 0x0B || bReq == 0x01 || bReq == 0x03) return {};
            *status = kStall;
            return {};
        }
        if (type == 2) {                               // vendor
            if (bReq == 0x02 && wIndex == 0xA1) {       // EDID byte read: returns 2 bytes, [1]=edid byte
                const uint8_t idx = wValue >> 8;
                const uint8_t b = idx < edid_.size() ? edid_[idx] : 0;
                return {0x00, b};
            }
            if (bReq == 0x12) return {};                // channel-select magic: ack (OUT)
            // PROBE: vendor IN reads we don't yet decode. Answering 0 bytes when
            // the driver asked for wLength can stall its state machine; reply
            // with the requested length instead. 0x13 (wValue/wIndex=0xffff)
            // looks like a status/"monitor present" read -> try a non-zero value.
            if (bmType & 0x80) {                        // device-to-host (IN)
                if (bReq == 0x13) return clamp({0x01, 0x00, 0x00, 0x00});  // GUESS: present/ready
                return std::vector<uint8_t>(wLength, 0x00);                // zero-filled, correct length
            }
            return {};                                  // vendor OUT: ack
        }
        *status = kStall;
        return {};
    }

    bool send_ret_submit(SOCKET c, uint32_t seq, int32_t status, uint32_t actual, const std::vector<uint8_t> &payload)
    {
        std::vector<uint8_t> o;
        put32(&o, kRetSubmit);
        put32(&o, seq);
        put32(&o, 0); put32(&o, 0); put32(&o, 0);      // devid, direction, ep
        put32(&o, uint32_t(status));
        put32(&o, status == kOk ? actual : 0);
        // start_frame, number_of_packets, error_count, then 8 bytes padding:
        // the full usbip reply header is 48 bytes (12 words), not 44.
        put32(&o, 0); put32(&o, 0); put32(&o, 0); put32(&o, 0); put32(&o, 0);
        if (!payload.empty()) o.insert(o.end(), payload.begin(), payload.end());
        return send_all(c, o.data(), o.size());
    }

    bool send_ret_unlink(SOCKET c, uint32_t seq)
    {
        std::vector<uint8_t> o;
        put32(&o, kRetUnlink);
        put32(&o, seq);
        for (int k = 0; k < 10; ++k) put32(&o, 0);   // 48-byte reply header (12 words)
        return send_all(c, o.data(), o.size());
    }

    void append_device_section(std::vector<uint8_t> *o, bool withInterfaces)
    {
        const std::string path = "/ctm/usbdisplay/" + busId_;
        o->insert(o->end(), path.begin(), path.end());
        o->insert(o->end(), 256 - path.size(), 0);
        o->insert(o->end(), busId_.begin(), busId_.end());
        o->insert(o->end(), 32 - busId_.size(), 0);
        put32(o, 1); put32(o, 1); put32(o, kSpeedHigh);   // busnum, devnum, speed
        put16(o, kDlVendorId); put16(o, kDlProductId); put16(o, 0x0100); // bcdDevice
        o->push_back(0x00); o->push_back(0x00); o->push_back(0x00);      // class/sub/proto
        o->push_back(1); o->push_back(1); o->push_back(1);               // cfgval, nconfig, nintf
        if (withInterfaces) { o->push_back(0xFF); o->push_back(0x00); o->push_back(0x00); o->push_back(0x00); }
    }

    void accept_loop()
    {
        while (running_) {
            SOCKET c = accept(listen_, nullptr, nullptr);
            if (c == INVALID_SOCKET) break;
            int nd = 1;
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&nd), sizeof(nd));
            { std::lock_guard<std::mutex> lock(clientMutex_); activeClient_ = c; }
            handle_client(c);
            { std::lock_guard<std::mutex> lock(clientMutex_); activeClient_ = INVALID_SOCKET; }
            closesocket(c);
        }
    }

    void handle_client(SOCKET c)
    {
        uint8_t op[8] = {};
        if (!recv_all(c, op, 8)) return;
        if (rd16(op) != kVersion) return;
        const uint16_t code = rd16(op + 2);
        if (code == kOpReqDevlist) {
            std::vector<uint8_t> o;
            put16(&o, kVersion); put16(&o, kOpRepDevlist); put32(&o, 0); put32(&o, 1);
            append_device_section(&o, true);
            send_all(c, o.data(), o.size());
            return;
        }
        if (code == kOpReqImport) {
            uint8_t busid[32] = {};
            if (!recv_all(c, busid, 32)) return;
            const bool match = std::strncmp(reinterpret_cast<char *>(busid), busId_.c_str(), 32) == 0;
            std::vector<uint8_t> o;
            put16(&o, kVersion); put16(&o, kOpRepImport); put32(&o, match ? 0 : 1);
            if (match) append_device_section(&o, false);
            if (!send_all(c, o.data(), o.size()) || !match) return;
            std::cout << "usbip-display imported busid=" << busId_ << "\n";
            urb_loop(c);
        }
    }

    void urb_loop(SOCKET c)
    {
        while (running_) {
            uint8_t h[48] = {};
            if (!recv_all(c, h, 48)) break;
            const uint32_t command = rd32(h);
            const uint32_t seq = rd32(h + 4);
            const uint32_t direction = rd32(h + 12);
            const uint32_t ep = rd32(h + 16);
            const uint32_t xferLen = rd32(h + 24);
            if (command == kCmdUnlink) { send_ret_unlink(c, seq); continue; }
            if (command != kCmdSubmit) break;

            std::vector<uint8_t> out;
            if (direction != kDirIn && xferLen > 0 && xferLen < (64u << 20)) {
                out.resize(xferLen);
                if (!recv_all(c, out.data(), out.size())) break;
            }

            const bool isControl = (ep & 0x0f) == 0;
            int32_t status = kOk;
            uint32_t actual = 0;
            std::vector<uint8_t> payload;

            if (isControl) {
                const uint8_t *s = h + 40;
                const uint8_t bmType = s[0], bReq = s[1];
                const uint16_t wValue = s[2] | (uint16_t(s[3]) << 8);
                const uint16_t wIndex = s[4] | (uint16_t(s[5]) << 8);
                const uint16_t wLen = s[6] | (uint16_t(s[7]) << 8);
                payload = handle_control(s, out, &status);
                if (direction == kDirIn) actual = uint32_t(payload.size());
                else { actual = xferLen; payload.clear(); }
                std::cout << "ctrl bmType=0x" << std::hex << int(bmType) << " bReq=0x" << int(bReq)
                          << " wValue=0x" << wValue << " wIndex=0x" << wIndex << std::dec
                          << " wLen=" << wLen;
                if (bReq == 0x06) std::cout << " GET_DESCRIPTOR " << desc_name(wValue >> 8);
                std::cout << " -> status=" << status << " ret=" << actual << "B\n";
            } else if ((ep & 0x0f) == kDlBulkOutEp && direction != kDirIn) {
                decoder_->decode(out.data(), out.size());   // the pixel stream
                actual = xferLen;
                ++bulkUrbs_;
                bulkBytes_ += out.size();
                // Capture the first ~4MB of raw bulk payloads so the actual
                // (newer-gen) DisplayLink command format can be inspected.
                if (dumpFile_ && dumpRemaining_ > 0) {
                    const size_t take = out.size() < dumpRemaining_ ? out.size() : dumpRemaining_;
                    fwrite(out.data(), 1, take, dumpFile_);
                    dumpRemaining_ -= take;
                    if (dumpRemaining_ == 0) { fflush(dumpFile_); std::cout << "[bulk dump complete]\n"; }
                }
                const auto now = std::chrono::steady_clock::now();
                if (now - lastBulkLog_ > std::chrono::seconds(1)) {
                    int w = 0, h2 = 0;
                    uint64_t gen = 0;
                    { std::lock_guard<std::mutex> lk(fb_->mutex); w = fb_->width; h2 = fb_->height; gen = fb_->generation; }
                    std::cout << "bulk urbs=" << bulkUrbs_ << " bytes=" << (bulkBytes_ / 1024) << "KiB"
                              << " fb=" << w << "x" << h2 << " gen=" << gen << "\n";
                    lastBulkLog_ = now;
                }
            } else if ((ep & 0x0f) == 0x02 && direction == kDirIn) {
                // PROBE: hot-plug interrupt endpoint. Real DisplayLink adapters
                // report monitor connect/disconnect here. Payload format is a
                // GUESS — a small "monitor connected" status, returned on every
                // poll (the driver should dedupe repeats).
                payload = {0x01, 0x00, 0x00, 0x00};
                actual = uint32_t(payload.size());
                if (intrPolls_ < 4)
                    std::cout << "intr-in poll #" << intrPolls_ << " -> " << actual
                              << "B (GUESS: monitor connected)\n";
                ++intrPolls_;
            } else {
                actual = (direction == kDirIn) ? 0 : xferLen; // tolerate stray endpoints
                std::cout << "urb ep=0x" << std::hex << (ep & 0x0f) << std::dec
                          << " dir=" << (direction == kDirIn ? "in" : "out") << " len=" << xferLen << "\n";
            }

            if (!send_ret_submit(c, seq, status, actual, payload)) break;
        }
        std::cout << "usbip-display connection closed\n";
    }

    static const char *desc_name(uint8_t t)
    {
        switch (t) {
        case 0x01: return "device";
        case 0x02: return "config";
        case 0x03: return "string";
        case 0x5f: return "vendor(DL)";
        default: return "?";
        }
    }

    DlDecoder *decoder_;
    DlFramebuffer *fb_;
    std::string busId_;
    std::vector<uint8_t> edid_, vendorDesc_, device_, config_;
    SOCKET listen_ = INVALID_SOCKET;
    SOCKET activeClient_ = INVALID_SOCKET;
    std::mutex clientMutex_;
    std::thread thread_;
    std::atomic_bool running_{false};
    bool wsa_ = false;
    uint64_t bulkUrbs_ = 0, bulkBytes_ = 0;
    uint64_t intrPolls_ = 0;
    std::chrono::steady_clock::time_point lastBulkLog_{};
    FILE *dumpFile_ = nullptr;
    size_t dumpRemaining_ = 4u * 1024 * 1024;
};

} // namespace dlusbip
