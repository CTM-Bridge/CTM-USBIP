# CTM Bridge — decision log

Append-only log of architectural / scoping decisions taken during the
autonomous integration work. Each entry: timestamp, what, why, files
touched, rollback note.

---

## 2026-05-26 — Session start: requirements doc, decision log init

- Drafted `docs/ctm_bridge_requirements.md` from user-confirmed answers to
  10 functional questions.
- User explicitly granted autonomous-work mandate: strip legacy CMU
  custom-driver integration from Sunshine + moonlight-tv, replace with
  USBIP-spawn-orchestrated CTM Bridge architecture, take architecture
  decisions independently with deep inspection.
- Backup destination chosen: `D:/Work/CMU/backups/`.

## 2026-05-26 — Decision: piggyback Moonlight ↔ Sunshine transport (vs direct TCP)

- **Decision**: TV-side `ctm-bridge` does NOT open an independent TCP
  socket to host-side `ctm-usbip`. Instead, CTM bytes tunnel through the
  existing Moonlight ↔ Sunshine streaming protocol (control / input
  channel).
- **Why** (user direction): "I would piggyback moonlight-sunshine
  transport route". Reuses Moonlight's encryption/pairing/NAT-traversal;
  no new firewall holes; no port-collision risk.
- **Implication**: must add new packet types to both moonlight-common-c
  copies (vendored in moonlight-tv + submodule in Sunshine). Shape
  similar to legacy `SS_CMU_HID_*` set, but cleaner (renamed CTM, no
  custom-driver-specific assumptions).
- **Trade-off**: more code than direct-TCP, but architecturally aligned
  with how Moonlight already handles input/feedback.

## 2026-05-26 — Sunshine outer strip complete

- Reverted 11 files via `git checkout HEAD --` (all CMU-driver-specific
  changes in cmake/, src/, src_assets/web/).
- Deleted untracked `src/cmu_bridge.cpp` and `src/cmu_bridge.h` (backed
  up to `D:/Work/CMU/backups/sunshine_cmu_legacy_2026-05-26.zip`).
- **Kept**: `cmake/dependencies/Boost_Sunshine.cmake` (Boost EXACT
  relaxation, build-env fix), `cmake/packaging/windows_wix.cmake` (WiX
  WARNING-vs-FATAL relaxation), `scripts/windows_build.sh` (local MSYS2
  build wrapper — useful, will repurpose for CTM).
- **Deferred**: Sunshine's `third-party/moonlight-common-c` submodule
  has 5 dirty files (SS_CMU_HID_* protocol additions). Diff saved to
  `D:/Work/CMU/backups/sunshine_moonlight-common-c_cmu_additions_2026-05-26.patch`.
  Will be handled in the CMU→CTM rename pass when I re-add cleaned
  CTM-named packets on top of upstream HEAD.

## 2026-05-26 — Found Sunshine MinGW build script

- `scripts/windows_build.sh` in the Sunshine fork. Runs under MSYS2
  UCRT64. `bash scripts/windows_build.sh` for clean (no CMU) build;
  `CMU=1 bash ...` for CMU-enabled. Installs pacman deps idempotently,
  initializes submodules, configures with Ninja, builds, then NSIS
  packaging. Will repurpose this for CTM later.

## 2026-05-26 — moonlight-tv outer strip complete

- Reverted 8 files via `git checkout HEAD --`. All CMU-driver-specific
  changes (settings, session lifecycle, gamepad bypass, UI checkbox).
- Deleted untracked `src/app/platform/webos/cmu_bridge_webos.[ch]`
  (backed up to `D:/Work/CMU/backups/`).
- **Kept**: AresPackage tweak, line-ending changes on scripts (unrelated
  local changes), `scripts/webos/local_ipk_build_macos.sh` (local build
  script).
- **Deferred**: `core/moonlight-common-c` submodule dirty (5 files w/
  SS_CMU_HID_* additions). Patch saved to
  `D:/Work/CMU/backups/moonlight-tv_moonlight-common-c_cmu_additions_2026-05-26.patch`.

## 2026-05-26 — CMU→CTM rename: DEFERRED to dedicated session

