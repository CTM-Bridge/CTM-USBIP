# DS4 Knowledge Base

## 1. Confirmed Baseline

1. DualShock 4 is not DualSense.
2. It has classic rumble and lightbar output, not DS5 haptics or adaptive triggers.
3. Public DS4 Bluetooth notes describe controller speaker audio as 32 kHz.
4. Public DS4 report tables describe Bluetooth output reports `0x11..0x19` as controller state and/or audio.
5. The largest Bluetooth audio report is `0x19`, with controller output bytes followed by audio metadata, SBC payload, padding, and CRC32.
6. Real wired DS4 descriptor baseline is saved in `docs/real_ds4_usb_descriptor.md`.

## 2. Bluetooth Audio

1. Audio codec: SBC.
2. Working/probed SBC shape:
   a. sample rate: `32000`.
   b. channels: `2`.
   c. channel mode: **under probe** -- previously `dual_channel`; now testing `stereo`. With `dual_channel` the user observed speaker mono + headset L only + headset R silent, which fits the A2DP semantics of `dual_channel` (two independent mono streams; firmware's L/R router on headset never engages).
   d. blocks: `16`.
   e. subbands: `8`.
   f. allocation: `loudness` by default; `snr` is a possible probe.
   g. bitpool: `25`.
3. SBC header for the above shape starts with:
   a. syncword: `0x9c`.
   b. config byte: `0x75`.
   c. bitpool byte: `0x19`.
4. One SBC frame for that shape consumes `128` PCM frames per channel.
5. One SBC frame for that shape is `112` bytes.
6. Four SBC frames consume `512` PCM frames per channel, or `16 ms` at 32 kHz.
7. Four SBC frames produce `448` bytes of SBC payload.
8. DS4 BT audio report `0x19` is `547` bytes total:
   a. byte `0`: report ID `0x19`.
   b. byte `1`: audio header (currently `0xc0` in our map).
   c. byte `2`: audio header continuation.
   d. bytes `3..33`: 31 bytes of controller output state (matches `0x11`).
   e. bytes `34..77`: zero padding.
   f. byte `78`: zero.
   g. bytes `79..80`: 16-bit little-endian audio frame counter.
   h. bytes `81..528`: 448 bytes of SBC payload (4 SBC frames).
   i. bytes `529..542`: zero padding.
   j. bytes `543..546`: CRC32 tail seeded with `0xa2`.
9. The physical DS4 BT HID stack advertises an output report ceiling of `547` and silently rejects writes that exceed it; the map must declare `physical_report_length = 547`.

## 3. Map Primitive

1. CMU-USBIP now exposes `codec.sbc_encode`.
2. Intended DS4 map use:
   a. select two USB PCM channels into `speaker_pcm`.
   b. encode with `sample_rate=32000 channels=2 frame_samples=512 bytes=448`.
   c. use `bitpool=25 blocks=16 subbands=8 channel_mode=dual allocation=loudness`.
   d. copy `sbc_payload` into the DS4 BT audio report.
3. The app still stays blind:
   a. DS4-specific sample rate, report ID, byte offsets, frame count, header, and CRC belong in the map/profile.
   b. the primitive only converts PCM to SBC bytes.

## 4. Open DS4 Work

1. Created first-pass `ds4_usb_over_ds4_bt.map`.
2. Created first-pass `ds4_composite.profile`.
3. Added CLI/profile selection so DS4 can be loaded without changing DS5 defaults.
4. Current DS4 profile mirrors the real wired DS4 audio/HID interface layout:
   a. virtual USB audio OUT endpoint `0x01`.
   b. virtual USB audio IN endpoint `0x82`.
   c. virtual USB input report `0x01`.
   d. virtual USB output report `0x05`.
   e. virtual HID endpoints `0x84` IN and `0x03` OUT.
   f. virtual USB product string is `Wireless Controller-E`.
   g. ISO endpoints `bInterval = 0x04` so they map to 1 ms service intervals at high-speed (usbip-win2 exposes the device as HS, so the real-wired FS value `0x01` would be 125 us and the kernel will not schedule that).
5. Current DS4 map translates:
   a. BT input report `0x11` into USB report `0x01`.
   b. USB output report `0x05` into BT report `0x11`.
   c. DS4 BT output header bytes and CRC are map-owned.
6. DS4 audio map now builds BT report `0x19` from USB ISO OUT:
   a. USB speaker PCM is `32000` Hz, stereo, signed 16-bit.
   b. map encodes four SBC frames per packet.
   c. map writes the 16-bit frame counter.
   d. map signs the report with DS4 BT CRC seed `0xa2`.
7. Virtual mic IN endpoint `0x82` is exposed to match the real descriptor; CMU-USBIP currently stubs ISO IN with zero-filled packets.
8. Still open:
   a. retest whether Windows now sends speaker ISO OUT URBs after the `bInterval` change.
   b. test whether physical DS4 accepts BT report `0x19` audio with current offsets.
9. USB HID feature report `0x02` is answered from a map-owned static response to avoid enumeration stalls while the exact DS4 payload is still being characterized.

## 5. Wireshark Reference Capture

1. Source: `WireSharkCaptures/ds4_plugin_some_input_advancing_audio.pcapng`.
2. Captured against a wired DS4 (VID `054c`, PID `09cc`) with audio playing.
3. Parser: `tools/parse_usb_capture.py` (requires `tshark` on PATH or at the default Wireshark install).
4. Cached interpretation: `logs/ds4_wireshark_interpretation.txt`.
5. Key observations:
   a. Windows arms the speaker with a single `SET_INTERFACE 1 alt=1`.
   b. Windows sends **zero** UAC1 class-specific setup requests; no `SET_CUR` sampling-frequency, no mute, no volume.
   c. first ISO OUT URB lands `6.4 ms` after `alt=1`.
   d. ISO OUT URB size is constant `1280` bytes (10 frames of 32 kHz stereo PCM).
   e. ISO OUT URB rate is about `100 Hz`; byte rate is about `125.8` KiB/s.
   f. HID IN ep `0x84` runs at about `250 Hz` with `64`-byte payloads.
   g. HID OUT ep `0x03` payloads are `32` bytes (not 64).
   h. ISO IN ep `0x82` was unused in this capture.

## 5.1 BT Serial / MAC

1. DS4's `HidD_GetSerialNumberString` returns a 12-digit *decimal* unit number (firmware-stamped), not a BT MAC.
2. The authoritative identifier is the BT MAC carried by the BTHENUM grandparent device (`BTHENUM\Dev_XXXXXXXXXXXX`).
3. The enumerator now always walks ancestors via `ancestor_looks_bluetooth` so `parent_instance_id` is populated whenever a BTHENUM ancestor exists.
4. `virtual_usb_serial_from_bt_device` prefers the BTHENUM MAC over `HidD_GetSerialNumberString`. The HID serial remains a fallback.
5. On the test fixture, the chain is:
   a. `hid_serial = 581031429044` (decimal unit number, ignored when MAC is available).
   b. `instance_id = HID\{0000-1124-...}_VID&0002054C_PID&09CC\B&...` (the HID interface; not the MAC).
   c. `parent_instance_id = BTHENUM\Dev_XXXXXXXXXXXX` (this carries the MAC, used as serial).

## 5.2 BT Output Report 0x15 (current audio path)

1. Wire size: `334` bytes total.
2. Byte layout in the current map (`maps/ds4_usb_over_ds4_bt.map`):

| Offset | Value | Status | Notes |
|---|---|---|---|
| 0 | `0x15` | confirmed | report ID |
| 1 | `0xC8` | **confirmed working** | header byte. **DO NOT change.** |
| 2 | `0xA8` | **confirmed working** | header byte. The **low nibble** (`0x8`) is an audio-latency / smoothing control -- changing it reintroduces clicking artefacts. The high nibble (`0xA`) is the header proper. **Do not use this as a route selector.** |
| 3-33 | controller output state (31 B) | confirmed | LEDs/rumble/triggers, same shape as BT `0x11` |
| 34-77 | zero padding | by construction | |
| 78-79 | audio frame counter LE16, increments by `step=2` | confirmed working | |
| 80 | `0x02` | confirmed target byte | `0x02` speaker, `0x24` headset, `0x26` speaker+headset probe, `0x00` off/no target |
| 81-304 | 224 B SBC payload (2 SBC frames of 112 B each) | confirmed | |
| 305-329 | zero padding | by construction | |
| 330-333 | CRC32 (seed `0xa2`, little-endian) | confirmed | |

3. Report `0x15` also carries volume fields in the controller-state area:
   a. bytes `21..22`: headset left/right volume.
   b. byte `24`: built-in speaker volume.
4. The `0x15` report ID is likely a *speaker-oriented* member of the DS4 BT output report family `0x14..0x19`; report `0x19` is the wider variant typically associated with stereo headphone audio. This is hypothesis until a real PS4→DS4 BT capture is on hand.
5. External references cross-check the same structure: Game Controller Collective names `BTAudio.AudioTarget` as `0x02` speaker / `0x24` headset candidate, and the SensePost DS4 BT audio write-up labels report `0x15` byte `80` as audio header with `0x02` speaker / `0x24` headset.

## 6. Philosophy For DS4 Audio

1. The driver/transport is blind: usbip-win2 only relays URBs and our process only relays HID reports.
2. The map declares the desired BT shape; the app is the interpreter.
3. The ingest path must never reject a Windows URB:
   a. `process_iso_output` computes frames from `data.size() / bytesPerFrame`.
   b. it pushes whatever full frames it can construct into the reservoir.
   c. it counts trailing bytes (sub-frame remainders) for diagnostics but never errors out.
4. The reservoir absorbs rate mismatch:
   a. on overflow, it drops oldest samples to make room for new ones.
   b. it never refuses a `push`.
5. `iso_out_completion_delay_us` paces Windows so it feeds at roughly the BT drain rate; the adaptive feedback in the map closes the residual loop.
6. `expected_urb_bytes` in the map is bookkeeping; it is never used to gate Windows traffic. The internal builder produces fixed-size chunks by construction.
7. The only intentional discard is the user-selected native-app audio mode `Off`: Windows URBs are still consumed, but outgoing DS4/DS5 BT audio reports are suppressed before `hidraw` write.

## 7. Current Audio Result

1. Detailed DS4 audio notes live in `docs/ds4_audio.md`.
2. Current working path uses BT report `0x15`, 334 bytes, with a 224-byte SBC payload.
3. Current source-map header is `0x15 0xC8 0xA8`.
4. Offset `2` low nibble `8` is the useful speaker-only shape; setting mic bits causes unwanted compound BT input reports.
5. Speaker audio is implemented and usable. Remaining issue is rare clipping, likely small clock/counter drift.
