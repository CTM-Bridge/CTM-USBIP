#pragma once
// DisplayLink gen1 (DL-1x5) device emulation data: USB descriptors, EDID, and
// the bulk command-stream decoder that reconstructs the framebuffer the host's
// DisplayLink driver pushes us. See docs/displaylink_protocol.md. Clean-room
// from mainline Linux udlfb/udl + libdlo.

#include <cstdint>
#include <mutex>
#include <vector>

// ---- emulated device identity -------------------------------------------
static constexpr uint16_t kDlVendorId = 0x17e9;   // DisplayLink
static constexpr uint16_t kDlProductId = 0x0141;  // arbitrary; driver matches on vendor + iface class
static constexpr uint8_t kDlBulkOutEp = 0x01;

// The 16-byte channel-select "magic" the driver sends via vendor request 0x12.
static constexpr uint8_t kDlChannelMagic[16] = {
    0x57, 0xCD, 0xDC, 0xA7, 0x1C, 0x88, 0x5E, 0x15,
    0x60, 0xFE, 0xC6, 0x97, 0x16, 0x3D, 0x47, 0xF2};

// ---- framebuffer reconstructed from the pixel stream --------------------
struct DlFramebuffer {
    std::mutex mutex;
    int width = 0;
    int height = 0;
    uint64_t generation = 0;          // bumped on every decoded batch (paint trigger)
    std::vector<uint16_t> pixels;     // RGB565, width*height

    void resize(int w, int h)
    {
        if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return;
        if (w == width && h == height) return;
        width = w;
        height = h;
        pixels.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
    }
};

// Decoder state machine over the AF-prefixed command stream. One instance per
// device; fed each bulk-OUT URB payload via decode().
class DlDecoder {
public:
    explicit DlDecoder(DlFramebuffer *fb) : fb_(fb) {}

    void decode(const uint8_t *buf, size_t n)
    {
        size_t i = 0;
        bool wrote = false;
        while (i < n) {
            if (buf[i] != 0xAF) { ++i; continue; }   // resync to next command
            ++i;
            if (i >= n) break;
            const uint8_t op = buf[i++];
            if (op == 0xAF) { continue; }             // AF AF... = no-op padding
            if (op == 0x20) {                         // register write: reg, val
                if (i + 2 > n) break;
                set_register(buf[i], buf[i + 1]);
                i += 2;
            } else if (op == 0x6B) {                  // RLX pixel copy
                if (!decode_rlx(buf, n, i)) break;
                wrote = true;
            }
            // unknown ops: fall through and resync on the next 0xAF
        }
        if (wrote) {
            std::lock_guard<std::mutex> lock(fb_->mutex);
            ++fb_->generation;
        }
    }

private:
    void set_register(uint8_t reg, uint8_t val)
    {
        regs_[reg] = val;
        if (reg == 0x20) base16_ = (base16_ & 0x00FFFF) | (uint32_t(val) << 16);
        else if (reg == 0x21) base16_ = (base16_ & 0xFF00FF) | (uint32_t(val) << 8);
        else if (reg == 0x22) base16_ = (base16_ & 0xFFFF00) | uint32_t(val);
        else if (reg == 0xFF && val == 0xFF) {        // vidreg unlock = end of mode-set
            const int w = (int(regs_[0x0F]) << 8) | regs_[0x10];
            const int h = (int(regs_[0x17]) << 8) | regs_[0x18];
            if (w > 0 && h > 0) {
                std::lock_guard<std::mutex> lock(fb_->mutex);
                fb_->resize(w, h);
            }
        }
    }

    // Parse one AF 6B command starting at index i (already past AF 6B). Advances
    // i past the command. Returns false on truncation (caller stops).
    bool decode_rlx(const uint8_t *buf, size_t n, size_t &i)
    {
        if (i + 5 > n) return false;
        const uint32_t addr = (uint32_t(buf[i]) << 16) | (uint32_t(buf[i + 1]) << 8) | buf[i + 2];
        i += 3;
        int cmd = buf[i++]; if (cmd == 0) cmd = 256;
        int raw = buf[i++]; if (raw == 0) raw = 256;

        std::lock_guard<std::mutex> lock(fb_->mutex);
        const size_t total = fb_->pixels.size();
        size_t pix = (addr >= base16_) ? (addr - base16_) / 2 : 0;

        int placed = 0;
        uint16_t last = 0;
        while (placed < cmd) {
            for (int k = 0; k < raw && placed < cmd; ++k) {
                if (i + 2 > n) return false;
                last = (uint16_t(buf[i]) << 8) | buf[i + 1];   // RGB565 big-endian
                i += 2;
                if (pix + placed < total) fb_->pixels[pix + placed] = last;
                ++placed;
            }
            if (placed >= cmd) break;
            if (i >= n) return false;
            int repeat = buf[i++];                              // extra copies of last literal
            for (int k = 0; k < repeat && placed < cmd; ++k) {
                if (pix + placed < total) fb_->pixels[pix + placed] = last;
                ++placed;
            }
            if (placed >= cmd) break;
            if (i >= n) return false;
            raw = buf[i++]; if (raw == 0) raw = 256;            // next span's literal count
        }
        return true;
    }

