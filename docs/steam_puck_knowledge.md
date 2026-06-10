# Steam Controller Puck (28de:1304) — composite bridge knowledge

Self-contained record of the puck-over-CTM-bridge work. Anyone (human or tool)
should be able to pick this up cold. Last updated 2026-06-04.

**CLEAN-ROOM (hard rule):** never use `hid-steam.c` or any external / kernel /
third-party source for puck protocol. This puck is ~1 month old and predates such
drivers; their bytes are unverified and likely wrong here. Every fact below is
from our own sysfs reads / on-wire logs / Wireshark captures. If a claim's only
possible basis is an external driver, reject it.

---

## 1. What the puck is (VERIFIED — on-TV sysfs dump 2026-06-03)

A Valve wireless dongle ("Steam Controller Puck", serial `FXB9954800096`) that
pairs up to four Steam Controllers. It is a **composite USB device**, genuinely
**full-speed** (`speed=12`), `bcdUSB=0x0201`, `bMaxPacketSize0=64`. Config = 235 B,
**7 interfaces**:

- Device descriptor: `12 01 01 02 ef 02 01 40 de 28 04 13 02 00 01 02 03 01`
  (class `ef/02/01` = IAD multi-function).
- IF0 cls02 CDC-ACM (control) → ttyACM0/COM. int ep `0x81` (16 B).
- IF1 cls0a CDC-data → **BULK** ep `0x82` IN / `0x01` OUT, **64 B** (`07 05 82 02 40 00`).
- IF2–5 cls03 HID slots → Controller 0–3. report **372 B**, int IN `0x83`–`0x86`
  / OUT `0x02`–`0x05`, 64 B, bInterval 2. The four slot report descriptors are IDENTICAL.
- IF6 cls03 HID mgmt/Service → report **54 B**, int IN `0x87` / OUT `0x06`, bInterval 0x20.

Map slots by **interface number** (2–5 = Controller 0–3, 6 = Service); hidraw
numbering does NOT necessarily follow interface order. IF6 has no input node → a
`/sys/class/input` scan can't see it; only a USB-device-tree walk does.

### Jail facts (webOS device-mode "prisoner")
- `/sys` has ONLY `class`, `dev`, `devices` — **no `/sys/bus`**.
- **No `/dev/bus/usb`** (no raw USB; no usbfs/usbip-host). Only `/dev/hidraw*`,
  `/dev/input/*`, sysfs metadata.
- USB host-controller dir VARIES (`xhci-hcd.2` or `vhci_hcd.0`) — never hardcode.
- **Reliable USB-dir resolution:** `realpath(/sys/class/input/<inputN>/device)`
  then walk up to `idVendor`. The `/sys/class/hidraw/<n>/device` realpath is
  **FLAKY** in this jail — do NOT use it (it was opening only the first hidraw).

---

## 2. Architecture — "TV sends all, Windows plugs all"

The TV is **BLIND**: it owns only the physical HID points (open every HID
interface, read/write raw, feature ioctls) and forwards raw bytes. It holds NO
puck protocol knowledge. The Windows bridge builds ONE composite USB device from
the puck's OWN forwarded enumeration and routes every transport per-interface.

1. **Capture** (TV, `ui_devices.c puck_enum_capture`): at plug, read the puck's
   own `descriptors` blob (device+config) + each HID interface's `report_descriptor`
   from the reliably-resolved USB dir; cache in `g_puck_enum`.
2. **Forward** (TV, `ui_bridge.c build_puck_enum_payload`): serialize into a
   `CTMB_MSG_ENUM` message, sent BEFORE `HELLO`. TV never interprets it.
3. **Build** (Windows, `backend.inl make_composite_profile_from_enum`): split the
   blob → device_descriptor[0..18] + configuration_descriptor[18..], per-interface
   report descriptors keyed by `bInterfaceNumber`. `agent.inl` `set_profile`s it
   for `kind=puck` after `start()` (enum arrives during the handshake).
4. **Enumerate**: usbccgp builds the composite child tree → Steam sees the puck +
   4 controller slots + mgmt. (Verified: Steam Settings → Controller shows it.)

`CTMB_MSG_ENUM` payload (packed): `[ctmb_enum_info_t = 32 B]{u16 descriptors_len;
u8 iface_count; u8 full_speed; u8 reserved[28]}` + descriptors blob +
`iface_count × ([ctmb_enum_iface_t = 4 B]{u8 interface_number; u8 iface_class;
u16 report_desc_len} + report_desc)`.

