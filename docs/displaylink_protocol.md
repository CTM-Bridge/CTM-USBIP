# DisplayLink (DL-1x5 / "gen1") protocol notes

Clean-room reference for the `ctm-usbdisplay` spike: emulate a DisplayLink gen1
USB graphics device over USB/IP so the stock Windows DisplayLink/Synaptics
driver binds to it and treats it as a monitor, then decode the pixel stream it
pushes us. All facts below are derived from the **open** mainline Linux drivers
(`drivers/video/fbdev/udlfb.c`, `drivers/gpu/drm/udl/*`) and libdlo — no
proprietary/encrypted (DL-3xxx+) protocol is involved.

## Device identity (what the driver matches on)
- `idVendor = 0x17e9` (DisplayLink), any `idProduct`.
- One vendor-specific interface: `bInterfaceClass = 0xFF`, sub `0x00`, proto `0x00`.
- One **bulk OUT** endpoint carries the entire command stream.
- The driver fetches a **vendor descriptor** (type `0x5f`) and refuses the device
  if it is malformed: `[0]=total_len, [1]=0x5f, [2]=0x01, [3]=0x00,
  [4]=total_len-2`, then key/len/value entries. Key `0x0200` (LE) = max pixel
  area (the SKU pixel limit).

## Control requests
- **EDID readback** — `bmRequestType=0xC0` (vendor IN), `bRequest=0x02`,
  `wValue=i<<8`, `wIndex=0xA1`, `wLength=2`; returns 2 bytes, the EDID byte is
  byte **[1]**. Read one byte per request across the whole EDID.
- **Channel select** ("magic") — `bmRequestType=0x40` (vendor OUT),
  `bRequest=0x12`, 16-byte payload
  `57 CD DC A7 1C 88 5E 15 60 FE C6 97 16 3D 47 F2`. Just ACK it.
- Standard GET_DESCRIPTOR / SET_CONFIGURATION / SET_INTERFACE as usual.

## Bulk command stream
Every operation begins with `0xAF`. Lone/!repeated `0xAF` bytes are no-op padding.

- **Register write:** `AF 20 <reg> <val>`.
  - `0xFF=0x00` lock vidregs, `0xFF=0xFF` unlock (ends a mode-set).
  - `0x00` color depth. `0x1F` blank/unblank.
  - `0x20/0x21/0x22` = base address of the 16bpp framebuffer (24-bit, hi→lo).
  - Mode geometry via `set_register_16`: width at reg `0x0F`(hi)/`0x10`(lo),
    height at `0x17`(hi)/`0x18`(lo). Other regs (`0x01..0x1B`) are LFSR-encoded
    timing — not needed to reconstruct the image.
- **Pixel copy (RLX):** `AF 6B <addr24> <cmd> <raw>` then span data.
  - `addr24` = byte address into the 16bpp FB; pixel index = `(addr - base16)/2`.
  - Pixels are **RGB565, big-endian**, 2 bytes each.
  - `cmd` = total pixels this command writes (`0` means 256).
  - `raw` = literal count of the **first** span (`0` means 256).
  - Decode loop until `cmd` pixels placed: read `raw` literal BE16 pixels; if
    more remain, read a **repeat byte** R = extra copies of the last literal;
    then read the **next span's** raw-count byte; repeat. A command that ends
    exactly on a literal span has no trailing repeat byte. Inline raw-counts are
    always ≥1 (the encoder elides empty spans), so `0`→256 is unambiguous.

## What this buys / ceiling
gen1 is USB 2.0: ~1080p, 16bpp (RGB565), SDR, ~60 Hz. Modern DL chips are
encrypted and out of scope. **Open risk:** whether today's Synaptics Windows
driver still drives a gen1-class emulated device with this exact stream — the
`ctm-usbdisplay` preview window is the instrument to confirm it.
