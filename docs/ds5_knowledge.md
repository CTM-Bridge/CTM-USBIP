# DS5 Knowledge Base

## 1. USB Side

1. The virtual device is a DualSense-compatible USB composite device.
2. The profile exposes HID plus USB Audio interfaces.
3. Windows sends speaker and haptics audio as 48 kHz, 4-channel, signed 16-bit PCM over the USB ISO OUT endpoint.
4. Current channel interpretation:
   a. channels `0,1` are speaker/headset audio.
   b. channels `2,3` are haptics/audio-vibration data.
5. Current USB Audio feature units:
   a. speaker feature unit: `2`.
   b. headset feature unit: `5`.
   c. mute control: `1`.
   d. volume control: `2`.
6. Current UAC1 volume values are signed Q8.8 dB:
   a. speaker range: `-25600..0`, resolution `0x0100`.
   b. headset range: `0..12288`, resolution `122`.

## 2. Bluetooth HID Side

1. DS5 Bluetooth does not consume the USB PCM stream directly.
2. Bluetooth output reports carry controller state, audio, haptics, and CRC-protected payloads.
3. Audio/haptics payloads can be carried by HID output reports in the `0x33` through `0x39` range if the payload fits.
4. The current stream path uses report `0x36`.
5. DS5 Bluetooth HID output reports require the HID output CRC32 with seed `0xa2`.
6. Known inner block IDs:
   a. `0x90`: state/effects block.
   b. `0x91`: timing/latency block.
   c. `0x92`: haptics block.
   d. `0x93`: speaker audio/Opus block.
   e. `0x94`: split speaker/headset audio block, still unconfirmed.
   f. `0x95`: speaker + headset audio/Opus block.
   g. `0x96`: headset audio/Opus block.

## 3. Stream Report Layout

1. Outer HID report:
   a. byte `0`: report ID, currently `0x36`.
   b. byte `1`: report sequence in the high nibble.
   c. bytes `2..393`: stream blocks and zero padding.
   d. bytes `394..397`: CRC32 tail, little-endian.
2. Current report length is `398` bytes.
3. Generic inner block format:
   a. block byte `0`: block ID/header.
   b. block byte `1`: payload length.
   c. block bytes `2..N`: payload.
4. Current block order and offsets:
   a. bytes `2..66`: `0x90`, 65 bytes total.
   b. bytes `67..75`: `0x91`, 9 bytes total.
   c. bytes `76..277`: `0x95` or alternate audio block, 202 bytes total.
   d. bytes `278..343`: `0x92`, 66 bytes total.
   e. bytes `344..393`: zero padding before CRC.

## 4. `0x90` State/Effects Block

1. Block byte `0`: `0x90`.
2. Block byte `1`: `0x3f`, payload length `63`.
3. Payload bytes `0..46`: current known state/effects payload.
4. Payload bytes `47..62`: currently zero.
5. Known carried state:
   a. lightbar color.
   b. player LEDs.
   c. microphone mute LED/state.
   d. classic rumble.
   e. adaptive trigger modes and parameters.
   f. audio volume/mixer-related state.
   g. haptics/effect enable flags.
6. Confirmed `0x90` payload bytes from DSX/DriftGuard logs, webOS player code, and user tests:
   a. payload byte `0`: primary validity/effect-enable flags. For TV-side DS5 audio controls, preserve unrelated bits and set the audio allow bits that match the bytes being patched: `0x10` for headset volume, `0x20` for speaker volume, and `0x80` for audio-control byte `7`.
   b. payload byte `1`: secondary validity/effect-enable flags. Do not set this just for route/volume; its `0x80` bit belongs to AudioControl2, not the route/volume fields currently patched.
   c. payload byte `2`: classic rumble / normal vibration intensity byte. User logs confirmed game rumble ramps here, e.g. USB output `02 ff 15 06 ...`, `0a`, `1d`, `2b`, ... `40`.
   d. payload byte `3`: paired classic rumble byte, likely the other motor lane.
   e. payload byte `4`: headset volume byte.
   f. payload byte `5`: speaker volume byte.
   g. payload byte `6`: microphone volume byte.
   h. payload byte `7`: audio control bitfield. Confirmed speaker/output-path enable uses bits `0x30`.
   i. current tested volume UI range is decimal `0..100` (`0x00..0x64`) for both headset and speaker.
