# Gulikit Android Mode Knowledge

USBView baseline from Gulikit in Android mode.

## Device

1. Windows label: `USB Input Device`
2. Product string: `GuliKit Controller A`
3. Manufacturer string: `ZhiXu`
4. VID/PID: `0x0079:0x181C`
5. Vendor: Shenzhen Longshengwei Technology, Co., Ltd.
6. `bcdUSB`: `0x0200`
7. `bcdDevice`: `0x0100`
8. Device class/subclass/protocol: `0x00 / 0x00 / 0x00`
9. USB speed: full-speed
10. `bMaxPacketSize0`: 64 bytes
11. Serial string: none

## Configuration

1. One configuration
2. `wTotalLength`: `0x0029`
3. One interface
4. `bmAttributes`: `0x80`, bus powered
5. `MaxPower`: 400 mA

## HID Interface

1. Interface `0`, alt `0`
2. Class/subclass/protocol: `0x03 / 0x00 / 0x00`
3. HID version: `0x0111`
4. Report descriptor length: `0x007C`
5. Endpoint IN `0x81`: interrupt, 64 bytes, interval `0x05`
6. Endpoint OUT `0x01`: interrupt, 64 bytes, interval `0x0A`

## Deductions

1. Android mode is normal HID, unlike XInput mode.
2. It is probably a generic DirectInput-style gamepad HID profile.
3. Polling is slower than XInput mode: IN interval `5 ms`, OUT interval `10 ms`.
4. It has no audio interface.
5. A CMU profile is easy, but a good map still needs the 124-byte HID report descriptor and actual input/output reports.
