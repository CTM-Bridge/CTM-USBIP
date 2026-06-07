# DS4 Audio

## Status

1. DS4 speaker audio over BT is implemented and user-tested.
2. Audio is not mathematically perfect yet, but it is usable:
   a. latest steady-state `audio_resampled_hz` average: `32013.86`.
   b. latest steady-state `audio_chunks_hz` average: `125.07`.
   c. latest steady-state `bt_audio` drops: `1` over the sampled window.
   d. observed issue: rare clipping roughly every 20+ seconds.

## USB Side

1. Virtual DS4 exposes UAC1 speaker OUT endpoint `0x01`.
2. Windows feeds stereo signed 16-bit PCM at nominal `32000 Hz`.
3. Wired DS4 reference capture showed `1280`-byte ISO OUT URBs at about `100 Hz`.
4. CMU-USBIP derives ISO OUT completion timing from submitted byte count and applies the map scalar:
   `iso_out_completion_delay_scale = 0.9995`.
5. The ingest path is size-agnostic:
   a. it computes frames from `data.size() / bytesPerFrame`.
   b. it pushes complete frames into the intermediate reservoir.
   c. it never rejects Windows URBs because of expected size.

## Reservoir And Builder

1. Current DS4 map keeps the reservoir at `32000 Hz`.
2. Current frame size is `256` PCM frames per channel.
3. At `32000 Hz`, `256` frames is `8 ms`, so the expected chunk cadence is `125 Hz`.
4. Current builder is unpaced:
   `audio_builder_pace_ms = 0.0`.
5. Current BT writer is fixed at:
   `bt_pace_ms = 5.0`,
   `pace_min_ms = 5.0`,
   `pace_max_ms = 5.0`.

## SBC Shape

1. Codec primitive: `codec.sbc_encode`.
2. Current map settings:
   a. `sample_rate = 32000`.
   b. `channels = 2`.
   c. `frame_samples = 256`.
   d. `bytes = 224`.
   e. `bitpool = 25`.
   f. `blocks = 16`.
   g. `subbands = 8`.
   h. `channel_mode = dual`.
   i. `allocation = loudness`.

## BT Report Shape

1. DS4 BT audio report ID mainly encodes packet size. It does not appear to
   define a different audio mode by itself, as long as the full packet fits.
2. Known candidate lengths:
   a. `0x14`: `270` bytes.
   b. `0x15`: `334` bytes.
   c. `0x16`: `398` bytes.
   d. `0x17`: `462` bytes.
   e. `0x18`: `526` bytes.
3. Current packet layout needs `224` SBC bytes plus report header, controller
   output state, frame counter, target byte, padding, and CRC. `0x15` is the
   smallest practical report that fits this layout. Larger reports should be
   equivalent except for extra padding.
4. Current test report ID:
   `0x14`.
5. Current test report length:
   `270`.
6. Current source-map header:
   a. offset `0`: `0x15`.
   b. offset `1`: `0xC8`.
   c. offset `2`: `0xA8`.
7. Current payload layout:
   a. offsets `3..33`: copied controller output/effects payload from USB report `0x05`.
   b. offsets `78..79`: little-endian audio frame counter.
   c. offset `80`: audio target `0x02`.
   d. offsets `81..304`: `224` bytes SBC payload.
   e. last 4 bytes: CRC32 with DS4 BT seed `0xA2`.
8. Current frame counter:
   `audio_frame_counter step = 2`.

## Header Findings

1. Offset `1` appears to carry BT report flags and cadence bits.
2. Offset `2` affects audio/mic mode more strongly than offset `1`.
3. Empirical findings:
   a. values with low nibble `8` on offset `2` preserve speaker audio and avoid mic-driven unwanted reports.
   b. high nibble `2` on offset `2` selects headset-only stereo.
   c. values that set low mic bits, such as `0x99`, cause unwanted compound BT reports.
   d. `0xA8` produced speaker audio without enabling mic input.
   e. `0xAA` caused the controller to emit mostly report `0x14`, consistent with enabling mic-related mode.
4. Keep mic bits off unless mic input is explicitly being implemented.
5. TV-side route buttons must not vary offset `2`; route selection belongs at
   offset `80`:
   a. `0x00`: off / no audio target.
   b. `0x02`: speaker.
   c. `0x24`: headset.
   d. `0x26`: speaker + headset probe.
6. DS4 volume bytes in report `0x15`:
   a. offsets `21..22`: headset left/right volume.
   b. offset `24`: built-in speaker volume.

## Input Interaction

1. DS4 BT reports `0x11..0x19` can be compound controller/audio reports.
2. They must not be blindly forwarded to virtual USB input.
3. Reports with HID disabled should be ignored for controller input.
4. Current safe policy:
   a. keep speaker-only audio mode.
   b. map normal controller input only from known HID-bearing reports.
   c. defer mic input and compound report parsing.

## Open Tuning

1. Rare clipping remains; likely causes:
   a. small feed-rate drift around `32000 Hz`.
   b. frame counter step not matching the selected BT report mode perfectly.
   c. DS4 internal speaker clock not exactly `32000 Hz`.
2. Do not tune by enabling mic bits; that changes report families and can create Windows shortcut/mouse side effects.
3. Next low-risk knobs:
   a. tiny changes to `iso_out_completion_delay_scale`.
   b. verify `audio_frame_counter step = 1` versus `2`.
   c. keep offset `2` ending in `8`.
