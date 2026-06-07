# Xbox GIP USB init handshake (from fresh capture)

Source: `WireSharkCaptures/xbox_over_usb_plug_unplug_plug.pcapng`, decoded via tshark.
This is the exchange the CACHED capture was missing. Windows (`xboxgip`) requires it
to promote the GIP device to an XInput gamepad (what Gamepadla / hardwaretester need).
Without it xboxgip binds the USB device but never creates the gamepad child.

Endpoints: device->host = interrupt IN 0x82, host->dev = interrupt OUT 0x02.
GIP header (single packet): [cmd, flags, seq, len, ...payload]. Chunked messages set
the chunk bit in flags (0xa0/0xb0/0xf0...) and the host ACKs chunks with cmd 0x01.

## Ordered sequence (first plug, frames 119-441)

```
DEV->HOST 02 20 02 1c 7eed89baf045 0000 5e04 120b 0500 1700 0600 0000 0804 0100 0100 0100   # ANNOUNCE: VID 045E, PID 0B12
HOST->DEV 04 20 01 00                                                                          # host: GET DESCRIPTOR
DEV->HOST 04 f0 03 3a a302 1000 ...                                                            # DESCRIPTOR chunk (0xf0 = first, carries total len)
DEV->HOST 04 a0 03 ba 00 3a 06 0a 0c 0d 1e 01 1a 00 "Windows.Xbox.Input.Gamepad" <iface GUID> # class name + interface GUID
DEV->HOST 04 a0 03 ba 00 74 ...                                                               # descriptor chunks (0xa0 = middle)
DEV->HOST 04 a0 03 3a ae ...
DEV->HOST 04 a0 03 3a e8 ...
DEV->HOST 04 b0 03 01 a2 0200                                                                  # descriptor chunk (0xb0 = last)
HOST->DEV 01 20 03 09 0004 20.. ....                                                           # host ACKs (cmd 0x01) interleaved
DEV->HOST 04 a0 03 00 a3 02
HOST->DEV 05 20 02 0f 06 00.. 005553 ......                                                    # host: set device state (locale "US")
HOST->DEV 05 20 03 01 00
HOST->DEV 0a 20 04 03 000114                                                                   # host: LED
HOST->DEV 06 20 01 02 0100                                                                     # host: cmd 0x06 (small, NOT a crypto challenge)
HOST->DEV 0d 20 05 05 00 0d5555e9                                                              # host: latency probe (token varies)
DEV->HOST 0d 20 04 0d 00 0d5555e9 d81a1800 151b1800                                            # device: echo token + 2 timestamps
HOST->DEV 1e 30 06 01 00                                                                       # host: GET metadata (cmd 0x1e, chunked)
DEV->HOST 1e .. metadata chunks (frames 233-291, ~10 chunks, large)                            # device: capability/metadata descriptor
DEV->HOST 1e 30 07 10 04 00 "3039373030313431383635333239"                                     # device: serial string
DEV->HOST 03 20 08 04 83010000                                                                 # device: status (we already send this one)
DEV->HOST 20 .. input reports start
```

## Key facts for implementation
- ANNOUNCE (0x02) must be sent first; host won't ask for the descriptor until it sees it.
- DESCRIPTOR (0x04) and METADATA (0x1e) are CHUNKED across 64-byte packets, host ACKs
  each with cmd 0x01. Sequence byte increments per message.
- 0x06 is tiny (2 data bytes) -> no large security/crypto challenge -> likely replayable.
- The descriptor/metadata payloads are static for this controller model -> capture-replay.
- 0x0d host token varies per session and must be ECHOED (comes after gamepad creation,
  not required for it).

## Raw chunk bytes
Full hex per frame is in the pcapng; re-extract with:
  tshark -r <file> -Y "usb.transfer_type==0x01 && usb.capdata && (usb.endpoint_address==0x82 || usb.endpoint_address==0x02) && frame.number>110 && frame.number<470" -T fields -e frame.number -e usb.endpoint_address -e usb.capdata