7. Confirmed TV-side audio route patch values inside the `0x90` payload:
   a. Off: byte `4 = 0x00`, byte `5 = 0x00`, byte `7 = 0x00`.
   b. Headset: byte `4 = headset volume`, byte `5 = 0x00`, byte `7 = 0x00`.
   c. Speaker: byte `4 = 0x00`, byte `5 = speaker volume`, byte `7 = 0x30`.
   d. Both: byte `4 = headset volume`, byte `5 = speaker volume`, byte `7 = 0x30`.
   e. Off is state only; it must not suppress or truncate outgoing audio cadence by itself.
   f. One-shot/grouped TV-side injection into the next stream `0x90` is not confirmed working. A test that patched the next outgoing `0x90` once, grouped route/headset-volume/speaker-volume, and then cleared dirty did not work in user testing on 2026-05-25. Do not treat that model as proven.
8. Probable payload groups:
   a. payload bytes `0..1`: validity/effect-enable flags.
   b. payload bytes `2..5`: rumble/audio/haptics route or volume-related fields, with `2..3` confirmed as classic rumble bytes and `4..5` confirmed as headset/speaker volume.
   c. payload bytes `6..20`: left/right adaptive trigger mode and parameter bytes.
   d. payload bytes `21..31`: motor, mic, speaker, headset, or effect option fields.
   e. payload bytes `32..46`: LED, lightbar, player LED, mute LED, and remaining effect state.
   f. payload bytes `47..62`: reserved or unused in the current layout.
9. Confirmed project behavior: preserving the USB output/effects bytes in this block keeps LEDs, triggers, rumble, and output effects working. Route/volume patching must be surgical: merge the allow flags and patch only known audio fields.
10. Unknowns:
   a. exact route flags.
   b. speaker/headset mix fields.
   c. exact haptics enable fields.
   d. exact gain curve.
   e. reserved firmware state.

### 4.1 External `SetStateData` Reference

1. Source: `https://controllers.fandom.com/wiki/Sony_DualSense`, Game Controller Collective Wiki, `SetStateData` structure.
2. The wiki names the DS5 output state payload `SetStateData` and describes the first two bytes as report-set flags: the controller only processes sections whose allow bits are set.
3. Relevant flag bits from the external reference:
   a. payload byte `0`, bit `4`: allow headphone volume.
   b. payload byte `0`, bit `5`: allow speaker volume.
   c. payload byte `0`, bit `6`: allow microphone volume.
   d. payload byte `0`, bit `7`: allow audio-control section.
   e. payload byte `1`, bit `1`: allow audio mute section.
   f. payload byte `1`, bit `7`: allow audio-control-2 section.
4. Relevant state bytes from the external reference:
   a. payload byte `2`: right/light rumble-emulation intensity.
   b. payload byte `3`: left/heavy rumble-emulation intensity.
   c. payload byte `4`: headphone volume, documented max `0x7f`.
   d. payload byte `5`: speaker volume, with PS5 noted as commonly using `0x3d..0x64`.
   e. payload byte `6`: microphone volume.
   f. payload byte `7`: audio-control bitfield.
   g. payload byte `9`: mute/power-save bitfield, including audio power save, mic mute, speaker mute, headphone mute, and haptic mute.
5. External route interpretation for payload byte `7`:
   a. bits `0..1`: microphone select.
   b. bits `4..5`: output path select.
   c. bits `6..7`: input path select.
6. The external reference also lists:
   a. USB output report `0x02` as controller state output.
   b. BT output report `0x31` as controller state output.
   c. BT output reports `0x32..0x39` as larger state/audio-capable output reports.
   d. feature report `0x80` as a test command and feature report `0x81` as test result; these are not currently classified as audio routing.

## 5. `0x91` Timing/Latency Block

