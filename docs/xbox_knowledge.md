# Xbox Controller Knowledge

Current target is a real Xbox controller USB/GIP device, not a virtual Xbox/XInput controller.

## USBView Baseline

Captured from a real wired Xbox controller with headset jack fully inserted.

Device:

1. Product: `Controller`
2. VID/PID: `0x045E:0x0B12`
3. Device class/subclass/protocol: `0xFF / 0x47 / 0xD0`
4. USB speed: full-speed USB 2.0
5. `bMaxPacketSize0`: 64 bytes
6. `bcdDevice`: `0x0517`
7. Serial string: `3039373030313431383635333239`
8. Configuration: one config, three interfaces, `wTotalLength=0x0077`, `bmAttributes=0xA0`, `MaxPower=500mA`

Open pipes:

1. Interrupt OUT `0x02`, 64 bytes, interval `0x04`
2. Interrupt IN `0x82`, 64 bytes, interval `0x02`
3. Isochronous OUT `0x03`, 228 bytes, interval `0x01`
4. Isochronous IN `0x83`, 64 bytes, interval `0x01`

Configuration layout:

1. Interface 0 alt 0: GIP data, interrupt OUT `0x02`, interrupt IN `0x82`, both interval `0x04`
2. Interface 0 alt 1: GIP data, interrupt OUT `0x02` interval `0x04`, interrupt IN `0x82` interval `0x02`
3. Interface 1 alt 0: GIP audio disabled, zero endpoints
4. Interface 1 alt 1: GIP audio enabled, iso OUT `0x03` max `0x00E4`, iso IN `0x83` max `0x0040`, interval `0x01`
5. Interface 2 alt 0: vendor/bulk disabled, zero endpoints
6. Interface 2 alt 1: bulk OUT `0x04`, bulk IN `0x84`, 64 bytes, interval `0x00`

## Conclusions

1. This is GIP, not HID and not USB Audio Class.
2. Data endpoints are `0x02` OUT and `0x82` IN, not the public-spec example `0x01/0x81`.
3. Audio endpoints are `0x03` OUT and `0x83` IN, not the public-spec example `0x02/0x82`.
4. Audio is headset-only. Windows exposes the Xbox controller audio device only when the 3.5 mm jack is fully inserted, so headset-present state must be represented in GIP status/metadata before Windows starts useful audio.
5. Audio consumption will need GIP audio flow-control messages, not UAC-style ISO ACK timing alone.
6. The map/profile should be named around real GIP, for example `xbox_gip_usb_over_xbox_bt`, not `xinput`.

## Implementation Implications

1. Add an Xbox GIP profile matching the real descriptor above.
2. Add runtime support for non-HID GIP interrupt data events.
3. Add GIP message primitives for headers, sequence, status, metadata, gamepad input, rumble, audio control, and audio render/capture flow.
4. Use real USB captures to fill metadata/security/status bytes; the descriptor alone is not enough.
5. Start with plugged-headset mode first, because that is the state where Windows exposes audio.

## 2026-05-30 First GIP Input Pass

1. Active Xbox route is now `xbox_gip_usb.profile` plus `xbox_gip_usb_over_xbox_bt.map`.
2. The map emits 48-byte GIP input packets on `0x82` with message type `0x20`, a byte sequence counter, buttons, triggers, and sticks.
3. The Bluetooth capture's ATT value has no report ID, and local Windows BT reads confirmed `input_len=16` with no report ID prefix. The active map therefore uses `source_report=0` and `source_offset=0`.
4. Sticks and triggers are based on the capture and public GIP/xpad layouts. Button bits are a first-pass mapping from the observed Bluetooth report byte; d-pad uses the observed hat ordinal. Validate with annotated button captures.
5. OUT/status/security/audio are intentionally incomplete. This may enumerate yet still fail Steam/gamepad tests until host GIP initialization and status replies are implemented.

## 2026-05-30 Enumeration Patch

1. Keep the driver/app blind: USB/IP serves descriptors, handles standard control plumbing, and queues byte packets declared by the map. Xbox/GIP-specific bytes stay in `xbox_gip_usb.profile` and `xbox_gip_usb_over_xbox_bt.map`.
2. The profile now carries Microsoft OS 1.0 descriptor data: string index `0xEE` advertises vendor request `0x90`, and the compatible ID descriptor advertises `XGIP10` for Windows GIP binding.
3. The app has generic virtual-input trigger rules. A rule matches a control setup or endpoint OUT prefix and queues literal packets to a configured IN endpoint. The first Xbox map rule queues the initial captured `0x82` packet after `SET_CONFIGURATION`.
4. The next missing research piece is real metadata/security/status exchange. If Windows proceeds past `c0 90 ... wIndex=4`, capture/log the first endpoint OUT packets and any stalled IN phase.

## 2026-05-30 Rumble and Input Debug Pass

1. Steam now binds to the virtual Xbox device. Gamepadla remains a required target, not a secondary concern.
2. Host rumble packets observed on virtual USB endpoint `0x02` are 13 bytes beginning with `09`. The matching Bluetooth ATT write payload captured from the real controller is the trailing 8 bytes beginning with `0f`.
3. The Xbox map now maps USB rumble `09 00 .. 0f ..` to the physical 8-byte payload with `tail = none`; the app only provides the generic option to disable output CRC signing.
4. The Xbox map enables generic mapped-input logging so the next test can compare BT source bytes to emitted `0x20` GIP input bytes and correct axes/buttons for Steam and Gamepadla.

## 2026-05-30 Web/Descriptor Cross-check Patch

1. The Bluetooth input report uses the Xbox HID report layout: four little-endian 16-bit axes, two 10-bit trigger fields, a hat ordinal, then HID buttons 1..10.
2. GIP `0x20` button bytes follow the captured/known Xbox order: start/back in byte 4 bits 2/3, ABXY in byte 4 bits 4..7, d-pad in byte 5 bits 0..3, shoulders in byte 5 bits 4/5, stick clicks in byte 5 bits 6/7.
3. The previous map skipped HID button bit 2 and shifted later buttons; it also omitted Back/Start/L3/R3. The map now translates HID buttons 1..10 into the GIP `0x20` button bits.
4. The previous `SET_CONFIGURATION` virtual packet was a Bluetooth adapter/HCI/ATT notification. It has been replaced with the real wired USB/GIP status packet `03 20 13 04 83 01 00 00`, and the map has a captured `0d 20` status reply rule for matching endpoint OUT packets.
