# Real Wired DS4 USB Descriptor Baseline

Captured from Windows USB Device Tree Viewer on 2026-05-21.

## 1. Device

1. Product string: `Wireless Controller`.
2. VID/PID: `054c:09cc`.
3. USB speed: full speed.
4. Device class: per-interface.
5. Configuration total length: `0x00e1`.
6. Interfaces: `4`.

## 2. Open Pipes

1. HID interrupt IN:
   a. endpoint: `0x84`.
   b. max packet: `0x40`.
   c. interval: `0x05`.
2. HID interrupt OUT:
   a. endpoint: `0x03`.
   b. max packet: `0x40`.
   c. interval: `0x05`.
3. Speaker ISO OUT:
   a. endpoint: `0x01`.
   b. attributes: `0x09`, isochronous adaptive data.
   c. max packet: `0x0084`.
   d. interval: `0x0001`.
4. Microphone ISO IN:
   a. endpoint: `0x82`.
   b. attributes: `0x05`, isochronous asynchronous data.
   c. max packet: `0x0022`.
   d. interval: `0x0001`.

## 3. Audio Control Interface 0

1. Class: Audio Control.
2. Class-specific header:
   a. `bLength = 0x0a`.
   b. `bcdADC = 0x0100`.
   c. `wTotalLength = 0x0047`.
   d. `bInCollection = 0x02`.
   e. streaming interfaces: `0x01`, `0x02`.

## 4. Speaker Output Chain

1. Input terminal:
   a. terminal id: `0x01`.
   b. terminal type: `0x0101`, USB streaming.
   c. associated terminal: `0x06`.
   d. channels: `2`.
   e. channel config: `0x0003`, left/right.
2. Feature unit:
   a. unit id: `0x02`.
   b. source id: `0x01`.
   c. controls: master mute + volume.
3. Output terminal:
   a. terminal id: `0x03`.
   b. terminal type: `0x0402`, headset.
   c. associated terminal: `0x04`.
   d. source id: `0x02`.

## 5. Microphone Input Chain

1. Input terminal:
   a. terminal id: `0x04`.
   b. terminal type: `0x0402`, headset.
   c. associated terminal: `0x03`.
   d. channels: `1`.
2. Feature unit:
   a. unit id: `0x05`.
   b. source id: `0x04`.
   c. controls: master mute + volume.
3. Output terminal:
   a. terminal id: `0x06`.
   b. terminal type: `0x0101`, USB streaming.
   c. associated terminal: `0x01`.
   d. source id: `0x05`.

## 6. Speaker Streaming Interface 1

1. Alt 0:
   a. no endpoints.
2. Alt 1:
   a. one endpoint.
   b. terminal link: `0x01`.
   c. delay: `0x01`.
   d. format: PCM.
   e. channels: `2`.
   f. subframe size: `2`.
   g. bit resolution: `16`.
   h. sample rate: `32000`.
   i. endpoint: `0x01`.
   j. max packet: `0x0084`.
   k. interval: `0x0001`.

## 7. Microphone Streaming Interface 2

1. Alt 0:
   a. no endpoints.
2. Alt 1:
   a. one endpoint.
   b. terminal link: `0x06`.
   c. delay: `0x01`.
   d. format: PCM.
   e. channels: `1`.
   f. subframe size: `2`.
   g. bit resolution: `16`.
   h. sample rate: `16000`.
   i. endpoint: `0x82`.
   j. max packet: `0x0022`.
   k. interval: `0x0001`.

## 8. HID Interface 3

1. Class: HID.
2. Report descriptor length: `0x01fb`.
3. Interrupt IN endpoint: `0x84`, max packet `0x40`, interval `0x05`.
4. Interrupt OUT endpoint: `0x03`, max packet `0x40`, interval `0x05`.

## 9. Implications For CMU-USBIP

1. Virtual DS4 should use PID `0x09cc`, not `0x05c4`, for this baseline.
2. Virtual DS4 should expose four interfaces:
   a. audio control.
   b. speaker streaming.
   c. microphone streaming.
   d. HID.
3. Speaker ISO OUT must use max packet `0x0084`, not `0x0080`.
4. HID interface should be interface `3`.
5. HID interrupt interval should be `0x05` if mirroring real wired DS4 exactly.