---

## 3. The descriptor transform — bulk 64→512 @ HIGH-speed (CORRECTED)

The CDC-data bulk endpoints are **64 B** (the puck is full-speed). 64-B bulk is
ILLEGAL at high speed (HS bulk must be 512) → Windows "Invalid Configuration
Descriptor". **We got this wrong twice first:**

- WRONG: present the device as full-speed so 64-B bulk is legal. **usbip-win2's
  3.x UDE host does NOT reliably honor a full-speed import** (tested — still
  rejected). The `full_speed` flag threaded through the enum is now vestigial.
- RIGHT (proven; matches the hand-built `steam_puck_composite.profile` that first
  enumerated): in `make_composite_profile_from_enum`, **raise every sub-512 BULK
  endpoint's wMaxPacketSize to 512** and present **HIGH-speed**. `usb_speed_for()`
  (descriptors.inl) derives the speed from the endpoints (returns HS once patched).
  We never bridge CDC bulk data (COM = TBD), so the inflated size is cosmetic. HID
  interrupt endpoints (64 B) are valid at HS and untouched.
- Also serve a 12-byte **BOS descriptor** (type 0x0f) — `bcdUSB 0x0201` makes
  Windows request it; serving it stops a retry-stall during enumeration.

This IS a small host-side descriptor edit — a deliberate, documented deviation
from the original "forward verbatim, never patch" note. It's generic USB
transport legality, not protocol interpretation, so the TV-blind / clean-room
core is intact.

---

## 4. Per-interface routing (all verbatim, all generic — no protocol)

- **INPUT** (controller → Windows): each TV reader forwards a report tagged with
  its source **IN endpoint** in the CTMB header `request_id` (primary via
  `input_thread_main`+`primary_in_ep`; siblings via `composite_reader_main`+
  `ci->in_ep`). Windows `RawInputCallback` carries the endpoint →
  `on_physical_input` delivers VERBATIM to `endpoint_address` (no map) →
  `handle_interrupt_in` returns it to the matching polled endpoint.
- **OUTPUT** (Windows → controller): interrupt-OUT to ep (e.g. `0x02`) →
  `send_output_report_ep` (endpoint in `request_id`) → TV `handle_message` routes
  to the sibling whose `out_ep` matches → `hid_write_fd_raw` (verbatim, no pacing).
