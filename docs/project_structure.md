# CMU-USBIP Project Structure

## 1. Purpose

1. `CMU-USBIP` replaces the unsigned `cmu.sys` virtual USB driver boundary with the Microsoft-signed `usbip-win2` UDE client driver.
2. The process exports a synthetic USB/IP device on `127.0.0.1:3240`.
3. `usbip.exe` attaches that exported device locally as bus id `cmu-ds5`.
4. Windows sees a virtual controller USB device from the selected profile.
5. The app translates between the virtual USB side and either:
   a. a local physical DualSense over Windows Bluetooth HID.
   b. the webOS bridge over TCP, where the TV writes to Linux hidraw/BlueZ.

## 2. Project Philosophy

1. The transport stays blind.
2. USB/IP only carries USB requests, descriptors, endpoints, and payloads.
3. Backends only read and write HID reports or bridge protocol messages.
4. DS5-specific knowledge belongs in the map/profile/docs, not in transport code.
5. The app provides primitives:
   a. copy bytes.
   b. set bytes.
   c. scale values.
   d. resample PCM.
   e. encode Opus.
   f. encode SBC.
   g. append blocks.
   h. calculate CRC.
   i. route feature/control requests.
6. The map decides how those primitives emulate the DS5.

## 3. Current Folder Layout

1. `app/`
   a. `cmu-usbip.vcxproj`: Visual Studio C++ project.
2. `docs/`
   a. `ds5_knowledge.md`: DS5 protocol and stream knowledge.
   b. `ds4_knowledge.md`: DS4 protocol and stream knowledge.
   c. `project_structure.md`: this project structure/functioning document.
   d. `worklog.md`: concise current worklog.
3. `include/cmu/`
   a. `hid.h`: HID/backend declarations.
   b. `profile.h`: profile loading data structures.
   c. `protocol.h`: shared protocol/event structures.
   d. `map/runtime.h`: map runtime public API.
4. `maps/`
   a. `ds5_usb_over_ds5_bt.map`: active DS5 USB to DS5 BT map.
   b. `ds4_usb_over_ds4_bt.map`: DS4 USB Audio/HID to DS4 BT map.
5. `profiles/descriptors/`
   a. `ds5_composite.profile`: virtual USB descriptor profile.
   b. `ds4_composite.profile`: DS4 USB Audio/HID virtual USB descriptor profile.
6. `src/`
   a. `main.cpp`: executable entry and single app translation unit.
   b. `app/`: CLI and common helpers.
   c. `audio/`: PCM reservoir.
   d. `backend/`: local BT and webOS bridge backends.
   e. `hid/`: Windows HID enumeration.
   f. `map/`: map runtime implementation.
   g. `profile/`: descriptor profile loader.
   h. `usb/`: descriptor helpers.
   i. `usbip/`: USB/IP device and server.
7. `third_party/`
   a. local third-party dependencies, currently including the vcpkg-installed Opus tree.
   b. FFmpeg runtime/import files used by the SBC map primitive.
8. `tools/`
   a. `parse_usb_capture.py`: parses a USBPcap capture of a real DS4/DS5 and prints the enumeration timeline, post-`SET_INTERFACE alt=1` window, ISO OUT pacing, HID IN/OUT activity, and every UAC1/HID class-specific control request. Requires `tshark` on PATH or at the default Wireshark install.
9. `out/`
   a. build output and copied runtime assets.
10. `build.ps1`
    a. finds Visual Studio.
    b. builds the project.
    c. copies the map/profile into the output folder.
11. `AGENTS.md`
    a. local instruction file for docs maintenance.

## 4. Build

1. Dependency:
   a. `C:\Program Files\USBip\usbip.exe`.
2. Debug build:
   a. `PowerShell -ExecutionPolicy Bypass -File "D:\Work\CMU\CMU-USBIP\build.ps1" -Configuration Debug`
3. Release build:
   a. `PowerShell -ExecutionPolicy Bypass -File "D:\Work\CMU\CMU-USBIP\build.ps1" -Configuration Release`
