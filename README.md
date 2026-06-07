# CTM-USBIP

![platform](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-0078D6?logo=windows&logoColor=white)
![language](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)
![transport](https://img.shields.io/badge/transport-USB%2FIP-2ea44f)

> Turn a controller attached to **another machine** into a **real USB device on Windows**, over the network, using USB/IP.

**CTM** = *Controller Translation Mapper*. CTM-USBIP is the Windows half of the bridge: it receives a controller's raw HID transport forwarded from the **ctm-bridge-webos** TV app (companion project) and re-creates it as a virtual USB device through the [usbip-win2] virtual host-controller driver — so Windows, Steam, and games see a genuine gamepad (input, rumble, LEDs, and audio where supported).

The original use case: you stream a PC game to an LG TV (Moonlight/Sunshine) and your controller is connected to the **TV**, not the PC. CTM-USBIP makes that controller appear on the PC.

---

## 1. What it does

- **Virtual USB device over USB/IP.** Hosts a local USB/IP server and presents the controller to Windows through usbip-win2's WHQL-signed virtual host controller — no custom kernel driver of our own.
- **Map-driven translation.** Each controller is described by two data files: a descriptor `.profile` (the USB device it should look like) and a `.map` (how to translate the source transport into it). Adding a controller is data, not code.
- **Composite devices.** Rebuilds multi-interface devices (e.g. the Steam Controller Puck dongle) from their *own* forwarded USB enumeration, routing each interface independently.
- **Supported today:** DualSense (DS5), DualShock 4 (DS4), Xbox (GIP), Steam Controller Puck, and a generic HID pass-through.

```
[controller] --BT/USB--> [ctm-bridge-webos]  --TCP/ENet-->  [CTM-USBIP agent]  --USB/IP-->  [usbip-win2 vhci]  -->  Windows / Steam / games
 (paired to TV)           reads hidraw,                       rebuilds the USB                WHQL virtual host
                          forwards raw HID                    device + maps reports           controller
```

The TV stays "blind" — it only moves raw HID bytes. All controller knowledge lives here, in the profiles and maps.

---

## 2. How to build

Requirements:
- Windows 10/11 (x64)
- **Visual Studio 2022** with the *Desktop development with C++* workload

```powershell
.\build.ps1                       # Debug x64 (default)
.\build.ps1 -Configuration Release
```

Output: `out\x64\<Config>\ctm-usbip.exe`, with `profiles\`, `maps\`, and the runtime DLLs copied next to it.

---

## 3. How to use it with USB/IP

CTM-USBIP needs the [usbip-win2] USB/IP client and its **WHQL-signed virtual host-controller driver** installed. CTM-USBIP runs its own USB/IP *server* for the virtual device and then calls `usbip.exe` to *attach* it locally — you don't attach anything by hand.

**Step 1 — install the usbip-win2 driver**

Download and install a usbip-win2 release that includes the WHQL/WHLK-certified driver:

➡ **https://github.com/vadimgrn/usbip-win2** — releases: **https://github.com/vadimgrn/usbip-win2/releases**

The MSI installs `usbip.exe` and the driver to `C:\Program Files\USBip\` (the path CTM-USBIP expects). Load the driver per the usbip-win2 instructions.

**Step 2 — run the CTM-USBIP agent**

```powershell
.\out\x64\Debug\ctm-usbip.exe agent 48054
```

The agent listens for the TV app on the control port (`48054`), runs a local USB/IP server on `127.0.0.1:3240`, and automatically `usbip attach`es the virtual device whenever a controller is plugged in on the TV.

**Step 3 — plug a controller on the TV**

Start the **ctm-bridge-webos** app on the TV, point it at your PC's IP and the agent port, and connect a controller. It appears on Windows in Device Manager and as a gamepad in Steam / games.

> Set `CTM_USBIP_VERBOSE=1` before launching the agent for detailed per-endpoint logging.

<details>
<summary>Standalone / local modes</summary>

`ctm-usbip.exe` can also run without the TV bridge for testing — e.g. against a local Bluetooth backend, or with `--profile` / `--map` overrides. See `src/app/`.
</details>

---

## Project layout

| Path | What |
|------|------|
| `src/usbip/` | USB/IP server + virtual USB device |
| `src/backend/` | transports (TCP bridge, ENet/UDP, local Bluetooth) |
| `src/map/` | map runtime (report translation) |
| `profiles/descriptors/` | per-controller USB descriptor profiles |
| `maps/` | per-controller translation maps |
| `include/`, `app/` | headers and the MSBuild project |
| `docs/` | design notes + worklog |

## Status

Experimental / research. DS5/DS4/Xbox input + rumble work. The Steam Controller Puck composite enumerates and the input transport is in place; controller gamepad-mode activation is still WIP — see `docs/steam_puck_knowledge.md`.

## Clean-room

All controller protocol in this project is derived from its own observation (sysfs reads, on-wire/Wireshark captures) — **not** from third-party or kernel driver sources.

## Companion project

**ctm-bridge-webos** — the LG webOS TV app that reads the controller and forwards it here.

## Acknowledgements

- [usbip-win2] by **vadimgrn** — the USB/IP client for Windows and its signed virtual host-controller driver, which CTM-USBIP builds on.

[usbip-win2]: https://github.com/vadimgrn/usbip-win2
