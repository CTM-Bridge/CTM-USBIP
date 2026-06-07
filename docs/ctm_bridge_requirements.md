# CTM Bridge — Functional Requirements

**Project:** CTM Bridge (Controller Translation Mapper Bridge)
**Author:** Ciprian-Teodor Misaila
**Date:** 2026-05-26
**Status:** Draft — functional spec; architecture deferred.

## 1. Purpose

Let a paired TV-side controller (DS4, DS5, Steam Puck, eventually Xbox /
generic xpad) act as if it were plugged into the host PC via USB during a
Sunshine ↔ Moonlight game-streaming session. Audio, rumble, LEDs, and
input must round-trip transparently.

Replaces the abandoned custom-Windows-driver approach (driver-signing
cost). Uses stock **USBIP** instead — same code path ports to Linux.

## 2. Components

| Component | Where it runs | Responsibility |
|---|---|---|
| `ctm-usbip` | Host (Windows now, Linux later) | USBIP virtual-controller server; declares USB descriptor to OS, exposes per-controller virtual USB devices. |
| `ctm-bridge` | TV (webOS) | Reads `/dev/hidraw*` for paired controllers, applies map, forwards reports over TCP to `ctm-usbip`. Spawned by Moonlight on stream start. |
| Sunshine integration | Host | Config flag + Vue checkbox. When enabled and Moonlight requests CTM, spawns `ctm-usbip` for the session. Capability negotiation. |
| Moonlight integration | TV | Config flag + LVGL settings. When enabled, spawns `ctm-bridge`, opens overlay slot for per-controller controls. |
| `.map` files | Both sides | Declare controller-specific byte layouts, capabilities, modes. |

## 3. Core principles