    DlFramebuffer *fb_;
    uint8_t regs_[256] = {};
    uint32_t base16_ = 0;
};

// ---- EDID --------------------------------------------------------------
// Exact copy of the host's active display (LG HDR 4K, GSM7707): 128-byte base
// block + one CTA-861 extension (4K modes, HDR, audio). Captured verbatim from
// the registry so the virtual adapter advertises a real, working monitor.
inline std::vector<uint8_t> dl_build_edid()
{
    static const uint8_t kEdid[256] = {
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x1e, 0x6d, 0x07, 0x77, 0xcc, 0x20, 0x03, 0x00,
        0x0b, 0x21, 0x01, 0x04, 0xb5, 0x3c, 0x22, 0x78, 0x9f, 0x3e, 0x31, 0xae, 0x50, 0x47, 0xac, 0x27,
        0x0c, 0x50, 0x54, 0x21, 0x08, 0x00, 0x71, 0x40, 0x81, 0x80, 0x81, 0xc0, 0xa9, 0xc0, 0xd1, 0xc0,
        0x81, 0x00, 0x01, 0x01, 0x01, 0x01, 0x4d, 0xd0, 0x00, 0xa0, 0xf0, 0x70, 0x3e, 0x80, 0x30, 0x20,
        0x65, 0x0c, 0x58, 0x54, 0x21, 0x00, 0x00, 0x1a, 0x28, 0x68, 0x00, 0xa0, 0xf0, 0x70, 0x3e, 0x80,
        0x08, 0x90, 0x65, 0x0c, 0x58, 0x54, 0x21, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x28,
        0x3d, 0x87, 0x87, 0x38, 0x01, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc,
        0x00, 0x4c, 0x47, 0x20, 0x48, 0x44, 0x52, 0x20, 0x34, 0x4b, 0x0a, 0x20, 0x20, 0x20, 0x01, 0x16,
        0x02, 0x03, 0x1c, 0x71, 0x44, 0x90, 0x04, 0x03, 0x01, 0x23, 0x09, 0x07, 0x07, 0x83, 0x01, 0x00,
        0x00, 0xe3, 0x05, 0xc0, 0x00, 0xe6, 0x06, 0x05, 0x01, 0x52, 0x48, 0x5d, 0x02, 0x3a, 0x80, 0x18,
        0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c, 0x45, 0x00, 0x58, 0x54, 0x21, 0x00, 0x00, 0x1e, 0x56, 0x5e,
        0x00, 0xa0, 0xa0, 0xa0, 0x29, 0x50, 0x30, 0x20, 0x35, 0x00, 0x58, 0x54, 0x21, 0x00, 0x00, 0x1a,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2c,
    };
    return std::vector<uint8_t>(kEdid, kEdid + sizeof(kEdid));
}

// ---- vendor descriptor (type 0x5f) the driver validates ------------------
inline std::vector<uint8_t> dl_build_vendor_descriptor()
{
    // header (5) + one key entry: 0x0200 max_area (3 + 4)
    const uint32_t maxArea = 1920u * 1200u;
    std::vector<uint8_t> d;
    d.push_back(12);          // [0] total length
    d.push_back(0x5f);        // [1] vendor descriptor type
    d.push_back(0x01);        // [2] version lo
    d.push_back(0x00);        // [3] version hi
    d.push_back(12 - 2);      // [4] length after type
    d.push_back(0x00); d.push_back(0x02); d.push_back(0x04);          // key 0x0200, len 4
    d.push_back(uint8_t(maxArea)); d.push_back(uint8_t(maxArea >> 8));
    d.push_back(uint8_t(maxArea >> 16)); d.push_back(uint8_t(maxArea >> 24));
    return d;
}

// RGB565 (host order) -> 0x00RRGGBB for the preview / PNG.
inline uint32_t dl_565_to_rgb888(uint16_t p)
{
    const uint32_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
    return ((r * 255 + 15) / 31 << 16) | ((g * 255 + 31) / 63 << 8) | ((b * 255 + 15) / 31);
}