4. Build output:
   a. `D:\Work\CMU\CMU-USBIP\out\x64\Debug\cmu-usbip.exe`
   b. `D:\Work\CMU\CMU-USBIP\out\x64\Release\cmu-usbip.exe`
5. Runtime assets copied to output:
   a. `profiles\descriptors\ds5_composite.profile`
   b. `maps\ds5_usb_over_ds5_bt.map`
   c. `profiles\descriptors\ds4_composite.profile`
   d. `maps\ds4_usb_over_ds4_bt.map`
   e. FFmpeg DLLs needed by `codec.sbc_encode`.

## 5. Runtime Modes

1. Local BT mode:
   a. command: `.\out\x64\Debug\cmu-usbip.exe bt 0`
   b. backend: `LocalBtBackend`.
   c. physical path: Windows HID API to the local Bluetooth DualSense.
2. Bridge mode:
   a. command: `.\out\x64\Debug\cmu-usbip.exe bridge 48055`
   b. backend: `BridgeBackend`.
   c. physical path: TCP to webOS bridge, then Linux hidraw/BlueZ to the physical DualSense.
   d. input path: one TCP reader thread queues `MsgInputReport` payloads, and one input worker calls the shared physical-input callback.
3. Agent mode:
   a. command: `.\out\x64\Debug\cmu-usbip.exe agent`
   b. listens for UDP discovery and TCP control on port `48054` by default.
   c. responds to TV discovery probes so the TV app does not need a hardcoded Windows IP.
   d. accepts `USBIP_ATTACH <busid>` and runs `usbip.exe attach -r <tv-ip> -b <busid>` for TV-exported USB devices.
   e. accepts `BRIDGE_START <ds4|ds5|hid> <port> <busid>` and starts an in-process bridge session using the matching map/profile.
   f. each bridge session registers its virtual USB device into the agent's shared USB/IP server on `127.0.0.1:3240`.
   g. accepts `BRIDGE_STOP <busid>` and stops only the matching bridge session/export.
4. Common behavior:
   a. loads profile.
   b. loads map.
   c. starts USB/IP server on `127.0.0.1:3240`.
   d. exports bus id `cmu-ds5` by default, or `--busid <id>` when supplied.
   e. runs `"C:\Program Files\USBip\usbip.exe" attach -r 127.0.0.1 -b <id>` unless `--no-attach` is used.

## 5.1 DS4 Runtime Selection

1. DS4 local BT first-pass command:
   a. `.\out\x64\Debug\cmu-usbip.exe bt 0 --profile profiles\descriptors\ds4_composite.profile --map maps\ds4_usb_over_ds4_bt.map --busid cmu-ds4`
2. DS4 support currently targets:
   a. input.
   b. lightbar.
   c. classic rumble.
   d. USB speaker audio through SBC BT report `0x19`.
3. DS4 USB audio still needs physical-device validation.

## 6. Local BT Flow

1. Windows game writes to virtual DS5 USB.
2. `usbip-win2` forwards URBs to `cmu-usbip`.
3. USB/IP server decodes the request.
4. `CmuUsbDevice` decides the endpoint/control path.
5. `CmuMapRuntime` translates the data using the map.
6. `LocalBtBackend` writes the resulting BT HID report to the physical controller.
7. Physical controller input is read from Windows HID.
8. The map converts BT input report `0x31` into virtual USB input report `0x01`.
9. USB/IP completes interrupt-IN requests back to Windows.

## 7. webOS Bridge Flow

1. Windows game writes to virtual DS5 USB.
2. `usbip-win2` forwards URBs to `cmu-usbip`.
3. `cmu-usbip` runs the same map translation as local mode.
4. `BridgeBackend` sends complete BT HID reports over TCP.
5. webOS bridge receives reports.
6. webOS bridge writes exact bytes to Linux hidraw.
7. Linux BlueZ sends the report over Bluetooth.
8. Physical controller input flows back from hidraw to the bridge, then over TCP to Windows.
9. `BridgeBackend` queues each TCP input report immediately.
10. The bridge input worker translates that queued input through the shared callback path.
11. Windows completes virtual USB input through USB/IP.