1. **CTM is opt-in, additive, fail-safe.** If anything fails (binary missing, no controllers, host doesn't support CTM, controller not supported, etc.), behavior degrades silently to standard Moonlight (SDL → ViGEm/uinput).
2. **Game is the source of truth by default.** No interception of game-issued USB writes unless the user explicitly toggles override (audio redirect, volume, etc.).
3. **Driver/transport blind.** All controller-specific knowledge lives in `.map` files. The runtime (Windows + TV side) executes generic byte ops declared in maps.
4. **Map-driven UI.** Overlay reads each map's `[advertise]` section to know which controls to render. New controller = new map + profile, no UI code change.
5. **No pairing security between TV and host.** The TV pairings (BT controllers) are already trusted by the user; the LAN path between TV and host is trusted via the existing Moonlight pin/cert pairing.

## 4. Controller lifecycle

### 4.1 Pairing
- Controllers are paired to the **TV** via webOS native settings before Moonlight is launched. Moonlight does not implement its own pairing flow.
- Moonlight on TV polls `/dev/hidraw*` every **2 seconds** when CTM mode is active to discover newly paired controllers.

### 4.2 Stream start
- User starts streaming with CTM mode on.
- If `ctm-bridge` binary missing or fails to start → fall back to standard Moonlight (log warning, no overlay error popup).
- Moonlight asks Sunshine over the control channel: "do you support CTM?".
- If Sunshine doesn't support CTM (older build) → fall back to standard Moonlight.
- If both sides agree → Sunshine spawns `ctm-usbip` with per-controller profile/map, Moonlight spawns `ctm-bridge` configured for paired controllers.

### 4.3 Hot-plug mid-stream
- New controller paired during the stream → auto-attach as next free slot. No user prompt.

### 4.4 Disconnect mid-stream
- Controller drops BT → bridge informs `ctm-usbip` → Windows sees USB-disconnect for that slot. No fallback, no auto-reconnect attempt by CTM itself (BT stack handles reconnect; CTM picks it back up on next 2 s scan).

### 4.5 Stream end
- Moonlight stops the stream → kills `ctm-bridge`. Sunshine session teardown kills `ctm-usbip`.
- TV releases the controller (returns to whatever owned it before, typically webOS input subsystem).

### 4.6 Slot ordering
- First paired = slot 0, second = slot 1, etc., up to **4 BT controllers** plus the Steam Puck (composite, surfaces as 4 entries, total cap separate).
- No user-facing reorder. Plug/unplug on host is transparent.

## 5. Controller-OS interaction during stream

- **Controller input → TV OS** is intercepted (blocked) while the stream is running. Controller inputs go *only* to the host game.
- **Remote control input → TV OS** stays active so the user can long-press EXIT to open the overlay (existing behavior).

## 6. Overlay UI (in-stream)

User opens the Moonlight overlay (long-press EXIT on webOS remote) and sees the existing stats/disconnect/quit panel. **New top-left panel: connected controllers.**

### 6.1 Layout
- Top-left of the overlay.
- One row per connected controller (compact card: name + battery icon).
- Clicking a row expands it; expanding another auto-collapses the previously expanded one (single expansion at a time).
- Scales to up to 5 entries (4 BT + Steam Puck) without overflowing the overlay.

### 6.2 Per-controller card content (when collapsed)
- Controller name (e.g. "DualSense", "DualShock 4", "Steam Controller")
- Slot index
- Battery icon (filled, unknown icon if unsupported, "?" if poll pending)
- Connection state indicator

### 6.3 Per-controller card content (when expanded)
- Header (same as collapsed)
- **Settings driven by the map's `[advertise]` `overlay_settings` list.** Each supported setting renders as a row.
- For DS5: latency (ms slider), audio_redirect (toggle + sub-options), volume_speaker (slider), volume_headphone (slider).
- For DS4: audio_redirect (toggle), volume_speaker (slider), volume_headphone (slider).
- For Steam Puck: serial + model + slot info only, no controls.

### 6.4 Battery polling
- Every **2 minutes** per controller. Displays last known value.
- Unknown icon when controller doesn't report battery (Steam Puck, generic xpads).
- "?" / loading state during the first ~100 ms after connect.

## 7. Audio redirect

### 7.1 Definition
"Redirect SDL audio → controller" diverts the **game audio stream from the host** (currently played via Moonlight → TV speakers) toward the controller's speaker / 3.5 mm headphone jack.

### 7.2 Modes
- **Exclusive**: audio plays only on the controller. TV may be muted automatically (see toggle below).
- **Shared**: audio plays on both controller and TV. ~10–20 ms processing delay on the controller side is accepted as-is (no TV-side delay matching).

### 7.3 "Mute TV when redirect" toggle
- Per-controller toggle. Default OFF (TV stays as-is).
- When ON and Exclusive mode is active: TV audio is muted while the redirect is active; restored on stream end or toggle-off.

### 7.4 Defaults on first connection
- New controller, no prior CTM settings → no intercept. Game's USB writes propagate naturally. Audio plays wherever the game/host decided (typically TV speakers).
- The user must explicitly toggle redirect to take over.

### 7.5 Persistence
- All overlay settings (latency, audio_redirect, volumes, mode) persist **per-controller, globally** (not per-game, not per-session). Tied to the controller's stable identifier (MAC for BT controllers).

### 7.6 Multi-controller audio
- Each audio-capable controller can independently enable redirect.
- Multiple controllers simultaneously playing redirected audio is supported (TV mute toggle is per-controller; if any controller has it ON, TV mutes).

## 8. Map manifest format

Each map gains an `[advertise]` section. Example for DS5:

```ini
[advertise]
controller_name = DualSense
rumble = true
leds = true
adaptive_triggers = true
haptics = true
audio_output = true
audio_input = false
audio_redirect = true
volume_control_speaker = true
volume_control_headphone = true
audio_route_modes = off, headphones, speaker, both, split
latency_adjustable_ms = 20..200
battery = true
stop_sniff_rate = true
overlay_settings = latency, audio_redirect, volume_speaker, volume_headphone
```

Steam Puck (info-only):
```ini
[advertise]
controller_name = Steam Controller (Puck)
overlay_settings =
```

Modes (if needed) declared in `[mode.xxx]` sections at end of map.

## 9. Advanced settings (Moonlight TV settings page, not overlay)

Global tweaks that don't belong in the overlay:
- **Stop-sniff rate** for DS5 (BT keep-alive ping interval). Default + slider with clear explanation of what it does.
- CTM mode on/off master toggle.
- Per-controller "forget settings" / "reset to defaults" buttons.

## 10. Sunshine UI (host)

Minimal:
- One checkbox: "Use CTM Bridge".
- When ON: Sunshine advertises CTM capability and spawns `ctm-usbip` when Moonlight requests it.
- When OFF or unsupported: standard SDL → ViGEm path.
- No advanced per-controller UI on host side. All per-controller settings live in the Moonlight overlay.

## 11. Out of scope (deferred / future)

- Mic input (audio_input = false in all current maps).
- Jack detection auto-routing (DriftGuard does this — low priority).
- Per-game profile selection.
- Linux host backend for `ctm-usbip` (kernel `usbip-vudc`).
- Steam Link client integration.
- Xbox (GIP) host-side support (webOS jail blocks it on TV side; host-side TBD).
- Pairing controllers from inside Moonlight UI (use webOS native settings).

## 12. Open architecture questions

Tracked in `~/.claude/projects/D--Work-CMU/memory/ctm_bridge_architecture_questions.md`. To resolve before/during implementation:
- Strip or keep `SS_CMU_HID_*` packets in moonlight-common-c.
- Process ownership / child-spawn vs external service.
- TV ↔ host transport: independent TCP vs piggyback on Moonlight stream.
- Multi-controller slot count cap.
- Linux Sunshine backend timing.
- Existing `cmu_bridge` legacy code: delete now vs phase out.