1. Block byte `0`: `0x91`.
2. Block byte `1`: `0x07`, payload length `7`.
3. Payload byte `0`: `0xfe`, observed constant in the current working layout.
4. Payload bytes `1..5`: audio latency byte repeated five times.
5. Payload byte `6`: audio stream sequence, increments per packet.
6. Current default latency byte: `0x60`.
7. Useful probe pattern: keep byte `0` as `0xfe` and change latency in `0x10` steps around `0x60`.
8. Unknowns:
   a. exact meaning of `0xfe`.
   b. whether the five latency copies map to separate firmware consumers or stream lanes.

## 6. `0x92` Haptics Block

1. Block byte `0`: `0x92`.
2. Block byte `1`: `0x40`, payload length `64`.
3. Payload bytes `0..63`: signed 8-bit haptics/audio-vibration stream.
4. Current source: USB PCM channels `2,3`.
5. Current conversion: 45 kHz reservoir PCM is downsampled to 3 kHz, then converted from signed 16-bit to signed 8-bit.
6. Current payload shape: 32 stereo haptic frames, interleaved two lanes.
7. Zero payload is valid silence and should still be sent.
8. This is not the classic low-frequency rumble command path. Classic rumble rides in `0x90`.
9. Adaptive trigger force curves ride in `0x90`, not `0x92`.
10. Probable lane interpretation:
   a. even payload bytes: haptic lane 0.
   b. odd payload bytes: haptic lane 1.
   c. `0x00`: neutral/silence.
   d. `0x01..0x7f`: positive signed amplitude.
   e. `0x80..0xff`: negative signed amplitude.
11. Unknowns:
   a. exact motor/coil assignment.
   b. polarity.
   c. gain curve.
   d. firmware filtering.

## 7. `0x95` Audio/Opus Block

1. Block byte `0`: `0x95`.
2. Block byte `1`: `0xc8`, payload length `200`.
3. Payload bytes `0..199`: Opus packet, padded to 200 bytes.
4. Current source: USB PCM channels `0,1`.
5. Current Opus settings:
   a. nominal sample rate: 48 kHz.
   b. channels: 2.
   c. frame size: 480 samples.
   d. frame duration: 10 ms.
   e. bitrate: 160 kbit/s CBR.
   f. signal: music.
6. Observed Opus payloads use 200 bytes and commonly start with TOC byte `0xf4`.
7. Unknowns:
   a. whether `0x95` selects a specific route.
   b. whether it selects a quality mode.
   c. whether it selects a specific firmware mixer path.

## 8. `0x93` Alternate Audio Block

1. Block byte `0`: `0x93`.
2. Probable payload shape:
   a. byte `1`: `0xc8`.
   b. payload bytes `0..199`: Opus packet.
3. Observed as an alternate primary Opus stream block with `len=200` and TOC `0xf4`.
4. Unknowns:
   a. route selection.
   b. headset/speaker mixer mode.
   c. alternate firmware audio path.

## 9. Current Stream Model

1. USB input is 48 kHz, 4-channel PCM.
2. The current working reservoir clock is 45 kHz.
3. The builder pulls 480 frames per Bluetooth stream packet.
4. With a 45 kHz reservoir, 480 frames gives about 10.667 ms per packet.
5. Haptics keep a 15:1 relationship: 45 kHz to 3 kHz.

## 10. Open DS5 Areas

1. Exact byte meaning inside `0x90`.
2. Exact firmware meaning of the `0x91` latency copies.
3. Exact `0x93` versus `0x95` route behavior.
4. Exact haptic lane assignment and gain curve for `0x92`.
5. Payload-size limits for reports `0x33` through `0x39`.

## 11. Reverse-Engineered Audio Routing (DSX + DriftGuard cross-reference)

This section captures findings from cross-referencing DSX BT captures
(`WireSharkCaptures/dsx_*.pcapng`) with the DriftGuard webapp's JS
(`driftguard_js/driftguard_app-*.js`). Treat each entry as the current
hypothesis; bytes are confirmed only where the capture column says so.

### 11.1 Audio sub-block header at BT 0x36 byte 75

The audio sub-block header that immediately follows the `0x91` timing block
encodes the audio destination for the Opus payload that follows.