- 538 `cmu_/Cmu/CMU` occurrences across 24 files in `CMU-USBIP/` +
  `cmu-bridge-test-webos/`, plus 13 uncommitted files in CMU-USBIP from
  prior sessions whose contents I don't fully understand. Mass sed risk
  is too high without user at the keyboard to verify each batch builds.
- **Decision**: skip the rename in this session. New CTM integration
  code will use `ctm_*` prefix from day one; legacy `cmu_*` code stays
  intact. A focused rename pass can happen later when user can verify
  per-file builds.
- Backups created at `D:/Work/CMU/backups/`:
  - `CMU-USBIP_pre-CTM-rename_2026-05-26.zip` (src+include+maps+profiles+app+docs+build.ps1)
  - `cmu-bridge-test-webos_pre-CTM-rename_2026-05-26.zip` (src+cmake+deploy+scripts+CMakeLists+README)

## 2026-05-26 — Forward plan

Remaining autonomous work this session:
1. **moonlight-common-c packet additions** — clean CTM-named packets in both copies (built on top of upstream HEAD, replacing the dirty SS_CMU_HID_* set).
2. **Sunshine**: add CTM config + Vue checkbox; spawn-orchestrator scaffolding at session lifecycle hooks.
3. **moonlight-tv**: add CTM setting + LVGL toggle; spawn-orchestrator scaffolding; new `ctm_bridge_webos.[ch]` skeleton.
4. **Overlay UI scaffolding** in moonlight-tv: top-left controller panel reading map `[advertise]` (skeleton, hardcoded controller list to start).
5. **TV-side map manifests**: add `[advertise]` section to existing DS4/DS5/Steam Puck maps in `CMU-USBIP/maps/` (these stay the source of truth).
6. **Build verification** at each stage: cmu-usbip via build.ps1 (works), moonlight-tv IPK via WSL (works). Sunshine MSYS2 build not yet verified in this session — may need user to confirm MSYS2 path or run windows_build.sh.

## 2026-05-26 — Scaffolding committed

Three forks now carry the CTM Bridge skeleton:

- **Sunshine** `6b58cee1`: config flag `input.ctm_bridge`, Vue checkbox,
  `src/ctm_bridge.{h,cpp}` stub orchestrator, main.cpp init/shutdown
  wiring, CMake includes the new sources.
- **moonlight-tv** `07c490a4` + `8bbfad1c`: setting `ctm_bridge`, ini
  read/write/default, new `src/app/platform/webos/ctm_bridge_webos.{c,h}`
  stub, session_worker.c session start/stop hooks (under TARGET_WEBOS),
  input.pane.c checkbox.
- **CMU-USBIP** `0795d04`: maps DS4/DS5/Steam Puck/HID identity gain
  `[advertise]` manifest section. Runtime ignores it today; ctm-bridge
  will read it at controller-attach time.

## 2026-05-26 — What stubs DO not DO yet (for hardware test)

When you enable both CTM toggles + start a stream:
- Sunshine logs "ctm_bridge: initialized (skeleton — no spawn
  implementation yet)" at daemon start.
- moonlight-tv logs "ctm_bridge_webos_session_start: spawn not
  implemented; falling back to standard path" on stream start.
- Standard SDL → ViGEm/uinput controller path runs normally.

Spawn implementations land next iteration. The stub commits exist to:
- Validate that strip+add doesn't break either fork's build.
- Land the UI surface so user can toggle and see the wire — even
  though it's no-op behaviorally.
- Reserve the lifecycle hooks at the right spots in stream.cpp /
  session_worker.c.

## 2026-05-26 — Session start/stop wiring in Sunshine: NOT wired yet

- `ctm_bridge::session_start(session_id, client_supports_ctm)` declared
  but no caller. Hook spot is `stream.cpp:1963 start(session_t&, ...)`
  after `session.input = input::alloc(...)`. Needs `client_supports_ctm`
  which doesn't exist yet — depends on capability negotiation packet
  (Sunshine-side advertise + Moonlight-side request) which itself
  depends on the moonlight-common-c packet design. Deferred to the
  protocol-design pass.
- `ctm_bridge::session_stop(session_id)` similarly unwired. Hook spot
  is `stream.cpp:1957` just before `platf::streaming_will_stop()`.

## 2026-05-26 — Overlay UI scaffolding: deferred

