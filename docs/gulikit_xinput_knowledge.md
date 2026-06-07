# Gulikit XInput Knowledge

USBView baseline from Gulikit in Xbox 360 controller mode.

## Device

1. Windows label: `Xbox 360 Controller for Windows`
2. Product string: `Gulikit X `
3. Manufacturer string: `Guli`
4. VID/PID: `0x045E:0x028E`
5. `bcdUSB`: `0x0200`
6. `bcdDevice`: `0x0110`
7. Device class/subclass/protocol: `0xFF / 0xFF / 0xFF`
8. USB speed: full-speed
9. `bMaxPacketSize0`: 64 bytes
10. Serial string: none

## Configuration

1. One configuration
2. `wTotalLength`: `0x0031`
3. One interface
4. `bmAttributes`: `0x80`, bus powered
5. `MaxPower`: 100 mA

## Interface

1. Interface `0`, alt `0`
2. Class/subclass/protocol: `0xFF / 0x5D / 0x01`
3. Endpoint IN `0x81`: interrupt, 32 bytes, interval `0x01`
4. Endpoint OUT `0x02`: interrupt, 32 bytes, interval `0x08`

## Notes

1. This is Xbox 360/XUSB-compatible, not HID gamepad.
2. The odd `HID Descriptor` block shown by USBView is likely USBView mis-decoding Xbox 360 class-specific descriptors as HID.
3. This profile is useful as a baseline for virtual Xbox 360/XInput compatibility.
4. It has no audio interface.
5. It should be easier than real Xbox GIP because Windows already binds it as `Xbox 360 Controller for Windows`.