## 8. Important Data Paths

1. Controller input:
   a. physical BT report `0x31`.
   b. map copies source bytes into USB report `0x01`.
   c. virtual endpoint: `0x84`.
2. Normal output effects:
   a. USB output report `0x02`.
   b. map produces BT report `0x31`.
   c. includes LEDs, rumble, triggers, and CRC.
3. Audio/haptics:
   a. USB ISO OUT endpoint `0x01`.
   b. 48 kHz, 4-channel PCM.
   c. app resamples into reservoir.
   d. map builds BT stream report `0x36`.
   e. blocks include `0x90`, `0x91`, `0x95` or `0x93`, and `0x92`.
4. Feature/control:
   a. USB feature and class requests are routed by map/runtime.
   b. UAC1 mute/volume/sample-rate controls are handled as USB Audio controls.
   c. HID feature requests are handled only when the target interface is HID.

## 9. Current Functioning Status

1. Tested by user:
   a. local audio works.
   b. local LEDs work.
   c. local inputs work.
   d. local triggers work.
2. Previously fixed:
   a. local BT writes no longer pad output reports to 547 bytes.
   b. UAC1 audio controls route through map runtime instead of being misread as HID requests.
   c. Windows HID `WriteFile` byte count reporting no longer falsely fails exact-size writes.
3. Current timing change:
   a. physical BT reads now block on overlapped HID completion.
   b. USB/IP interrupt-IN requests are queued and completed by a pending-input worker.
   c. pending interrupt-IN completes on fresh physical input instead of stale cached input.
   d. the audio writer queue waits on a condition variable without timeout polling.
   e. ISO OUT audio ACK pacing runs in a dedicated worker so the USB/IP reader loop stays hot.
   f. interrupt-IN and ISO OUT ACK workers use separate queues and condition variables.
4. Current validation state:
   a. Debug and Release builds pass.
   b. HID interrupt endpoints are still declared with `bInterval=0x01`.
   c. local polling now reaches about 500 Hz with good stability.
   d. remote polling before bridge queue work was still about 75-78 Hz average.
   e. bridge input receive is now split from callback processing with a bounded queue.
   f. remote bridge queue showed TCP input was not the bottleneck.
   g. remote ISO ACK worker test is pending.
5. Bridge input diagnostics:
   a. `bridge input rx_hz`: TCP input report receive rate.
   b. `bridge input callback_hz`: rate delivered into the shared physical-input callback.
   c. `drops`: old input reports dropped when the bounded queue is full.
   d. `depth`: current bridge input queue depth.
   e. `tcp_gap_*`: TCP input inter-arrival timing.
   f. `queue_wait_*`: time spent waiting in the bridge input queue.
   g. `callback_*`: physical-input callback duration.
6. Local BT input diagnostics:
   a. `bt input rx_hz`: Windows BT HID read completion rate.
   b. `bt input callback_hz`: rate delivered into the shared physical-input callback.
   c. `read_gap_*`: BT HID input inter-arrival timing.
   d. `callback_*`: physical-input callback duration.
   e. `read_errors`: local BT HID read failures in the interval.

## 10. Source Organization Notes

1. `src/main.cpp` currently includes several `.inl` implementation fragments.
2. This was a mechanical split from the former large source file while preserving behavior.
3. Separately compiled modules can be introduced later after behavior is stable.
4. `src/hid/enumerate.cpp`, `src/map/runtime.cpp`, and `src/profile/loader.cpp` already compile as separate `.cpp` files.

## 11. Future Integration Direction

1. Keep stabilizing CMU-USBIP first.
2. After timing and map behavior are stable, port the USB/IP server path into Sunshine.
3. Keep Moonlight/webOS bridge protocol as the physical-controller side.
4. Remove dependence on the old unsigned `cmu.sys` path.
5. Keep the same map/runtime logic so fixes made here carry forward.