- The top-left controller panel in the in-stream Moonlight overlay
  (`src/app/ui/streaming/streaming.view.c:14-151`) is significant LVGL
  work and the per-controller card rendering needs the `[advertise]`
  parser on the TV side which itself depends on the spawn binary
  shipping the map content. Skipped this session to keep the commits
  small + reviewable. Will land in a dedicated UI session.

## 2026-05-26 — All builds GREEN

End-of-session build check after all CTM scaffolding commits:

| Build | Result | Output |
|---|---|---|
| cmu-usbip Release | 0 err | `D:\Work\CMU\CMU-USBIP\out\x64\Release\cmu-usbip.exe` |
| cmu-usbip Debug | 0 err | `D:\Work\CMU\CMU-USBIP\out\x64\Debug\cmu-usbip.exe` |
| cmu-bridge-test-webos IPK | 0 err | `D:\Work\CMU\dist\com.local.cmubridge_0.1.0_arm.ipk` |
| moonlight-tv IPK | 0 err | `D:\Work\CMU\moonlight-tv\dist\com.limelight.webos_1.6.36_arm.ipk` |
| Sunshine (MSYS2 UCRT64) | 0 err | `D:\Work\CMU\sunshine\Sunshine\build\sunshine.exe` + `tools\sunshinesvc.exe` |

Sunshine MSYS2 build invoked via `C:\msys64\usr\bin\bash.exe` with
`MSYSTEM=UCRT64`. Used `SKIP_DEPS=1 SKIP_SUBMODULES=1 SKIP_INSTALLER=1`
to skip the slow pacman + submodule init + NSIS packaging steps —
these only matter for clean-room builds. The web UI also rebuilt
cleanly (`vite` output above includes the new `ctm_bridge` checkbox
strings).

Ready for hardware test tomorrow.

## 2026-05-29 — Architecture (B): single agent, multi-device + fixes

Host side switched from "one ctm-usbip process per controller" to the
**agent** model: one process, one USB/IP server on 3240, N busids.
(agent mode already implemented single-server-multi-device; rounded it
out + rewired Sunshine to drive it.)

- ctm-usbip agent.inl: added `puck` kind; `BRIDGE_STOP` now really stops
  the session (was no-op) → clean Windows unplug on controller-off.
- Sunshine ctm_bridge.cpp: `init()` spawns ONE `ctm-usbip agent 48054`;
  `attach()` → `BRIDGE_START <kind> <port> <busid>` + ctmb_ client;
  `detach`/`shutdown` → `BRIDGE_STOP` + kill agent. No 3240 collision.
- `pick_kind(vid,pid,bus)`: USB → generic identity ("hid"); BT → ds5/ds4.
  (BT maps are transport-specific; USB controllers identity-forward.)
- TV (ctm_bridge_webos.c): sends REAL bus (USB=3/BT=5) in DEVICE_INFO
  instead of hardcoded BT; Sunshine input.cpp threads packet->bus in.
- ViGEm phantom fixed: input.cpp skips alloc_gamepad on both paths when
  ctm_bridge is on ("CTM = no ViGEm, period").

Still open: SS_CTM_HID_MAX_REPORT_SIZE=256 too small for DS5 output/audio
(398/547B) — raise to 4096 in both moonlight-common-c copies; Steam Puck
composite still single-interface.

## 2026-05-26 — Items 1-4, 6, 7 done (full end-to-end wiring)

After the strip+scaffolding session, the integration path is now wired
end-to-end and builds clean on all five targets:

- **moonlight-tv** `158ad498`: full hidraw pipeline (renamed CMU→CTM).
- **Sunshine** `a22faaab`: full legacy-shaped wiring restored + renamed.
- **Sunshine** `604abfbc`: ctm_bridge.cpp REWRITTEN from scratch as a
  TCP `cmub_` client to a spawned `cmu-usbip` (~500 lines, replaces
  ~850 lines of CMU custom-driver shared-memory plumbing).
- **moonlight-common-c (both submodules)**: `SS_CTM_HID_*` protocol
  packets reinstated, renamed.

The full byte path when both toggles are on + a controller is paired
on the TV:

```
TV /dev/hidraw
  → ctm_bridge_webos reader thread
  → LiSendCtmHidInputReport (moonlight-common-c SS_CTM_HID_INPUT)
  → ENet control channel
  → Sunshine input.cpp passthrough(PSS_CTM_HID_REPORT_PACKET)
  → ctm_bridge::submit_report  (Sunshine-side)
  → cmub_ INPUT_REPORT over local TCP
  → spawned cmu-usbip
  → USB/IP virtual device
  → Windows kernel + game
```