- **FEATURE** (SET/GET_REPORT): interface in the **high byte** of `request_id`,
  reply-correlation in the low 24 bits (`remote_interface_feature`, bridge.inl).
  TV `feature_fd_for(request_id)` routes the `HIDIOCSFEATURE`/`HIDIOCGFEATURE` to
  the interface's hidraw and echoes `request_id` back. Bypasses the map runtime
  (the map has no rules for the puck's vendor feature reports — running them
  through it dropped them and stalled Steam's config).
- **CDC class requests** to non-HID interfaces are ACKed (device.inl) so usbser
  stops retry-storming.
- Endpoint/interface resolution on the TV uses `/sys/class/input` + VID/PID
  (`resolve_usb_device_dir`, `iface_endpoints`), never `/sys/class/hidraw` realpath.

---

## 5. The interrupt-IN head-of-line blocking bug (FOUND + FIXED)

**Symptom:** composite enumerates, `bridge input rx_hz≈270` (reports reach the
agent), but `input poll/fresh≈0.0` (Windows gets nothing). Log smoking gun after
`interrupt-IN first poll ep=0x84`: `wait=11052/22098ms`.

**Cause:** `server.inl` had ONE `interruptInWorker` draining a shared queue;
`handle_interrupt_in()` **blocks indefinitely** until its endpoint has data. An
idle composite endpoint (e.g. an unused slot `0x84`) at the head blocked the
worker for 11–22 s and starved the active slot `0x83` behind it.

**Fix (committed `9aa8000`):** one interrupt-IN worker **per endpoint**
(`EndpointInWorker` map, lazily spawned per endpoint, joined at teardown). Each
worker blocks only on its own endpoint, so idle slots can't starve the active
one. Touches the SHARED USB/IP path → re-test DS5/DS4/Xbox (degrades to one
worker per their single endpoint, so should be a no-op for them).

---

## 6. Status — verified vs open

**Working (verified from logs / Device Manager / Steam):** composite enumerates;
TV→agent input transport (~270 Hz, drops=0); per-endpoint delivery works when
Windows polls (saw `poll/fresh=18.8/14.2Hz`); output/rumble; per-interface feature
routing (resolved the `02 a3` feature storm).

**OPEN — the controller does not enter gamepad mode (protocol, clean-room RE):**
- It streams vendor reports `0x79` / `0x45`, never the gamepad input report
  `0x42` the descriptor defines. Steam loops sending output `0x81` to ep `0x02`
  and stops polling — its activation handshake never completes.
- The mgmt interface (`0x87`) shows zero traffic in logs.
- The controller is NOT detected by Steam when powered on *before* bridging (the
  connect event is missed); turning it on *after* the bridge is up is the working
  order so far.
- Hypothesis (unconfirmed): the interrupt-IN fix may unblock this — Steam was
  starved of the input responses its handshake needs. RE-TEST first.
- Ground truth for the activation sequence = `D:\Work\CMU\WireSharkCaptures` of a
  real puck on Windows (what Steam writes, what the controller replies, on which
  interface). This is the user's RE; do not infer it from external sources.

---

## 7. Do / don't (decisions)

- **DON'T implement `controller_steam_puck.c select_node`.** That was for the old
  pick-one-interface model; the composite forwards ALL interfaces. `NULL` is correct.
- **DON'T add TV-side gamepad-mode/lizard init (`on_plug_init`).** Putting puck
  protocol on the TV breaks "TV is blind" + clean-room. Activation comes from
  Steam through the bridge.
- **ENet (`--enet`) composite is a latent bug:** `bridge_enet.inl` does not store
  the forwarded enum and expects HELLO first, so composite fails over ENet. TCP
  (the default/test path) is unaffected. Fix later.
- `full_speed` flag is vestigial (superseded by the bulk-patch + `usb_speed_for`).

---

## 8. File map

Windows (`CTM-USBIP/src`):
- `backend/backend.inl` — `make_composite_profile_from_enum` (build + bulk patch),
  `RawInputCallback` endpoint, base `enum_payload`/`send_output_report_ep`/
  `remote_interface_feature` virtuals.
- `backend/bridge.inl` — store `enumPayload_` + accessor; `send_output_report_ep`;
  `remote_interface_feature` (interface in request_id high byte); INPUT endpoint carry.
- `usbip/device.inl` — `is_composite`; per-iface report descriptor by wIndex (0x21/0x22);
  BOS (0x0f); composite input bypass; composite feature SET/GET; CDC ack;
  diagnostic first-report/first-poll logs.
- `usbip/server.inl` — **per-endpoint interrupt-IN workers**; `usb_speed_for` use.
- `usb/descriptors.inl` — `usb_speed_for`. `app/agent.inl` — composite build for kind=puck.

webOS (`ctm-bridge-test-webos/src`):
- `controllers/controller_common.c` — composite open (reliable `/sys/class/input`),
  per-sibling readers, output routing by `out_ep`, `feature_fd_for` (feature by
  interface), `read_out_endpoint`/`iface_endpoints`.
- `controllers/controller_steam_puck.c` — `.composite = true` (ops).
- `app/ui_devices.c` — `puck_enum_capture`. `app/ui_bridge.c` — `build_puck_enum_payload`,
  `bridge_kind_for_item` (28de:1304→puck). `shared/ctm_bridge_protocol.h` — `CTMB_MSG_ENUM`.

## 9. Build + test
- TV IPK: `wsl.exe -d Ubuntu -- bash -lc "cd /mnt/d/Work/CMU/ctm-bridge-test-webos && REUSE_LVGL=1 ./scripts/build_ipk_macos.sh"` → `D:\Work\CMU\dist\com.local.ctmbridge_0.1.1_arm.ipk`.
- Windows: `& 'D:\Work\CMU\CTM-USBIP\build.ps1'` → `out\x64\Debug\ctm-usbip.exe` (close the running agent first, else LNK1168).
- Run agent: `& "D:\Work\CMU\CTM-USBIP\out\x64\Debug\ctm-usbip.exe" agent <port>`.
- Useful log lines: `composite profile built ... hid_ifaces=5`, `bridge input rx_hz`,
  `input poll/fresh`, `composite input first report ep=`, `interrupt-IN first poll ep=`.