| Header | Modal meaning | Evidence | Notes |
|---|---|---|---|
| `0x90` | state/effects | every 0x36 frame begins with this | not audio routing |
| `0x91` | timing/latency | always present between state and audio | not audio routing |
| `0x92` | haptics | always emitted by us today; carries s8 PCM at ~3 kHz | independent of audio routing |
| `0x93` | **audio routing: controller speaker only** | confirmed: 1316/1316 frames in `dsx_speaker_only_audio_capture.pcapng` | Opus stereo payload, 200 B |
| `0x94` | **audio routing: speaker 1 ch + headphones 1 ch (hypothesis)** | not observed in any capture in this repo | needs a fresh DSX capture in that mode to confirm |
| `0x95` | **audio routing: speaker stereo + headset stereo (both)** | confirmed: 1755/1755 frames in `dsx_headset_speaker_audio_capture_headset_volume_changeing.pcapng` | currently our hardcoded `audio_block_id` |
| `0x96` | **audio routing: stereo headset only** | confirmed: 1684/1684 frames in `dsx_headset_only_audio_capture.pcapng` | best target for clean stereo headphone output |

DSX uses exactly one of `0x93`/`0x95`/`0x96` per BT frame and never mixes
them within a single 0x36 packet. The Opus block content (length, TOC byte
pattern) does not visibly differ between modes -- only the header changes.

### 11.2 Host-side `0xB0` routing/volume payload

DriftGuard never writes the BT 0x9X header directly. It controls audio
routing through a `0xB0` payload. On wired USB, USBPcap shows this as HID
output report `0x02` whose payload starts with `0xb0`, for example
`02 b0 ...`. On Bluetooth, DriftGuard sends the same payload shape through
its BT output helper. The firmware then chooses the BT 0x9X sub-block header
internally based on this state.

Buffer layout below is relative to the `0xB0` payload, not including the
outer USB output report byte `0x02` (length >= 0x28 / 40 bytes):

| Offset | Meaning | Notes |
|---|---|---|
| `0` | `0xB0` | payload marker / command id |
| `1` | flags (bit 7 = beamforming-related) | OR'd with `0x80` when beamforming toggle is on |
| `2..3` | unknown | |
| `4` | headphone volume | `0` when headphone path is disabled |
| `5` | speaker volume | `0` when speaker path is disabled |
| `6` | unknown | |
| `7` | mic + speaker-enable bits | `bits 0..1` = mic source (0=off, 1=internal, 2=external); `bits 4..5` (`0x30` mask) = speaker enable |
| `8..0x24` | other audio/mic/beamforming params | |
| `0x25` | beamforming sub-flag | bit `0x08` set/cleared based on `psBeamformingToggle` |

DriftGuard's routing decision table:

| User-visible mode (DriftGuard) | `routeMode` (internal) | byte[4] (hph vol) | byte[5] (spk vol) | byte[7] speaker bits (`0x30`) | Expected BT 0x9X result |
|---|---|---|---|---|---|
| Off | `'off'` | `0` | `0` | clear | route off-style `0x90` values; do not suppress cadence by default |
| Headphone only | `'head'` | `hphVol` | `0` | clear | `0x96` |
| Both | `'both'` | `hphVol` | `spkVol` | set | `0x95` |
| Speaker only | `'spk'` | `0` | `spkVol` | set | `0x93` |

The `audioPassthrough` `<select>` in DriftGuard maps:

| select value | routeMode | Note |
|---|---|---|
| `'off'` | `'off'` | |
| `'0'` or `'1'` | `'head'` | |
| `'2'` | `'both'` | |
| `'3'` | `'spk'` | |

### 11.3 Implications for CMU-USBIP

1. Our virtual DS5 currently hardcodes `audio_block_id = 0x95` in
   `maps/ds5_usb_over_ds5_bt.map`, so every BT frame requests both outputs.
   Users plugged into the 3.5 mm jack get duplicated audio on the speaker
   instead of clean stereo headphones.
2. Quick override exists today: `--audio-block 0x96` on the CLI forces the
   header for an entire session.
3. Proper plumbing should preserve/intercept the DS5 `0xB0` payload from the
   Windows side. For wired/WebHID DriftGuard this arrives as USB output report
   `0x02` with payload byte `0` equal to `0xb0`, not as USB Feature Report
   `0xB0`.