Output reports (rumble / LEDs / feature roundtrips) ride the reverse
path: cmu-usbip → cmub_ OUTPUT_REPORT/FEATURE_GET → ctm_bridge reader
thread → Sunshine gamepad_feedback queue → 0x5504/0x5505 control
packet → Moonlight ctmHidOutput/ctmHidFeatureRequest listener →
ctm_bridge_webos_write_output / hidraw write.

Capability negotiation: implicit — when Moonlight detects a paired
controller it sends `LiSendCtmHidDeviceInfo`. Sunshine's passthrough
calls `ctm_bridge::attach` which spawns cmu-usbip; if cmu-usbip isn't
on PATH (or `config.ctm_profile_path` doesn't point at it),
`init()` returns false and `available()` is false, so attach is a
no-op and the standard SDL → ViGEm path runs. Fail-safe.

### Configurable

- `config.ctm_virtual_driver` (Sunshine): master toggle
- `config.ctm_profile_path` (Sunshine): path to CMU-USBIP install
  containing `cmu-usbip.exe` + `profiles/descriptors/` + `maps/`.
  Empty = look next to sunshine.exe in `ctm_profiles/` and `ctm_maps/`.
- `use_ctm_virtual_driver` (moonlight-tv): TV-side toggle

### Profile/map auto-selection (Sunshine `pick_profile`)

| VID:PID | Profile | Map |
|---|---|---|
| 0x054C:0x0CE6/0x0DF2/0x0E5F | ds5_composite.profile | ds5_usb_over_ds5_bt.map |
| 0x054C:0x05C4/0x09CC/0x0BA0 | ds4_composite.profile | ds4_usb_over_ds4_bt.map |
| 0x28DE:0x1304 | steam_puck.profile | steam_puck_identity.map |
| (other) | auto (dynamic) | hid_identity.map |

### What's still TBD (deferred per user direction)

- **Item 5 — Overlay UI**: top-left controller panel in moonlight-tv
  reading map `[advertise]`. Deferred until end-to-end controller
  works on hardware.
- **Item 8 — CMU → CTM mechanical rename** across CMU-USBIP/ and
  cmu-bridge-test-webos/. 538 occurrences. Cosmetic; doesn't block
  functionality. Will run as a dedicated rename pass.
- **Item 9 — Steam Puck composite** (4 HID + 1 mgmt + 2 CDC). Today's
  level: single-interface only.
- **Item 10 — Linux Sunshine backend** (kernel usbip-vudc). User
  explicitly deferred.
- **Generic xpad / Xbox / Gulikit** — webOS-side blocked (per
  xpad_xbox_webos_findings.md).
- **Real-hardware test** — needs the user.

### Build artifacts

| Build | Output |
|---|---|
| cmu-usbip Release | `out/x64/Release/cmu-usbip.exe` |
| cmu-usbip Debug | `out/x64/Debug/cmu-usbip.exe` |
| cmu-bridge-test-webos IPK | `D:\Work\CMU\dist\com.local.cmubridge_0.1.0_arm.ipk` |
| moonlight-tv IPK | `D:\Work\CMU\moonlight-tv\dist\com.limelight.webos_1.6.36_arm.ipk` |
| Sunshine installer | `D:\Work\CMU\sunshine\Sunshine\build\cpack_artifacts\Sunshine.exe` |

## 2026-05-26 — Binary distribution: Option B (independent release)

- User pick: CTM binaries (`ctm-usbip`, `ctm-bridge`) ship from their
  own release artifact, not bundled into the Sunshine installer or
  Moonlight IPK. Sunshine + Moonlight just spawn from PATH (or a
  configurable path setting if PATH is impractical on webOS).
- Future option (user-stated): re-bundle later — "we can eventually
  add the binary and usbip driver to the installer".
- Implication for next session: produce a CTM Bridge release artifact
  containing both binaries + maps + profiles, with clear install
  instructions. Sunshine + Moonlight just need to know how to find it.

## 2026-05-29 — REVERSAL: direct TCP for CTM HID (undoes 2026-05-26 piggyback)

- **Decision**: When "Use CTM Virtual Driver" is ON, moonlight-tv connects
  **directly over TCP** to the host-side `ctm-usbip` agent (the proven
  `ctm-bridge-test-webos` CTMB path), bypassing the Moonlight↔Sunshine
  ENet transport entirely for HID. This REVERSES the 2026-05-26 "piggyback
  Moonlight transport" decision.
- **Why**: ENet reliable+ordered + ~8ms control-stream service tick caps
  the effective HID rate. Measured: piggyback ≈125Hz; after per-report
  flush ≈282Hz avg (jitter 7.8ms); **direct-TCP standalone = 500Hz avg,
  1.9ms latency, 2.7ms jitter** (user reference). ENet cannot match raw
  TCP here (window/throttle/ack servicing).
- **Bonus**: the CTMB connection is bidirectional — one socket carries
  input up AND output/haptics/audio + FEATURE_GET/SET down. So this also
  resolves the DS5 output path (haptics/audio/triggers, "item 3") for
  free, since it rides the same proven path the standalone uses.
- **Architecture**: agent runs independently on Windows (eventually a
  **Windows service**, auto-start, LAN-bound). moonlight-tv on stream-start
  drives the standalone two-phase handshake: (1) control → `BRIDGE_START
  <kind> <dataPort> <busid>` on 48054 → agent creates bridge + does the
  usbip attach locally (`run_usbip_attach`); (2) data → connect dataPort →
  `HELLO` (caps+descriptor) → `HOST_CONFIG` → stream. **Sunshine is out of
  the HID loop** (it just streams video; stops spawning the agent).
- **`pick_kind` moves to the TV** (vid/pid→puck/ds5/ds4/hid), since `kind`
  is chosen at BRIDGE_START before the descriptor is seen. Matches the
  "all device knowledge on TV/maps" philosophy.
- **OFF → default SDL→ViGEm**, untouched. Missing profiles added as needed.
- **Accepted trade-offs** (costs the 2026-05-26 decision avoided): LAN-only
  (no NAT traversal), CTMB is unencrypted, requires a Windows firewall hole
  for the agent ports. Acceptable for local streaming.
- **Double-input guard**: TV-side only — a bridged controller must not also
  be sent over the normal gamepad path; Sunshine needs no change.
- Agent/bridge ports already bind `INADDR_ANY` (agent.inl:346,
  bridge.inl:102); usbip port stays loopback (server.inl:74) — correct.
- **Rollback**: revert moonlight-tv `ctm_bridge_webos.c` to the
  `LiSendCtmHidInputReport` ENet sender; re-enable Sunshine agent spawn.

## 2026-06-03 — Decision: composite puck via forwarded LIVE enumeration (not a static profile)

- **Decision**: bridge the Steam Puck as the full composite it is, built from the
  puck's OWN live USB enumeration captured on the TV — NOT from a hand-authored
  static `.profile`, and NOT using any external (`hid-steam.c`) protocol knowledge.
- **Flow**:
  1. Capture at physical plug (monitor connect): read `<devdir>/descriptors` +
     each interface `report_descriptor` + `ep_XX` from sysfs ONCE; cache keyed by
     the USB device (enumeration is static — wireless controllers don't change it).
  2. Forward the cached enumeration verbatim over a new CTMB message on bridge
     Plug in; the TV does not interpret it.
  3. Windows builds the virtual composite from the forwarded bytes; `usbccgp`
     spawns the child tree.
  4. Link EVERY puck hidraw (Controller 0-3 + Service); tag each interface's input
     with its source IN endpoint (CTMB `request_id`); route feature/output
     per-interface back to the matching hidraw. `device.inl` already delivers
     input per endpoint (`inputEndpointStates_`/`handle_endpoint_in`), so this is
     tagging + feature/output routing, not new USB plumbing.
- **COM/CDC** (`ttyACM0` cls02 + cls0a): deferred (TBD) — gamepad path is all HID.
- **Why**: clean-room (no `hid-steam.c`; this puck postdates it); TV stays blind
  (forwards bytes); Windows owns the build — matches the project philosophy.
- **Supersedes** the stashed composite attempt (static `steam_puck_composite.profile`
  + hid-steam-derived detection), parked in `stash@{0}`, not reused.