4. The `0xB0` route/volume payload should drive both:
   a. the effective `0x90` volume bytes (`payload[4]` headset, `payload[5]` speaker).
   b. the BT audio sub-block header:
      i. `byte[4] > 0 && byte[5] == 0` -> `0x96`
      ii. `byte[4] == 0 && byte[5] > 0` -> `0x93`
      iii. `byte[4] > 0 && byte[5] > 0` -> `0x95`
      iv. `byte[4] == 0 && byte[5] == 0` -> route off-style values/state.
5. The direct BT stream also carries current volume in `0x90`; `0x91` is
   latency/sequence only, and `0x93`/`0x95`/`0x96` are route headers plus Opus
   payload, not volume containers.
6. Failed TV bridge experiment on 2026-05-25: sending a dedicated BT output report `0x31` once on DS5 route/volume changes, using the USB-style `02 b0 ...` payload translated as `0x31`, tag `0x10`, `b0 80 00 00 hph spk 00 ctrl ...`, zero fill, and CRC32 seed `0xa2`, did not produce the desired behavior and was checked out from the TV worker.
7. Do not treat the dedicated one-shot `0x31` command as confirmed. Revisit only with a capture showing the exact BT wrapper and controller response.

### 11.4 Reverse-engineering gaps

1. `0x94` header value is speculation; capture needed.
2. The "auto" mode (jack-detect based) is hypothetical: DS5 BT input
   report `0x31` should expose a jack-state bit, but the exact bit
   position is not yet confirmed.
3. DriftGuard byte[7] bits `0x40`/`0x80` (above the mic/speaker mask)
   are not yet characterised.
4. Whether modes `0x93`/`0x95`/`0x96` accept different Opus encoder
   settings (lower bitrate when only speaker is active, etc.) is not yet
   tested.

### 11.5 Current General Conclusions

1. `0x90` is not an arbitrary tail. It is the DS5 state/effects payload carried inside BT output reports. It contains rumble, triggers, lights, audio controls, and volume.
2. Audio route/volume values map to confirmed state fields:
   a. preserve incoming host/game state.
   b. set only the required audio allow bits while preserving unrelated flags when mutating an existing `0x90`.
   c. payload bytes `4`, `5`, and `7` are the headset volume, speaker volume, and audio-control fields.
   d. recalculate the DS5 BT CRC after any mutation.
   e. do not assume one-shot injection into an arbitrary future stream `0x90` is enough; that exact grouped dirty test failed and was checked out.
3. Replacing payload bytes `0..1` is wrong because it clears unrelated sections. It can break rumble, triggers, LEDs, or other state. Merge flags instead.
4. Rumble is confirmed in payload bytes `2..3`; do not repurpose these bytes for audio.
5. The BT audio sub-block header still matters:
   a. `0x93`: speaker audio.
   b. `0x95`: speaker + headset audio.
   c. `0x96`: headset audio.
6. The sub-block header is not the volume container. Volume is in `0x90` payload bytes `4` and `5`.
7. Off should not drop or truncate outgoing audio reports by itself. It should apply off-style state values: payload byte `4 = 0`, byte `5 = 0`, byte `7 = 0`.
8. TV-side bridge mutation is acceptable only for explicit user knobs and must stay surgical. The Windows map/runtime remains the source of packet shape and audio cadence.
9. DriftGuard's visible `0xB0` is an output payload starting with `0xb0`, not a USB Feature Report `0xB0` in this context. It remains useful evidence, but the attempted direct TV-side `0x31` one-shot command using that payload shape failed in user testing.
10. Current tested DS5 TV UI volume range is decimal `0..100` (`0x00..0x64`) for both headset and speaker while we continue discriminating exact user-facing curves.
11. Failed experiment to remember: one-shot grouped `0x90` injection with dirty flags, route/headset/speaker grouped together, and unused volume allow bits cleared did not work. Revisit with captures/logs before reintroducing.
12. Failed experiment to remember: dedicated one-shot `0x31` command, built from the DriftGuard-style `02 b0 ...` payload, on connect and on DS5 route/headset-volume/speaker-volume changes. It was checked out from the TV worker after user testing failed.
