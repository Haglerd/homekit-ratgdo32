# Change Log

**v3.x.x firmware is for ratgdo32 and ratgdo32-disco boards only**

All notable changes to `homekit-ratgdo32` will be documented in this file. This project tries to adhere to [Semantic Versioning](http://semver.org/) and [Keep a Changelog](https://keepachangelog.com/).

---

## Fork releases (`v3.4.4-forceclose.N`)

This section documents changes specific to the `Haglerd/homekit-ratgdo32` fork. Upstream changes are listed in the `v3.x.x` section below; the fork tracks upstream and adds these on top.

### v3.4.4-forceclose.82 (2026-05-13) — UI: once-per-page-load version check + rate-limit error message

Two web UI bugs surfaced by the `.80 → .81` OTA soak:

**1. `checkVersion()` was being re-invoked on every SSE reconnect.** `checkStatus()` calls `checkVersion()` inside the `status.json` `.then()`, and `checkStatus()` is correctly called from SSE error/timeout handlers to re-establish the subscription. Post-OTA the device's SSE stack flaps for 2–3 min during the early-boot bring-up (HomeKit init + GDO discovery + slot bring-up), so `checkVersion()` fired every 1–3 seconds — burning GitHub API rate-limit (60 req/hr unauthenticated) and visually flashing the dots animation. Now: `versionCheckedOnce` module-level flag gates the auto-call. The manual "Check for update" button bypasses the guard and re-invokes directly.

**2. Rate-limit error path showed blank instead of explanation.** Pre-`.82`, `checkVersion()`'s error path cleared `versionElem` to blank — leaving the user staring at empty space under "Firmware:" with no indication of why. Now: shows `"Unable to check (GitHub rate-limited)"` on 403/429 and `"Unable to check for updates"` on other errors.

**Files**: `src/www/functions.js`, `docs/manifest.json`. No firmware behavior change.

### v3.4.4-forceclose.81 (2026-05-13) — UI: clear version-check dots animation on GitHub API error path

`checkVersion()` (`src/www/functions.js`) starts a `dotDotDot` `setInterval` to animate dots under the firmware version while the GitHub releases API fetch is in flight. The success path at line 1139 cleared the interval; the error path at lines 1067-1073 returned early WITHOUT clearing it, so on GitHub API rate-limit (60 req/hr unauthenticated — easy to hit right after a release tag when the dev's browser + auto-release workflow are both polling) the dots would animate forever, growing then resetting every 10s.

**Fix**: add `clearInterval(aniDots)` + clear `spanDots.innerHTML` to the error-path early-return. One-line UX bug, no firmware behavior change.

**Files**: `src/www/functions.js`, `docs/manifest.json`.

### v3.4.4-forceclose.80 (2026-05-13) — HK-FC: gate release callback on `forceCloseInProgress` (log-audit-20260513-007, P1)

**P1 cleanup of a force-close state-machine race surfaced by the 18h post-`.79` soak.**

At 2026-05-13T18:45 CDT, `send_force_close_release_then_maybe_retry` (esp_timer task, fires ~hold_ms after press scheduling) ran AFTER `clear_force_close_state` had already zeroed `forceCloseAttempt` + cleared `forceCloseInProgress` — the door reached `CURR_CLOSING` during the hold, `comms.cpp:1122` fired the unconditional unwind, and the in-flight release callback then mis-logged `"release sent (2-attempt mechanic, attempt 0/2)"` and armed a phantom attempt-2 (correctly dropped by the gap-arm drain at `ratgdo.cpp:449`).

**Fix:** ACQUIRE-load `forceCloseInProgress` at release-callback entry. Always send the release packet (must pair with the press; skipping would leave the GDO observing an indefinite wall-button hold), but suppress the "attempt N/2" log + attempt-2 scheduling when state was cleared upstream. All existing force-close paths (single-hold, 2-attempt success, 2-attempt schedule) take the `stillInProgress=true` branch and behave identically to `.79`.

**Files**: `src/comms.cpp` (release-callback entry gate), `docs/manifest.json` (version bump).

**Build**: ESP32 Flash 96.3% / RAM 26.2% — no delta from `.79`. ESP8266 unaffected (non-`USE_GDOLIB` branch).

### v3.4.4-forceclose.79 (2026-05-09) — log-audit-006: gate `mdns` task + heap-floor short-circuit (P0 — fixes .78 panic recurrence)

**Critical P0 follow-up.** `.78` (PR #122) fixed the `tiT` re-entry chain but the device still panicked once after ~10h46m uptime. Crash dump showed the same neighborhood (`0x4008EBBC` vs `.77`'s `0x4008EBC4`) but the originating task was `mdns`, not `tiT`. The mDNS service task owns its own RX path and was not in PR #122's gated set.

**Pre-panic conditions** (from syslog):
- `19:54:50` `mdns_networking: Cannot allocate memory (receive(176), free heap: 380 bytes)`
- `19:55:26` panic on task `mdns` at `0x4008EBBC` (IllegalInstruction → abort path)
- Boot at `19:55:35` with `restart reason: 4` (ESP_RST_PANIC)

**Fix (two layers, both in `LOG::logToBuffer`):**

1. **Add `"mdns"` to the network-task name set.** Closes the immediate hole — the mDNS task can no longer fan out its own OOM `ESP_LOGE` into a `socket()` call from inside its own context.

2. **Heap-floor short-circuit (`SYSLOG_HEAP_FLOOR_BYTES = 4096`).** Defense-in-depth for any task we missed. Even on a non-gated task, opening a UDP socket needs ~1 KB of heap for the lwIP control block + pbuf; below ~4 KB, `socket()` either returns ENOMEM (cheap) or asserts in a low-level alloc path (panic). When free heap is below the floor, drop the network fan-out entirely — the line is still in the on-device ring buffer for `/showlog`.

**Acceptance**: `.79` runs 24h+ clean across iOS-quiet stretches AND mDNS OOM bursts without panic.

**Files**: `src/log.cpp` (gate-mdns + heap floor), `docs/manifest.json` (version bump).

### v3.4.4-forceclose.78 (2026-05-09) — log-audit-005 REDUX: name-based tiT-task gate (P0 — fixes panic loop)

**Critical P0.** The PR #105 (.71) fix for the BOOT-OOM-MDNS panic was BROKEN — wasn't catching the case it claimed to. User device on `.77` panicked **13 times overnight** (between 02:51 and 06:13 CDT 2026-05-09) with the exact same `mdns_mem_calloc` → `ESP_LOGE` → `logToSyslog` → `socket()` from `tiT` → `__assert_func` chain that PR #105 was supposed to prevent.

**Confirmed crash dump** (from /crashlog on the device):

```
E (02:16:41.355) mdns_networking: Cannot allocate memory (receive(176), free heap: 136 bytes)
Crash in task: tiT, at address: 0x4008EBC4
0x4008EBC4  panic_abort
0x4008EB89  esp_system_abort
0x40095705  __assert_func
0x401E7833  _strerror_r
0x4010C33F  logToSyslog            ← OUR code, called from tiT
0x4010C635  LOG::logToBuffer
0x4010C6F6  esp_log_hook
0x4009544D  esp_log_va
0x40172A72  mdns_mem_calloc
0x40145F99  udp_input
0x4014956F  ip4_input
0x4008FB49  vPortTaskWrapper       ← tiT task entry
```

Identical call-chain shape to the original `.65` panic, just shifted by ASLR/build-relocation.

**Why the .71 fix didn't catch it**: PR #105 cached `xTaskGetHandle("tiT")` on first call to `LOG::logToBuffer`. If the very first `ESP_LOGx` fires BEFORE the lwIP TCP/IP task is created (early boot), `xTaskGetHandle("tiT")` returns `nullptr` and gets cached as null forever. Subsequent calls FROM real-tiT-task then never match (curTask is non-null real handle, cache holds null), so the `fromNetworkTask` check is always false and the syslog re-entry path is never blocked.

**Redux fix**: replace cached-handle compare with `pcTaskGetName(curTask)` + `strcmp`. No caching to get wrong; `pcTaskGetName` is a TCB pointer-deref (~ns). Checks all four task names: `"tiT"` (lwIP TCP/IP task), `"tcpip_thread"` (alternate lwIP name on some IDF versions), `"wifi"` (WiFi task), `"sys_evt"` (esp-netif event task). All four can originate ESP_LOGx that would re-enter lwIP if forwarded to syslog/SSE.

**Why heap fell to 136 bytes**: separate concern. iOS hub state-sync + HomeSpan HAP + mDNS query bursts can momentarily consume nearly all heap. The fact that mDNS *attempts* its 176 B alloc when only 136 B free is a healthy lwIP behavior — the alloc fails, mDNS drops the packet, logs ESP_LOGE, and life continues. **The panic is OUR firmware mishandling that benign log line, NOT mDNS itself.** Heap-pressure mitigation is a separate workstream (HK-FC-MIGRATE / HANG-WATCH queue items track related); this PR fixes the lethal log-fanout re-entry independently.

ESP8266 path unchanged. Force-close FSM untouched. Build: Flash 96.1%, RAM 26.2%.

### v3.4.4-forceclose.77 (2026-05-08) — HK-FC: rate-limit overlap-rejection log on iOS HK redundant-close bursts

Hygiene fix surfaced from `.76` soak. Field observation 2026-05-08 showed iOS HomeKit / Apple home-hub state-sync periodically firing bursts of redundant `target=Closed` writes — three separate bursts of 6-13 close commands within 1-2 seconds each, all redirected through the HK-FC mode 2 path. Each burst correctly hit the `door_command_force_close` overlap-rejection guard (the `__atomic_test_and_set(&forceCloseInProgress)` check), but each rejection logged a per-event `WARN` line — flooding the 16 KB on-device ring buffer and wrapping preceding context out before `/showlog` could be fetched.

**Fix**: rate-limit the overlap-rejection log to one line per 5 s window, with a suppressed-count summary on the next post-window fire — same `.72` SEC1 TX-fail / v46 SSE buffer-full pattern. First rejection logs immediately; subsequent rejections within 5 s are counted silently; next rejection after the window emits:

```
W FORCE CLOSE: ignoring request — a sequence is already in progress [+12 suppressed in last 1850ms — iOS HomeKit redundant-close burst]
```

**Defense semantics unchanged** — every redundant request still gets rejected by the atomic test-and-set guard. Only the log cadence drops. Force-close FSM internals untouched. ESP8266 path unchanged.

**About the iOS-side cause**: not addressable from firmware. Likely an iOS Home automation duplicated across multiple paired controllers (iPhone + iPad + Apple TV + HomePod each firing the same close trigger), Apple home-hub state-sync retrying writes when target!=current, or a HomeKit scene with multiple participants. The firmware's overlap-rejection guard correctly prevents double-toggle in all cases — door closes once cleanly. Document for users to investigate iOS Home → Automations if they see the warn-burst pattern.

Build: Flash 96.1%, RAM 26.2%.

### v3.4.4-forceclose.76 (2026-05-08) — HK-FC: door-state safety gate (P0 — fixes unintended door open)

**Critical safety fix.** A redundant `target=Closed` HomeKit characteristic write to an already-closed door under HK-FC mode 2 fired the force-close press packet, which on Sec+1.0 GDOs is a wall-button toggle: pressing while the door is Closed **opens it**. Real-world incident at 2026-05-07T22:31:03 CDT: iOS hub state-sync wrote `target=Closed` to an already-closed door, force-close attempt 1 toggled it open (door at ~Opening when 1500 ms gap fired), attempt 2 toggled it back to Stopped — left the garage door **half-open at night**, until iOS re-triggered close 41 seconds later.

**Fix**: gate `door_command_force_close()` at the entry point on the door's current state. If `CURR_CLOSED` or `CURR_CLOSING` (or unknown 0xFF pre-init), log a `WARN` and return — no press packet, no movement. Only fires the press when door is `CURR_OPEN`, `CURR_OPENING`, or `CURR_STOPPED` — the states where force-close has a meaningful intent.

The fix is at the single dispatch point so it protects **every** force-close caller:
- HK-FC mode 2 primary tile (the actual incident path)
- HK-FC mode 1 companion force-close tile (same bug if anyone wired an automation against it)
- Legacy `/setgdo forceClose=<ms>` POST endpoint used by `homebridge-ratgdo-forceclose`
- Auto-close TTC fire path (already gates on door=Open, but safer to belt-and-suspenders)

Matches the legacy `close_door()` semantics — close on already-closed = silent no-op, never opens the door. Force-close FSM internals (2-attempt sequence, single-hold, per-mechanic hold-ms) untouched. ESP8266 path unchanged. Build: Flash 96.1%, RAM 26.2%.

### v3.4.4-forceclose.75 (2026-05-07) — HK-FC: separate hold-ms per mechanic + UI cleanup + logging fixes

User-feedback follow-up to `.74`. Three issues addressed in one PR:

**1. Two independent hold-ms fields, one per mechanic.** `.74` shared a single `forceCloseHoldMs` between the 2-attempt and single-hold mechanics; flipping the checkbox without remembering to bump the duration left users at 3500 ms in single-hold mode (way too short to trigger override on most GDOs). Now there are two persisted fields:
- `forceCloseHoldMs` (default 3500, range 1000-10000) — per-press hold for 2-attempt mechanic
- `forceCloseHoldMsSingle` (default 7000, range 1000-15000) — total continuous hold for single-hold mechanic

The active value is picked at force-close-fire time based on `forceCloseSingleHold`. Flipping the mechanic checkbox no longer trashes either timing. `comms_refresh_force_close_single_hold` chains into `comms_refresh_force_close_hold_ms` so the cache rotates correctly when the flag flips. `door_command_force_close` clamp range bumped to [1000, 15000] (union of both mechanics' ranges).

**2. Web UI cleanup.** The Force-Close row was a jam of inline `style="font-size:0.85em;"` and `&nbsp;` filler. Reworked to match the project's existing pattern (HomeKit Hint Levels block as the reference): label spans with `display: inline-block; min-width: 90px;`, `<br>` separators between fields, hint text styled `font-size: 0.75em; color: #888;`. Both hold-ms inputs visible all the time — clearer than hide-on-checkbox.

**3. Logging fixes.** The `.74` `send_force_close_press` log line said `attempt %d/2 press hold=Xms` even when in single-hold mode (where there is no "/2" — only one attempt). Same problem in `send_force_close_release_then_maybe_retry`'s `attempt %d release sent`. Both now branch on the cached mechanic flag:
- 2-attempt: `FORCE CLOSE: press fired, attempt 1/2 holding for 3500ms`
- single-hold: `FORCE CLOSE: press fired, holding for 7000ms (single-hold mechanic)`

**Empirical note from .74 testing**: single-hold (8000 ms) did NOT trigger photo-eye override on at least one Sec+1.0 GDO model (door closed-then-reopened — held press registered as repeated toggles after door reached Closed). 2-attempt at lower per-press hold (2500 ms) worked cleanly on the same hardware. Single-hold stays as an opt-in for users whose GDO does need continuous-hold semantics. See PR #115 for full test data.

ESP8266 path unchanged. Force-close FSM untouched. Build: Flash 96.1%, RAM 26.2%.

### v3.4.4-forceclose.74 (2026-05-07) — HK-FC single-hold press mechanic (opt-in)

User feature, complements `.73`'s tri-state mode. Adds an opt-in flag `forceCloseSingleHold` controlling the **press mechanic** of `door_command_force_close`:

- **Default OFF (legacy 2-attempt)**: existing behavior — `DoorButtonPress` → wait `forceCloseHoldMs` → `DoorButtonRelease` → 1.5 s gap → `DoorButtonPress` → `DoorButtonRelease`. From the GDO's perspective, two distinct button presses with a gap.
- **ON (single hold)**: one continuous press — `DoorButtonPress` → wait `forceCloseHoldMs` → `DoorButtonRelease`. Done. From the GDO's perspective, one long button-down — same wire-protocol shape as a human holding the wall button continuously, which is the canonical Sec+1.0 photo-eye-override pattern.

Recommended for setups whose GDO needs the continuous-hold override to bypass safety on close (e.g. door reverses partway in legacy 2-attempt; the press-release-press sequence breaks the override window). Set `forceCloseHoldMs` to ~6000-8000 ms when single-hold is enabled to mimic a sustained human button press.

**Web UI**: new `Single continuous hold (no retry)` checkbox alongside the existing hold-ms input under the Force-Close section. Hold-ms range unchanged at 1000-10000 ms.

**Cache plumbing**: `comms_refresh_force_close_single_hold()` mirrors the existing `comms_refresh_force_close_hold_ms()` pattern — settings save calls the helper, which writes the cache via `__atomic_store_n` (RELAXED). Both caches now also seeded at boot in `setup_comms` (was a pre-existing latent bug — `forceCloseHoldMsCached` only picked up its saved value after the first post-reboot settings save).

**No behavior change for existing users**: default OFF preserves the legacy 2-attempt sequence. ESP8266 path unchanged (force-close infrastructure is ESP32-only).

Heap delta: +5 B BSS (one bool config + cache + refresh function), ~140 B flash.

### v3.4.4-forceclose.73 (2026-05-07) — HK-FC tri-state mode (off / companion / replace)

User feature. The HomeKit force-close toggle was binary (off / companion-tile). Adds a third mode for users whose GDO **always** needs the long-press hold to close — skips the wasted normal-close-then-fall-back-to-force-close cascade.

**Modes** (`forceCloseHomeKit` in settings):
- **0 = Off** (default): primary tile close uses normal toggle press. No second tile.
- **1 = Companion**: primary tile uses normal close; a separate "Force Close Door" tile fires the force-close hold. Migrated automatically from the previous boolean `true`.
- **2 = Replace**: single tile (no companion). The primary close button calls `door_command_force_close(holdMs)` directly — saves the cascade for setups whose GDO always needs the long-press.

**Migration**: existing devices with `forceCloseHomeKit=true` deserialize to mode 1 (companion) — no behavior change. Mode 2 is opt-in.

**Web UI**: `Force-Close Tile:` checkbox replaced by `Force-Close Mode:` 3-option select. Hold-ms input unchanged (1000-10000 range).

**Implementation**: `cfg_forceCloseHomeKit` becomes `int` (was `bool`), `enable_service_homekit_force_close()` takes `int mode` (was `bool enable`). Boot-time second-accessory creation gated on `mode == 1`. `DEV_GarageDoor::update()` close path checks `mode == 2` and dispatches `door_command_force_close()` instead of `close_door()`. Sec+2.0 / dry-contact users on mode 2: `door_command_force_close` falls back transparently to a normal close (`comms.cpp:2880-2881`).

**Known follow-up** (not in this PR): the force-close 2-attempt sequence currently does press-release-press, so when the door reverses partway it lets the door come fully back up before re-pressing. Mechanically a continuous hold (mimicking the wall button) would be more efficient but requires hardware-side analysis of relay duty cycle limits + a way to release on door-state-changed-to-Closed. Filed as a future item.

### v3.4.4-forceclose.72 (2026-05-07) — SEC1 TX-fail log rate-limit (obstruction noise)

Hygiene fix surfaced from .71 soak. When a SEC1 (Sears/Genie) wall-panel send fails repeatedly — typically because the door is **physically obstructed** during a close, so the GDO is busy reversing and not ACKing — the wall-panel emulator queues the next packet → that one also exhausts retries → another `ESP_LOGE`. A single obstructed-close event from `comms.cpp:1665` produces ~45 identical `SEC1 TX send failed, exceeded max retry` lines over ~12 seconds, dominating the 16 KB on-device ring buffer and wrapping out the surrounding user-action context (door state transitions, light toggles, motion clears) before `/showlog` can be fetched.

**Fix**: rate-limit the per-packet "exceeded max retry" log to one line per 5 s window. First failure logs immediately. Subsequent failures within the window are counted but not logged. Next failure after the window emits a summary line with the suppressed count: `SEC1 TX send failed, exceeded max retry [+N suppressed in last Xms — obstructed door / busy bus]`. Same v46 pattern as the SSE buffer-full skip rate-limit. Packet-delivery semantics unchanged — only log cadence. ESP8266 path unchanged (same code, same fix applies).

### v3.4.4-forceclose.71 (2026-05-07) — log-audit-005 BOOT-OOM-MDNS panic real fix (`tiT` re-entry into lwIP)

**P0 fix.** PR #89 (.65 release) was a misdiagnosis — it deferred ratgdo's `mdns_service_add` calls until heap recovered above 50 KB, but the actual crash had nothing to do with our service registration timing.

**Real cause** (decoded from .65 stack trace via `addr2line`): mDNS's UDP receive callback runs on `tcpip_thread` (`tiT`). When the lwIP NETBUF pool can't satisfy a 176 B inbound packet allocation, mDNS calls `ESP_LOGE("mdns_networking", "Cannot allocate memory ...")` from inside `tiT`. Our `esp_log_vprintf` hook catches it and calls `logToSyslog(outLine)` → `WiFiUDP::beginPacket` → `socket(AF_INET, SOCK_DGRAM, 0)` from inside `tcpip_thread`. lwIP's socket API forwards every call as a tcpip-message to `tcpip_thread` and waits for a reply; if the caller IS `tcpip_thread`, the wait is for itself and lwIP's safety check fires `LWIP_ASSERT` → `_strerror_r` → panic in `tiT`. `IllegalInstruction`. The mDNS OOM is benign in isolation; it's our log fan-out that turned it lethal.

**Fix**: cache the FreeRTOS task handles for `tiT` / `wifi` / `sys_evt` once on first hit; in `LOG::logToBuffer`, skip both `SSEBroadcastState` AND `logToSyslog` when the originating task is one of them (the line still captures into the on-device message buffer + Serial — no observability loss). Also gate `xPortInIsrContext()` belt-and-suspenders. This protects against any future ESP-IDF component (DHCP6, IPv6 ND, OpenThread, lwIP TCP retransmit) emitting `ESP_LOGx` from a network task — all share the same re-entry hazard.

PR #89's heap-floor service-registration deferral is kept (orthogonal heap-pressure mitigation). ESP8266 path unchanged. Force-close FSM untouched.

Heap delta: +12 B BSS (cached task handles), ~200-400 B flash. No dynamic allocation in the gate path.

### v3.4.4-forceclose.70 (2026-05-07) — R-?-fork HomeSpan processSerialCommand thread-safety doc

Investigation-gate close. HomeSpan's `pollTask` holds `pollMutex` (a `std::shared_mutex`, exposed via `homeSpan.getMutex()`) for each iteration; state-mutating CLI commands ('F' factory reset, 'U' unpair) called from another task without taking that mutex would race with pollTask's accesses. Inspected the 4 fork call sites: `handle_reset` ('U' via `homekit_unpair`) and `helperFactoryReset` ('F') both reboot within ms-to-~500 ms, collapsing the race window; `homekit_dump_state` ('s'/'i'/'d') is read-only — torn-read cosmetic only. Mutex NOT added: pollTask iterations can take seconds, waiting on it from loopTask could trip the loop watchdog. **Closed as non-finding.** Doc comments added to `homekit_dump_state` and `helperFactoryReset` documenting the rationale (the unpair site already had the v43/W29 comment).

### v3.4.4-forceclose.69 (2026-05-07) — W44 auto-close DST spring-forward / fall-back mitigation

**Verified applicable** at the gate: `autoCloseInWindow` and `autoCloseSecsUntilNextStart` both run against `localtime_r` (`comms.cpp:2935`/`2974`) → DST shifts move the comparison. SNTP's `time_is_set` callback fires only on initial sync / step, NOT on DST transitions (DST is a localtime view, not a clock event). So a one-shot `autoCloseTicker.once_ms(secs)` could sleep ~22 h waiting for next window-start; if a DST transition happens mid-sleep, the actual fire-time drifts by ±1 h relative to local-time intent.

**Fix (option-A from v45 plan)**: cap the long-sleep horizon at 30 min in `autoCloseSecsUntilNextStart`. The scheduler now chains: each 30-min wake-up re-runs `update_auto_close_schedule` → fresh `localtime_r` → either another 30-min cap or transition to 60 s in-window tick. Worst-case DST drift bounded to 30 min instead of the previous ~23 h. Log line at the schedule call updated to `(W44-capped, will re-evaluate)` so the cap isn't surprising in syslog tails.

Trade-off: ~46x more wake-ups during the inactive window (e.g. a 22:00→06:00 window with current time 12:00 used to sleep ~10 h once; now wakes 20 times at 30 min cadence). Each wake is a single `localtime_r` + cache read + log line — negligible CPU/battery impact. Worth it to cap DST drift.

### v3.4.4-forceclose.68 (2026-05-07) — W43 writeBuffer rename + W48 _C field audit (doc-only)

**W43 — `writeBuffer` rename + invariant comment.** Hygiene rename: the file-scope `writeBuffer[512]` in `web.cpp` is renamed to `loopTaskScratchBuf512` so every call site advertises the loopTask-only invariant up front. A comment block at the declaration documents (a) the ESP32 invariant — only written from loopTask context (Arduino WebServer dispatch, OTA upload, web_loop status path); (b) the ESP8266 carve-out — `SSEBroadcastState` reuses this global on the 8266 because the ~4 KB main-task stack can't absorb +512 B per call. Per-caller stack buffers were rejected during planning for that reason.

**W48 — `_C` change-tracked vs raw `JSON_ADD_*` field consistency audit.** Inventoried 27 fields in `web_loop`'s SSE delta-broadcaster. **Conclusion A**: existing split is deliberate — `_C` for per-tick polled fields with a per-field cache slot in `last_reported_*`, raw for fields gated by single-shot event flags (the flag IS the cache). Doc comment block added above the SSE broadcaster articulating the rule + sibling-path warning that `build_status_json` (polled snapshot) uses RAW for ALL fields by design (full-snapshot consumer contract). **Knock-on**: W40 closes as non-finding — the 11 fork-added fields W40 cited are in `build_status_json` where NO field uses `_C`, so W40's "drift from a `_C`-elsewhere convention" premise doesn't hold up. The bandwidth concern is a property of the polled-snapshot architecture, not the specific fields; reopening would require redesigning the `/status.json` contract entirely (out of scope).

Zero behavior change for both items.

### v3.4.4-forceclose.67 (2026-05-07) — log-audit-002 hkConsecutiveHealthyTicks always-on

Cosmetic / observability fix. The `hkConsecutiveHealthyTicks` counter (reported in the periodic `diag-hk` log line) was nested inside the `else if (hkRecoverAttempts > 0)` branch of the HomeKit watchdog, so with auto-recover disabled (default — `hkAutoRecover=false`) the counter never moved. Field syslog showed `hkHealthyTicks=0` on 110 consecutive diag lines over ~5 h despite `controllers=4 paired=yes wifi=connected` and observed iOS reads — falsely suggesting an unhealthy HomeKit when everything was fine.

**Fix**

Hoist the increment+reset out of the recover-counter-clear branch — runs unconditionally on every tick now. Counter type bumped `uint8_t → uint32_t` to avoid wrap-to-zero on long healthy uptimes (uint8_t at 60s tick = 4.25 h overflow). The recover-counter clear logic still gates on `hkRecoverAttempts > 0`, but the streak is always observable. No behavior change for actual auto-recover semantics.

### v3.4.4-forceclose.66 (2026-05-07) — log-audit-004 SSE write rewrite

Fixes the recurring `errno 11 fail on fd 51/52 "No more processes"` syslog noise AND a previously-undiagnosed silent-broadcast bug. Previous attribution to "fd exhaustion / browser fan-out" (log-audit-002 / PR #77) was a misread — fd 51/52 are long-lived SSE TCP sockets whose `LWIP_SOCKET_OFFSET=50` puts them at the bottom of the fd range, not the top.

**Two bugs in the SSE clientWriteEx path**

1. **Silent-broadcast bug.** `clientWriteEx`'s v24 fast-path checked `client.availableForWrite() < len` to skip writes when the TCP send buffer was full. But Arduino-ESP32's `NetworkClient` does NOT override `Print::availableForWrite()` — the inherited default returns 0. So the fast-path returned `BUFFER_FULL` on every call without ever calling `client.write`. Normal-size status broadcasts (status JSON < 512 B) silently failed, and v47's wedge sweep eventually reaped the slot at the 30-consecutive-BUFFER_FULL threshold. The web UI's status panel was effectively running on heartbeat-only (the heartbeat path's small payload uses a different code path). ESP8266's WiFiClient DOES override `availableForWrite` (queries `tcp_sndbuf`), so the bug was ESP32-only.

2. **Framework log_e flood.** Oversized broadcasts (LOG_MESSAGE > ~490 B, RATGDO_STATUS > ~490 B — `jsonPeak` measured at 2312 B in field syslog) bypass the writeBuffer and call `subscription[i].client.printf(...)`, which enters Arduino-ESP32's `NetworkClient::write` send-retry loop. That loop logs `ESP_LOGE("fail on fd %d, errno: %d, \"%s\"")` UNCONDITIONALLY when `lwip_send(MSG_DONTWAIT)` returns -1 with `EAGAIN` (TCP send buffer full — benign flow control on a slow peer), up to `WIFI_CLIENT_MAX_WRITE_RETRY=10` log lines per write. errno 11 = EAGAIN; "No more processes" is just newlib's strerror text for it.

**Fix**

ESP32-only rewrite of `clientWriteEx` and the two oversized-payload paths in `SSEBroadcastState`:

- `clientWriteEx` now calls `lwip_send(client.fd(), data, len, MSG_DONTWAIT)` directly. EAGAIN at start → clean `BUFFER_FULL` (no log_e). EAGAIN mid-payload within the 200 ms slow-write budget → small `vTaskDelay(2ms)` + retry. EAGAIN mid-payload beyond budget → `client.stop()` + `FAILED` (cleaner than corrupting the SSE event stream). Other errno values → single `ESP_LOGW` + stop.
- Oversized LOG_MESSAGE / RATGDO_STATUS broadcasts route through a heap-malloc'd buffer + `clientWriteEx` instead of `client.printf`, so they share the same direct-lwip_send code path.
- ESP8266 paths unchanged.

**Net effect**

- `errno 11 fail on fd N` syslog noise eliminated on ESP32 (all SSE writes now bypass the framework's noisy retry loop). Real socket errors (ECONNRESET / EPIPE / ENOTCONN) still surface as a single `ESP_LOGW` per occurrence.
- Status broadcasts that fit in the 512 B writeBuffer now actually reach subscribers; v47's wedge sweep no longer fires from the broken fast-path's deterministic BUFFER_FULL stream.

**Heap budget**

Oversized broadcasts now `malloc(needed+1)` (~2.4 KB at observed `jsonPeak`) per broadcast, used briefly then `free()`d. Allocations of this size are clean against typical post-boot heap (~50 KB+); short-lived nature minimizes fragmentation risk.

### v3.4.4-forceclose.57 (2026-05-06)

Hotfix for v56 regression. v56 switched the repo from the auto-managed `pages-build-deployment` workflow to a custom `.github/workflows/pages.yml`. That worked for source-only commits (Pages correctly skipped them per the `paths: ['docs/**']` filter) but broke the actual release path: `release.yml`'s combined commit (which DOES touch `docs/`) didn't trigger Pages.

**Root cause**

GitHub's documented anti-recursion rule: pushes made with `GITHUB_TOKEN` do NOT trigger other workflows. The legacy `pages-build-deployment` workflow was special-cased to ignore that rule, so it deployed every push regardless of token. The custom `pages.yml` is NOT special-cased — `release.yml`'s GITHUB_TOKEN push of v56 firmware bins landed on `main` but Pages didn't rebuild → device-side OTA fetched stale bins from Pages → MD5 mismatch on every v56 OTA attempt.

**Fix**

`release.yml` now dispatches `pages.yml` explicitly via `gh workflow run pages.yml --repo $GITHUB_REPOSITORY --ref main` immediately after the combined commit pushes. `workflow_dispatch` events ARE allowed under `GITHUB_TOKEN`. Job-level `permissions: actions: write` added so the gh CLI call succeeds.

The dispatch is gated on a new `FW_BINS_PUSHED` env var that's only set when the combined commit actually pushed (skipped when re-running a release on already-uploaded bins to avoid redundant deploys).

**Net effect**

Future releases: 1 Pages run per release cycle, fired explicitly after `release.yml` lands the combined commit. Source-only commits, hook commits, README edits → 0 Pages runs (v56's paths filter still in effect).

### v3.4.4-forceclose.56 (2026-05-06)

Workflow optimization (round 2). Replaces GitHub's auto-managed `pages-build-deployment` workflow with a custom `pages.yml` that has a `paths: ['docs/**']` filter — Pages now only rebuilds when files Pages actually serves (the `docs/` tree) actually change.

**Why**

v48 cut release.yml from 2 commits to 1 per release, which halved the cancellation churn. But the auto-managed Pages workflow still fires on EVERY push to `main` regardless of paths — so direct hook commits, source-only PRs, README tweaks, and the auto-release PR mechanics all triggered redundant Pages runs that deployed identical content. Observed during the v54/v55 releases: 6 cancellations across 12 commits in a 10-min window because Pages couldn't distinguish "docs/ changed" from "any other file changed."

**Fix**

New file `.github/workflows/pages.yml`:
- Triggers on `push` to `main` only when `paths: ['docs/**']` matches.
- `workflow_dispatch` retained for manual re-deploy.
- Same `concurrency: pages, cancel-in-progress: true` as the auto-managed workflow — back-to-back commits to `docs/` still dedupe.
- Standard `actions/configure-pages@v5` + `actions/upload-pages-artifact@v3` + `actions/deploy-pages@v4` chain.

After the PR merges, the repo's Pages `build_type` is flipped from `legacy` (auto-managed branch deployment) to `workflow` (custom workflow-driven) via `gh api -X PUT repos/Haglerd/homekit-ratgdo32/pages -f build_type=workflow` — done as a post-merge step in the same release motion, no manual UI click needed.

**Effect**

- Hook commits, source-only PRs, README edits → **0 Pages runs**
- Release.yml's combined commit (touches `docs/manifest.json` + `docs/firmware/*`) → 1 Pages run
- Auto-release PR mechanics (merge commit doesn't touch `docs/`) → 0 Pages runs

Net: from ~3-5 Pages runs per release cycle down to 1.

### v3.4.4-forceclose.55 (2026-05-06)

Fixes a deterministic crash during OTA firmware update. Verified via the v52 crash log on the user's device: panic at 80% upload progress, `LoadProhibited` exception in `esp_timer` task. addr2line resolved the backtrace to `homekit_health_log()` at `src/homekit.cpp:679` calling `uxTaskGetStackHighWaterMark()` on a stale task handle.

**Mechanism.** `helperUpdateUnderway` calls "Shutdown HomeKit and GDO communications" at OTA start, which tears down HomeSpan tasks (freeing the `autoPoll` task's TCB). The `homekit_health_log` diagnostic Ticker fires every 3 minutes on a separate `esp_timer` callback. If it fires mid-OTA — which it CAN because that Ticker isn't gated by the `suspend_service_loop` flag that protects most other drains (v43's W20 fix) — `homeSpan.getAutoPollTask()` returns a stale pointer to the now-freed TCB. `uxTaskGetStackHighWaterMark(stale_handle)` dereferences it → panic.

The crash is timing-dependent. Probability rises with OTA duration: a slow upload (Tailscale relay, weak WiFi, large firmware) is more likely to span the 3-minute Ticker boundary. User's v52 crash hit it at 80%; second OTA attempt completed cleanly because the 3-minute Ticker boundary didn't fall during the upload window. **Both successful AND failed OTA attempts in production used to be possible — this fix makes ALL OTA attempts safe.**

**Fix.** Bail early in `homekit_health_log()` (`homekit.cpp:610`) if `firmware_update_in_progress()` returns true. Same defense-in-depth pattern audit W20 used for `service_timer_loop` drains (v43). Health logging naturally resumes after the post-OTA reboot.

**Compatibility.** No protected surfaces touched. v22-v54 SSE infrastructure unchanged. v47 keepalive + class 5d untouched. The `firmware_update_in_progress()` predicate already existed (web.cpp:290) and was already used elsewhere in `homekit.cpp:777`. Adding one more callsite. Zero new state, zero new race surface.

**Worth filing upstream.** This bug exists in upstream too — the `homekit_health_log` Ticker is fork-only (added by Haglerd/v22+), but the underlying race pattern (Ticker callback firing on freed task state during shutdown) is generic. The fix is upstream-mergeable as a defensive guard.

### v3.4.4-forceclose.54 (2026-05-06)

Fixes the underlying "first prompt rejected, second prompt accepted" + "logs.html prompts more often than settings" UX issues. **Root cause was identified:** arduino-esp32's Digest auth implementation issues a fresh nonce per response and does NOT set `stale=true` on stale-nonce 401 responses. When a browser sends an Authorization header with a cached (now-stale) nonce, the device responds 401 with a fresh challenge. Per WHATWG Digest spec, a server should set `stale=true` to tell the browser "your password is correct, just retry with this new nonce silently." Without that flag, browsers treat the 401 as wrong-credentials and re-prompt the user. Symptom: every 3-second `/showlog` polling fetch could trigger a re-prompt, so /logs.html became prompt-heavy compared to /settings (which only auths once per click).

**Fix:** new macro `AUTHENTICATE_OR_ALLOWLIST()` (`web.cpp` line ~921) that checks the v37/v39 per-IP recent-auth allowlist FIRST. If the client's IP is already allowlisted (was stamped by `/auth` or any other AUTHENTICATE'd endpoint within the last 15 minutes per v52), the request is allowed WITHOUT running Digest. Falls back to full `AUTHENTICATE()` if not allowlisted (first visit, post-reboot, or after 15-min idle). Applied ONLY to read-only polling endpoints — `/showlog`, `/showrebootlog`, `/crashlog`. State-changing endpoints (`/setgdo`, `/reboot`, `/reconnectHomeKit`, `/reset`, etc.) keep full `AUTHENTICATE()` for max security.

**User-visible effect:**
- First page load of `/logs.html`: Digest prompt fires once via the `/auth` call (v53 design unchanged).
- Subsequent 3-second `/showlog` polls within 15 min: allowlisted, **no Digest, no prompt**.
- After 15-min idle: allowlist expires, next request prompts again.
- Same allowlist powers SSE subscribe (v37/v39), now also powers read-only polling — single source of truth.

**Security model:** identical to the existing SSE allowlist gate. Per-IP. Same-IP attacker (NAT, shared LAN) gets access — but they already could via cached browser Digest creds + replay. The allowlist isn't weaker than the status quo; it's just less chatty about repeated re-auth.

**Compatibility:** v22-v53 SSE infrastructure preserved unchanged. State-changing endpoints unchanged. v52 15-min `AUTH_ALLOWLIST_TTL_MS` tuning interacts cleanly with this change. Upstream-mergeable as a quality-of-life improvement (upstream has the same Digest stale-nonce issue inherited from arduino-esp32; the fix doesn't depend on any v22+ fork-only infrastructure beyond the allowlist itself, which IS fork-only).

### v3.4.4-forceclose.53 (2026-05-06)

Hotfix for v52. v52 dropped the SSE setup AND the leading `/auth` fetch from `logs.js`. SSE removal was correct. But removing `/auth` was a mistake — `loadLogPages` fires THREE auth-required fetches in parallel (`/showlog`, `/showrebootlog`, `/crashlog`) via `Promise.allSettled`, and browsers handle three concurrent `401 + WWW-Authenticate: Digest` responses inconsistently. Most either lose the prompt or cycle through it and reject the user's credentials.

Symptom: direct navigation to `/logs.html` shows the auth dialog but rejects credentials. Workaround: visit `/settings` first (which calls `/auth` cleanly via `checkAuth()`), then navigate to `/logs.html` — the cached creds replay through the parallel fetches without confusion.

**Fix:** restore the single `/auth` fetch at the top of `loadLogs()` before `loadLogPages()`. One auth request → one browser prompt → cached creds → parallel fetches replay cleanly. Matches the working `/settings` flow byte-for-byte.

**No regressions reintroduced:** v46 localStorage skip-auth (caused dead-page-after-reboot) — still NOT in v53. v50/v51 SSE error cascade (caused repeated `/auth` calls flashing the spinner) — still NOT possible (no SSE in logs.js since v52). The `/auth` call fires exactly once per page load.

### v3.4.4-forceclose.52 (2026-05-06)

Two cleanups based on production observation of v51:

**Logs UI: drop SSE entirely, pure polling.** v51's polling fallback was layered ON TOP of SSE — but the interaction was buggy. Every SSE error triggered `loadLogs()` which set the spinner visible and re-ran `loadLogPages()`, which `insertAdjacentText('afterbegin', ...)` PREPENDED the entire /showlog buffer to the existing content (duplicates). Polling's next tick then saw text that didn't start with `lastShowlogContent`, treated it as a buffer-wrap, CLEARED both panes and repopulated — the "logs flicker / disappear briefly" symptom the user reported. User's syslog showed the cascade plainly: 4-6 SSE-orphan reaps for the same UUID within seconds.

v52 deletes the SSE setup from `logs.js` entirely. Pure polling at the v51 3-second cadence. Removes: the `/auth` warmup hop (no longer needed; /showlog uses standard browser-cached Digest, not the per-IP allowlist), the `/rest/events/subscribe` fetch, the EventSource setup, the `logger` event listener, the close-and-resubscribe error handler. Result: zero SSE-reconnect cascades, zero PREPEND duplication, zero pane-clear flicker. Page is "live enough" at 3-second polling — close to instant for door state changes, plenty for log viewing.

Home page (`functions.js`) STAYS on SSE — its local 1-Hz uptime ticker (v51) is the primary live indicator and the SSE event volume on the home page is small (no log-line broadcasts), so the wedge pattern doesn't fire there.

**Allowlist TTL: 5 min → 15 min.** Bumps `AUTH_ALLOWLIST_TTL_MS` in `web.cpp` from 5 minutes to 15 minutes. Reduces re-auth pressure on the home page's SSE subscribe — users idle on the home page for 10 minutes won't have to re-Digest. Security trade-off is minor: an attacker on a different LAN IP still can't read SSE without first authing from THEIR IP, which our enforce_same_origin / Digest gate already blocks. The allowlist TTL is purely a UX cushion for the legitimate user.

Compatibility unchanged. v22-v51 SSE infrastructure still in place server-side (just no longer used by logs.js). functions.js untouched.

### v3.4.4-forceclose.51 (2026-05-06)

Stops depending on SSE for "the page is live." Three independent layers — each one alone keeps the UI advancing — so even if SSE wedges (which it does on slow-draining browser tabs / homebridge poll storms / various network conditions), the page never goes stale.

**Layer 1 — local 1-Hz uptime ticker (functions.js).** The home page's uptime span now counts up locally every second based on the device-reported `upTime` value as a baseline. Each authoritative status update (from SSE OR the initial `/status.json` fetch) re-anchors the counter to the device's value, bounding drift to "between updates." Result: the user sees the counter advance every second from the moment the page loads, regardless of SSE state. Pre-v51 the counter was frozen at the last SSE-delivered value, which on a wedged connection meant frozen-until-refresh.

**Layer 2 — `/showlog` polling fallback (logs.js).** Every 3 seconds, fetch `/showlog` and append any content the user hasn't seen yet. Diffs the full buffer text against `lastShowlogContent` and inserts only the suffix that's actually new. Pauses while the tab is hidden (Page Visibility API) — browsers throttle hidden tabs anyway, no point burning device WebServer ticks for invisible content. If SSE delivers events live (best case), the diff is mostly idempotent. If SSE is wedged (the common failure mode mechanism 2 was reaping), this is what makes the page feel live. 3-second cadence matches "feels live" without flooding `/showlog` (auth'd, serialised through the WebServer task).

**Layer 3 — explicit close + re-subscribe on SSE error (both files).** v50 tried to drop this in favour of pure spec-compliant auto-reconnect, but EventSource auto-reconnect only retries the channel-specific URL — and after a sweep reap (5b/5c/5d) the device returns 404 to that URL, which browsers treat as permanent failure. v51 restores explicit `close() + setTimeout(loadLogs, 1000)` (logs.js) and `close() + setTimeout(checkStatus, 1000)` (functions.js) so a sweep reap triggers a full re-subscribe via the `/rest/events/subscribe` → new EventSource flow. Persistent UUID in localStorage (v27) means `handle_subscribe`'s `foundExisting` branch reuses the slot when possible.

**What stays from v50.** Server-side `retry: 3000\n\n` SSE field in `SSEHandler` — still useful for the cases where EventSource's native auto-reconnect IS appropriate (transient TCP blip, channel slot still alive). Pins reconnect to 3 seconds, overrides Chrome's exponential backoff.

**Compatibility.** All v22-v49 SSE infrastructure preserved unchanged: v22 deferred-cleanup, v24 SO_SNDTIMEO, v27 persistent UUID + heartbeat=10, v29 tri-state SseWriteResult, v37/v39 auth + per-IP allowlist, v46 force-close LOGI, v47 keepalive + class 5d reap, v49 status.json LOGV demotion. The 30s `checkHeartbeat` watchdog at `functions.js:907-913` also unchanged.

**Trade-off.** Layer 2 adds one /showlog GET every 3 seconds while the logs page is open and visible. /showlog returns the on-device 16KB buffer (~10-15KB after gzip-content-encoding negotiation). On the WebServer task (~100ms turnaround per /showlog) this is well within capacity — homebridge's existing 1Hz /status.json poll is the dominant non-event traffic, and that's been working fine. Net cost is ~5KB/s upstream while a logs page is open. Acceptable for "always live" UX.

**Lesson logged.** SSE alone is too fragile for "the page must be live" UX on a constrained device with variable browser-side draining. Production HTTP status pages always layer polling under SSE for exactly this reason. The fork's design before v51 assumed SSE would always work; it doesn't. Architectural fix is complete now.

### v3.4.4-forceclose.50 (2026-05-06)

Fixes the long-standing "stale-UI until manual page refresh" bug that affects both `/logs.html` (live log streaming + HomeKit tab) and `/index.html` (status counter not advancing). **The bug predates v47 — it predates the entire fork's v22+ SSE work.** Origin is upstream commit `4e3063d` (Aug 2025), which introduced an `evtSource.close()` call in the EventSource error handler that permanently transitions readyState to CLOSED and defeats the browser's spec'd auto-reconnect (WHATWG SSE §9.2.6). Once any single transient TCP error fired, the page died for SSE until F5. Investigation cross-referenced against `/var/log/ratgdo.log` on the syslog Pi confirmed the "TCP dropped" reaps every 30-50s were paired AT THE SAME millisecond with `Sending 304 not modified .../logs.html` requests — those reaps were the CONSEQUENCE of the user pressing F5 to recover, not the CAUSE.

**Three surgical changes:**

- `src/www/logs.js:185-189` — removed `evtSource.close()` from the error handler. Browser auto-reconnects per spec.
- `src/www/functions.js:933-938` — same removal (the explicit `setTimeout(checkStatus, 5000)` was re-implementing what EventSource does natively, less robustly). The separate 30-second `checkHeartbeat` watchdog at `functions.js:907-913` is preserved unchanged — it correctly handles "connection looks alive but device sent nothing" which is a different failure mode.
- `src/web.cpp:2447-2448` — added `retry: 3000\n\n` SSE field to the handshake. Overrides browser-default exponential backoff (Chrome escalates 3s → 6s → 12s → 24s+ after repeated failures), pinning reconnect interval at 3s for predictable recovery. 3000 ms matches Mercure / sse-pubsub / nginx push-stream defaults.

**Compatibility:** v47's per-socket TCP keepalive STAYS (correctly catches truly-dead peers — was not the cause of this bug). v47's mechanism 2 (sweep class 5d / consecutive-BUFFER_FULL reap) STAYS — verified working in production. v24's SO_SNDTIMEO STAYS. The persistent UUID in localStorage (v27) makes the auto-reconnect path reuse the same SSE slot via handle_subscribe's foundExisting branch (web.cpp:2632-2651). All v22-v49 SSE infrastructure is untouched.

**Upstream-compatible:** this fix improves upstream's behavior too. Could be PR'd back to upstream as a bug fix — recommend doing so.

### v3.4.4-forceclose.49 (2026-05-06)

Two logs UX hotfixes for issues observed post-v47/v48 deploy.

**Logs UX — remove the v46 localStorage auth-skip (the auth trap).** v46's `logs.js` cached a 4-min localStorage timestamp and skipped `/auth` if it was warm, intending to avoid re-prompts on refresh. The hole: when the device reboots, the per-IP allowlist clears, but the browser's localStorage timestamp doesn't know. Refresh after reboot → page skips `/auth` → SSE-subscribe gets 401 from the now-empty allowlist → no recovery code path → user sees prompt-then-rejected loop with no working logs (and clearing localStorage is the only manual recovery). v49 reverts to "always call `/auth` on page load" (the pre-v46 behavior) and explicitly clears any stale `ratgdo-logs-last-auth-at` localStorage entry on every page load so existing users don't have to manually purge it. Cost: an occasional Digest prompt on refresh when the browser's auth cache lapses. Benefit: no more dead-page-after-reboot trap.

**Force-close logs visible at DEBUG — `/status.json` poll demoted to ESP_LOGV.** v46/v47 promoted force-close to ESP_LOGI but they were still being wrapped out of the 16KB on-device ring buffer because of the `/status.json` poll noise. Homebridge polls `/status.json` at 1Hz steady-state and 1.5Hz during force-close, each poll producing two debug lines (`Client X requesting: /status.json` + `JSON status: ... build time ...`). At ~120 lines/min that's half the buffer per minute — a 16-second post-force-close fetch could miss the lines (verified against `/var/log/ratgdo.log` on the syslog Pi: force-close fired at uptime 00:04:24, on-device buffer captured starting at 00:04:34, the lines fell into the wrap window). v49 demotes both lines from `ESP_LOGD` to `ESP_LOGV` (verbose, level 5). At DEBUG (level 4) the poll noise is now invisible while every other request log stays at LOGD (`/setgdo`, `/reboot`, `/reconnectHomeKit`, `/showlog`, etc. — fired on user actions and exactly what you want to see). The `/rest/events/subscribe` and `/rest/events/unsubscribe` endpoints get the same treatment (also high-frequency from the SSE re-subscribe cycle when `mechanism 2` reaps a wedged subscriber). VERBOSE level still shows everything for deep debugging.

**Out of scope for v49**: live-log SSE wedging (browser tab keeps wedging the SSE TCP buffer to `have 0` even after v47's class-5d reap; mechanism 2 keeps re-reaping but the underlying browser-side drain issue isn't fixed). Will scope as a separate v50 investigation.

### v3.4.4-forceclose.48 (2026-05-05)

Workflow optimization. Halves the user-visible Pages-publish wait per release by combining the two `release.yml` commits to `main` into a single commit.

**Why**

The auto-managed `pages-build-deployment` workflow has built-in `cancel-in-progress` concurrency — every push to `main` cancels the in-flight Pages build and starts a new one. Pre-v48 each release pushed twice to `main`:

1. Early commit: `Update manifest.json for vXX` (before the firmware build)
2. Late commit: `Upload firmware bins for vXX` (after the build, ~4 min later)

Plus the original PR-merge commit. So per release: PR merge → Pages #1 starts → manifest commit → Pages #1 cancelled, #2 starts → bins commit ~4 min later → Pages #2 cancelled, #3 starts → finally completes. User-visible wait: ~8 min for the bins to actually be live on Pages.

**Fix**

`release.yml` now does a single combined commit at the end of the workflow:
- New `Stash updated manifest.json for combined commit` step (replaces the early `Commit updated manifest.json` step) — saves the action-json'd manifest to `/tmp` without committing or pushing.
- The existing firmware-bins commit step is renamed `Commit manifest.json + firmware bins to docs/` and now restores `manifest.json` from `/tmp`, stages it alongside the four flash bins, and commits both with the message `Release vXX: manifest + firmware bins`. Single push, single Pages build, no cancellation.

**Side benefit**

If `pio run` fails mid-release, `manifest.json` no longer lands on `main` pointing at non-existent bins. Pre-v48 the early manifest commit could leave a broken manifest if the build subsequently failed. v48 makes manifest+bins atomic w.r.t. `main`.

**Impact**

- Pages publish wait: ~8 min → ~4 min per release.
- Net commits to `main` per release: 2 → 1 (excluding the original PR-merge commit which is unavoidable).
- No change to release-attachment behavior, OTA paths, or `docs/firmware/` contents.

### v3.4.4-forceclose.47 (2026-05-05)

Industry-standard SSE backpressure hardening + the v46 logs UX fixes shipped together. Two SSE mechanisms (this release) layered on top of the v22+ orphan-sweep / `pendingRemove` / tri-state-`clientWriteEx` subsystem. ESP32-only (matches v24 SO_SNDTIMEO gating).

**TCP keepalive on SSE sockets.** `web.cpp::SSEHandler` now arms per-socket TCP keepalive on every SSE TCP connection: `SO_KEEPALIVE=1` + `TCP_KEEPIDLE=30s` + `TCP_KEEPINTVL=10s` + `TCP_KEEPCNT=3`. Worst-case 60s for the kernel to detect a silently-dropped peer (laptop lid, switch power-down, NAT-table flush) — 5x faster than the existing 300s `SSE_IDLE_TIMEOUT_MS` 5c sweep. lwIP flips `WiFiClient::connected()` to `false` once `KEEPCNT` probes fail; sweep class 5b reaps on the next service tick. Industry-standard pattern (nginx SSE / Mercure / sse-pubsub libraries). `CONFIG_LWIP_TCP_KEEPALIVE=y` added to `sdkconfig.defaults` explicitly even though IDF v5 defaults it on — self-documenting + immune to future IDF default flips. Per-socket `SO_KEEPALIVE` remains OFF by default; the four `setsockopt` calls in `SSEHandler` enable it.

**Per-slot consecutive `BUFFER_FULL` threshold (sweep class 5d).** New `SSESubscription::consecutiveBufferFull` field (volatile uint32_t, atomic). Application-layer wedged-subscriber detection for the case where the peer TCP is alive (so keepalive doesn't fire) but the application stops draining — browser tab Ctrl-Z, mobile tab in background, etc. Stamped at the four `BUFFER_FULL` callsites (`SSEheartbeat`, two `SSEBroadcastState` branches, `handle_firmware_upload`): `+1` on `BUFFER_FULL`, reset to `0` on `OK`. New sweep class 5d (between existing 5b and 5c in `sweep_sse_orphans`) reaps the slot when the counter exceeds `SSE_MAX_CONSECUTIVE_BUFFER_FULL = 30` — ~30s real-time at typical 1-event/s broadcast cadence, 10x faster than 5c's 300s. OTA exception: skip the `firmwareUpdateSub` slot. Slow-link upload tails legitimately hit `BUFFER_FULL` during a chunk; the existing OTA end/abort paths clear `firmwareUpdateSub`, restoring normal lifecycle.

**v46 fixes carried in this release** (originally planned as a separate v46 release; rolled into v47 since they ship together):

- Force-close `ESP_LOGD` → `ESP_LOGI` on per-attempt lines (`comms.cpp:2750/2766/2782` — "attempt N release sent", "scheduling attempt N+1", "attempt N/2 press hold=Xms"). Reverts v32's "MH2 sub-2 — force-close 6 INFO → 3" cleanup that demoted these as "routine state". Force-close is a momentary user-triggered action, not routine state; per-attempt visibility matters when verifying the hold-to-close override fired correctly. Force-close press attempts visible at default INFO again.
- SSE `clientWrite` buffer-full `ESP_LOGD` rate-limited to once-per-60s (`web.cpp:362-381`). Verified against syslog: 260 skip lines per day (~7% of all log volume), concentrated in bursts when an SSE subscriber is wedged — that volume dominated the 16KB on-device log buffer and wrapped out short-lived user-action lines (force-close sequences, auto-close fires) before `/showlog` could capture them. The cumulative `sseBufferFullSkips` counter still increments on every skip; the log line just samples once per minute with a `[N total skips]` suffix. v47's mechanism 2 makes this less critical (the wedged subscribers causing the noise now get reaped) but the rate-limit stays as belt-and-suspenders for transient Tailscale-style backpressure.
- `logs.js::loadLogs()` skips `/auth` when the per-IP recent-auth allowlist is still warm (4-min `localStorage` timestamp window — kept inside the device's 5-min `AUTH_ALLOWLIST_TTL_MS`). Eliminates re-prompt on refresh for browsers that don't persist Digest credentials (Chrome's modern privacy default, Safari with aggressive cache-clearing, etc.). After the 4-min window the `/auth` call fires normally; on 401 the cached timestamp is cleared.

**Out of scope (still deferred)**: W25 (`web_loop` rate limit, needs soak data), boot-time heap → `tiT` mDNS-OOM (architectural).

### v3.4.4-forceclose.44 (2026-05-05)

Discipline hardening — closes the last v38-round-2 audit nit. **W9** added `volatile` to the two cross-task scalars in `comms.cpp:2640-2641` (`forceCloseAttempt`, `forceCloseHoldMsCached`). Both are written and read across loopTask + esp_timer task boundaries; the surrounding `__atomic_*` flag barriers (`forceCloseInProgress`, `forceCloseGapPendingArmMs`, `forceCloseClearPending`) already provide happens-before edges so this is a discipline gap, not a runtime bug. The fix restores consistency with the other 11 cross-task scalars in `comms.cpp` that already carry `volatile`, and forbids future cross-call load hoisting if `-flto` is ever re-enabled. Zero codegen change on the current `-Os` (no LTO) toolchain. See audit-notes W9 for full mechanism.

**Out of scope (still deferred)**

- Boot-time heap exhaustion → `tiT` mDNS-OOM — architectural; needs boot-time-allocator profiling traces from a workbench.
- W25 (`web_loop` 10/sec rate limit) — needs soak data on burst-reconnect storm; no field evidence the gate is masking a real issue today.

### v3.4.4-forceclose.43 (2026-05-05)

Audit cleanup pass — closes 17 of the 19 round-3 findings from the audit-notes "v39 round-3 findings (still open)" section (`audit-notes/2026-05-04-fork-vs-upstream-attribution.md`). Most are Nit severity; W20 / W32 / W36 are the only items with any runtime behavior implications. Builds on v42's cache-bust fix so the W28 `logs.js` regex change actually reaches deployed devices.

**Important fixes (cross-task discipline)**

- **W20 — `service_timer_loop` deferred ALL drains during OTA.** During a 30-second OTA upload `suspend_service_loop=true`, gating nine drains in `ratgdo.cpp`. Two of them — `sweep_sse_orphans()` + `process_sse_pending_removes()` — only touch SSE slots OTHER than the active firmware-update slot (which is stamped on every successful chunk write at `web.cpp:2955` so the orphan sweep never targets it). Pre-v43 a wedged SSE subscriber could hold its socket for the full upload duration. v43 hoists the SSE drain pair ABOVE the `if (suspend_service_loop) return;` gate at `ratgdo.cpp:388-389`. Force-close / auto-close drains stay below — those are cheaper to delay than holding wedged sockets.
- **W32 — Auto-close re-checks door state immediately before fire.** `checkAutoClose` runs on esp_timer task; loopTask can transition the door out of CURR_OPEN between the Ticker tick's earlier state read and the `door_command_force_close(3500)` call. On Sec+1.0 a press on a now-CLOSED door TOGGLES → door re-opens unintentionally. v43 adds `if (garage_door.current_state != CURR_OPEN) return;` immediately before the press fire at `comms.cpp:3063`. Cheap re-check addresses the user-visible failure mode without a full atomic-discipline pass on `doorOpenedAtMillis` / `autoCloseFiredThisCycle` (audit doc explicitly preferred the cheap re-check; full atomic pass deferred).
- **W36 — `WiFi.disconnect(false)` no longer blocks ~100 ms.** arduino-esp32's 2-arg `WiFi.disconnect(eraseap=false, wifioff=false)` defaults `timeoutLength=100` — the call blocks up to 100 ms waiting for the SYSTEM_EVENT_STA_DISCONNECTED event before returning. The v34 F7 split-stage comment claimed "~0 ms"; the actual block was ~100 ms. v43 passes `(false, false, 0)` explicitly to make the call fire-and-forget. Stage 2 still drives the re-associate at ≥250 ms via `homekit_drain_pending_reconnect_stage2`. Comment at `homekit.cpp:976` updated to reflect the actual semantics.

**Nit fixes (portability + cleanup)**

- **W18 — `xTaskGetHandle("Tmr Svc")` looked up the wrong task.** `homekit_health_log` is invoked from `Ticker.attach_ms`, which on arduino-esp32 dispatches via the `esp_timer` task (NOT the FreeRTOS Tmr Svc daemon). The `tmrHWM=` line in the diag log was reporting the HWM of the unrelated Tmr Svc daemon (which has minimal traffic on arduino-esp32) instead of the timer task we're actually running on. v43 uses `xTaskGetCurrentTaskHandle()` at `homekit.cpp:666`. Variable name + log key kept for grep compatibility.
- **W21 — log.cpp recursion table now ESP8266-gated.** The F3 8-slot per-task recursion guard added in v31.2 uses `TaskHandle_t`, `xTaskGetCurrentTaskHandle()`, and atomic-CAS on `volatile TaskHandle_t` — all FreeRTOS / ESP-IDF APIs unavailable on Arduino-ESP8266. Every other fork addition in `log.cpp` is `#ifndef ESP8266`-gated; this one was the outlier. v43 wraps the table in `#ifndef ESP8266 ... #else (broadcast directly) ... #endif`. ESP8266 cooperative scheduling can't recurse into itself in the way the ESP32 case guards against, so the else branch broadcasts/syslogs without a guard. Fork ships ESP32-only today, but if upstream ever revives the ESP8266 sibling, this is no longer a silent compile error.
- **W23 — `helperForceClose` redundant clamp dropped.** Both `helperForceClose` (web.cpp:1696-1702) and `door_command_force_close` (comms.cpp:2832-2833) clamped `hold_ms`, but with different bounds: the helper only caught `<= 0`, while comms enforced `< 1000 → 3500` and `> 10000 → 10000`. Behavior was correct (comms always won) but dual-validation invited future drift. v43 drops the helper-side clamp; comms is the single source of truth.
- **W24 — `handle_unsubscribe` Arduino String allocation per beacon.** Pre-v43 every `/rest/events/unsubscribe` POST allocated two heap Strings (one local, one for `server.arg(i)`'s return). `navigator.sendBeacon` fires this on every page navigation that had an EventSource open — heap fragmentation accumulates over weeks. v43 replaces with a `char uuid[40]` stack buffer + `subscription[i].clientUUID.equals(uuid)` comparison. Matches the v22+ migration of every other handler in this file.
- **W27 — Auto-close + watchdog constants centralized.** Pre-v43 the four scalar bounds (`AUTO_CLOSE_MAX_MINUTES=720`, `AUTO_CLOSE_MAX_TOD_MIN=1439`, `HK_WATCHDOG_MIN_SECS=60`, `HK_WATCHDOG_MAX_SECS=7200`) and two defaults (`AUTO_CLOSE_DEFAULT_START_MIN=1320`, `AUTO_CLOSE_DEFAULT_END_MIN=360`) were scattered across config.cpp / comms.cpp / web.cpp. Bumping any one bound required touching 3-4 files in lockstep. v43 adds six `constexpr uint32_t` to `config.h` and references them from every C++ site. Frontend (functions.js + index.html) intentionally stays literal — no codegen pipeline imports C++ constexpr — but a comment in config.h enumerates the matching frontend locations as the source-of-truth contract for human review.
- **W28 — `logs.js isHomeKitLine` regex simplified.** The regex `/ratgdo-homekit|HomeKit |HomeSpan|WiFi |Wifi |wifi |force-close to clear|HomeKit reconnect/i` had three issues: `WiFi |Wifi |wifi ` collapsed to one alternation under the `/i` flag, `HomeKit reconnect` was subsumed by `HomeKit `, and `force-close to clear` did not appear in any firmware ESP_LOG (dead pattern from a removed log line). v43 reduces to `/ratgdo-homekit|HomeKit |HomeSpan|WiFi /i` at `logs.js:294`. v42's CRC-substitution fix ensures device-deployed clients actually pick this up.
- **W30 — `Serial.print(".")` debug noise removed.** Three `Serial.print` calls in `handle_firmware_upload` (progress dot + two close-newlines) emitted unconditionally regardless of log level. The `ESP_LOGI` percentage line is the authoritative progress indicator. On a syslog-only deployment with no USB serial cable, each call still spent the per-call latency to the UART driver. v43 deletes the three `Serial.print` calls; ESP_LOGI lines remain.
- **W31 — Auto-close + force-close stubs for `-D USE_GDOLIB`.** Six fork-added functions (`update_auto_close_schedule`, `request_auto_close_reschedule`, `auto_close_drain_pending_reschedule`, `door_command_force_close`, `force_close_drain_pending_arm`, `force_close_drain_pending_clear`) were declared `extern void` in `comms.h` and called unconditionally from `setup_comms` / `web.cpp` / `utilities.cpp` / `ratgdo.cpp` — but defined only inside the `#ifndef USE_GDOLIB` block at `comms.cpp:2357-3067`. Enabling `-D USE_GDOLIB` produced six linker errors. The shipping build does not set `USE_GDOLIB` (commented out at `platformio.ini:60`) so this was latent. v43 adds an `#else` branch with no-op stubs for all six so the build flag matrix stays clean.
- **W34 — `mdnsDoorUpdateAt` time_t renamed to bool one-shot flag.** The `static time_t mdnsDoorUpdateAt = 0;` at `web.cpp:634` was set once when `lastDoorUpdateAt` first became non-zero, then never read except as the LHS of `!mdnsDoorUpdateAt`. The time_t type was misleading. Renamed to `static bool mdnsDoorUpdateInit = false;` with the same logic.
- **W35 — `/status.json` heap allocations every poll.** Four lines used `WiFi.macAddress().c_str()` / `WiFi.SSID().c_str()` / `WiFi.BSSIDstr().c_str()` patterns that returned borrowed pointers into Arduino String temporaries destroyed at the semicolon — undefined behavior even though `JSON_ADD_STR` happens to copy the bytes immediately. The `wifiRSSI` line additionally chained 3+ `std::string` allocations via `+`. Homebridge polls `/status.json` every 3 s and `SSEheartbeat` fires per-subscriber per-10s; over weeks the heap-fragmentation pressure accumulates. v43 copies each Arduino String into a stack buffer before passing the pointer (~120 B saved per poll) and rewrites the wifiRSSI concat as `snprintf` into a single stack buffer. `mdns_announce` at `web.cpp:1529-1544` has the same pattern but is rare and explicitly out of scope.
- **W37 — `matchHdr[8]` bumped to `[16]` defensive sizing.** Today's CRC32 ETag is 6 chars (urlsafe-b64 of 4 bytes, `=` padding stripped) so `[8]` fits, but is one byte from silent truncation if the encoding ever changes (full-base64 with `=` = 9 chars, MD5 hex = 32 chars). `[16]` comfortably accommodates any reasonable hash format. Zero runtime cost.
- **W38 — Stage-2 reconnect drain comment.** Documented that on the same tick that `homekit_force_reconnect` set `reconnectStage=1`, the stage-2 drain at `ratgdo.cpp:447` always bails — the elapsed-time gate at `homekit.cpp:1010` returns until ≥250 ms have passed.

**Comment-only fixes (intentional non-changes)**

- **W29 — `homekit_unpair()` synchronous from web request task — kept synchronous.** Inconsistent with the v31 deferred-flag pattern used by `homekit_dump_state` / `homekit_refresh_mdns` / `homekit_force_reconnect`, but `sync_and_restart()` reboots within ~500 ms of this call. Wrapping in a request-flag deferred wrapper would race the reboot path. v43 documents the rationale in a comment at `web.cpp:1117-1122`; behavior unchanged.
- **W33 — `handle_unsubscribe` security-comment hygiene.** The pre-v43 comment claimed "the UUID is the only authority required and an attacker without the UUID can't target a specific session." But UUIDs are emitted via `server.send_P` at `handle_subscribe` AND logged via `ESP_LOGD` at channel-allocation time; syslog readers and same-LAN sniffers can capture all active UUIDs and beacon-disconnect any user. v43 weakens the comment to acknowledge the actual threat model (DoS bounded to forcing SSE reconnects). Adding `AUTHENTICATE()` would break sendBeacon — sendBeacon doesn't implement Digest, this is the same v37 EventSource trap from a different angle. Behavior unchanged.

**Out of scope (still deferred)**

- **Boot-time heap exhaustion → `tiT` IllegalInstruction in mDNS receive.** Architectural. Carries forward from v37/v38/v39/v40/v41/v42. Needs boot-time-allocator profiling.
- **W9 — `forceCloseAttempt` and `forceCloseHoldMsCached` not `volatile`.** From v38 round-2. Still open. No observability under current `-Os` toolchain (`-flto` was reverted in v35); discipline gap, not a runtime bug.
- **W25 — `web_loop` 10-calls/sec rate limit.** Needs soak data on the burst-reconnect storm scenario the user has not exercised. Carry forward.

### v3.4.4-forceclose.42 (2026-05-05)

Build-pipeline hotfix (no firmware behavior change). `build_web_content.py` was computing each web asset's URL cache-bust hash from its on-disk source, before the `?v=CRC-32` placeholders inside the file were substituted with real per-file CRCs. So when a JS file changed but its parent HTML's source didn't, the parent HTML's URL hash + ETag stayed identical across builds — even though the served body now referenced a different child JS hash. Combined with `web.cpp` `CACHE_CONTROL = 30 days`, browsers held the stale parent HTML in disk cache for a month, never re-fetched, and kept loading the old JS by URL — masking the v41 `logs.js` fix entirely on already-deployed installs. v42 iterates the CRC computation post-substitution to a fixed point so any change in a referenced file propagates into the parent's cache key, busting browser cache reliably across firmware updates. See `audit-notes/2026-05-04-fork-vs-upstream-attribution.md` finding W39 for full mechanism + the cousin-bug check on `wifiap.html` (clean — no cross-file symbol references like the v40 `checkAuth` regression).

### v3.4.4-forceclose.41 (2026-05-05)

Hotfix for a v40 regression: the Logs UX fix called `checkAuth()` from `functions.js`, but `logs.html` only loads `logs.js` — not `functions.js`. ReferenceError, JS died, page stuck spinning. v41 inlines the equivalent `/auth` fetch directly in `logs.js:111-126` so no cross-file dependency. Same Digest-prompt-and-stamp-IP behavior, contained to logs.js.

### v3.4.4-forceclose.40 (2026-05-05)

Audit cleanup pass + Logs UX. Closes ten W11-W26 findings from the audit-notes "v36 post-closeout fresh-eye review" + a v39 follow-up UX fix in the device-UI Logs page.

**Critical fixes (HIGH severity)**

- **W11 — `hap_pair_cb` parameter shadowed `::isPaired` global.** The fork added this callback specifically to track live unpair-from-iOS events, but the parameter `boolean isPaired` shadowed the file-scope `static volatile bool isPaired` so the callback never updated the global. All 12 `notify_homekit_*` gates and `/status.json paired:` were stuck at the last `HS_PAIRED`-set value until reboot. Renamed param to `paired` + explicit `isPaired = paired;` assignment in `homekit.cpp:475-491`. The whole reason the fork added this callback is now actually working for the first time.
- **W12 — `WiFi.onEvent` re-registered on every `HS_WIFI_CONNECTING`.** Four event handlers were added to arduino-esp32's `NetworkEvents` list inside the status callback's `HS_WIFI_CONNECTING` branch — which fires on every WiFi connect/reconnect. `NetworkEvents` has no dedup; pre-v40 every WiFi flap appended four duplicate handler nodes (~24 B each, slow heap leak), and every WiFi event was logged 1x, 2x, 3x... times after each reconnect. v40 wraps the registrations in a `static bool wifiHandlersRegistered = false;` guard at `homekit.cpp:1078-1097`. Handlers persist across `WiFi.disconnect()`, so registering once is correct.
- **W14 — Force-close `forceCloseInProgress` leak in the press-1↔press-2 gap window.** During the 1500ms gap between press 1 release and press 2, `TTCtimer` is detached and `forceCloseGapTimer` is the active one. The pre-v40 cleanup in `update_door_state(CURR_CLOSING)` was gated on `TTCtimer.active()`, so a door starting CLOSING during the gap missed the cleanup → `forceCloseInProgress` leaked AND the gap timer fired press 2 onto a now-closing door, toggling it back to open. v40 separates the two concerns: TTCtimer detach + state-resend stays gated on `TTCtimer.active()` (only meaningful when a timer is live); `clear_force_close_state` runs unconditionally on every CURR_CLOSING transition (`comms.cpp:1069-1091`). The function early-returns on `forceCloseInProgress=false` so it's a no-op cost when no force-close is in flight.
- **W15 — `open_door` / `close_door` bypass / `toggle_door` leak path.** Three door-action sites (`comms.cpp:3160`, `:3325`, `:3389`) detach `TTCtimer` to cancel a TTC delay, but never invoked `request_force_close_clear`. If a force-close was using TTCtimer for press 1 hold, the detach killed the pending release callback as a side effect — the only path that clears `forceCloseInProgress` on normal completion. Same v17 leak pattern, never propagated. Added `request_force_close_clear()` after each detach with a contextual reason string.
- **W16 — Frontend duplicate switch-case made `updateAutoCloseStatusRow()` unreachable.** `functions.js` had two switch blocks matching `autoClose*` keys: one at line ~572 (correct, sets the form fields) and a duplicate at line ~811 (which had the `updateAutoCloseStatusRow()` call — the intended home-page status row refresh). JS switch matches first-and-break, so the duplicate never fired. Home-page Auto-Close status row never updated after initial page load. v40 deletes the unreachable block + adds `updateAutoCloseStatusRow()` calls inside the earlier `autoClose` / `autoCloseMinutes` / `autoCloseStartMinutes` / `autoCloseEndMinutes` / `autoCloseIgnoreWindow` cases (`functions.js:572-602`).

**Important fixes**

- **W13 — `rebooting` static bool now `volatile`.** Written by `statusCallback` (loopTask) on `HS_REBOOTING`, read by `homekit_health_log` (Ticker / esp_timer task). Without volatile the compiler can cache the value across reads, missing the reboot window in the diag log. Single-byte access is atomic on Xtensa so volatile is sufficient.
- **W17 — Watchdog state resets reordered BEFORE the release-store.** v38 W3 added release/acquire pairing on the `hkCfg*` config family with `hkCfgEnabled` as the RELEASE anchor — but the resets of `hkLastHintLevel` / `hkRecoverAttempts` / `hkConsecutiveHealthyTicks` happened AFTER the release-store, so a Ticker tick observing fresh `hkCfgEnabled=true` could observe stale counters for one tick. v40 reorders the resets to before the RELEASE on `hkCfgEnabled` so they get published with the rest of the config (`homekit.cpp:567-606`).
- **W19 — `logToSyslog` mutex bounded to 50ms.** v36 V1+V2 fix took the syslog mutex with `portMAX_DELAY`, which would block every concurrent logger indefinitely if a wedged UDP send (DNS SERVFAIL on stale hostname, lwIP buffer pressure) held the mutex. v40 uses `pdMS_TO_TICKS(50)`; on timeout, increments a `volatile uint32_t syslogDrops` counter and silently drops the line. The V1+V2 critical section is unreachable on timeout (we return before entering it) so the use-after-free + interleave properties are preserved. Counter is exposed for diag log surfacing in a future release; for now log-volume health gives the same signal.
- **W26 — Hint threshold order auto-fix.** The HomeKit hint-level cascade (`homekit_health_log`) evaluates `LikelyNR > Stale > Quiet` to assign level 3 / 2 / 1. Out-of-order user input (e.g. Quiet=1000, Stale=500, LikelyNR=300) silently subverted the intent — every idle tick jumped straight to level-3 Silent and the intermediate hints never fired. v40 adds an order-check after the per-key clamp pass at `web.cpp:1944-1973`: if not `Quiet < Stale < LikelyNR`, sort the three values ascending (3-element insertion sort), reassign in order, and log WARN so the user can see the auto-fix happened.

**Nit fixes**

- **W22 — `Serial.printf("Move door to: %d%\n", ...)` UB.** Bare `%\n` is an invalid printf conversion specification (C99 §7.19.6.1¶9). Most implementations emit a stray `%`, stricter ones may abort. Fixed to `%%\n` (literal `%` + newline) at `homekit.cpp:1144`. Dev-only path under `#ifdef USE_GDOLIB`.

**UX (v39 follow-up)**

- **`loadLogs()` calls `checkAuth()` first.** v39 introduced a per-IP recent-auth allowlist for the SSE log subscribe path (because EventSource can't do Digest auth). But the Logs tab JS opened EventSource immediately on tab load — pre-v40 the browser silently failed (EventSource doesn't surface 401 to the user) and required navigating to Settings or another auth'd page first to trigger the Digest prompt. v40 invokes the existing `checkAuth()` helper at the top of `loadLogs()` (`logs.js:111-119`), which forces a regular `fetch('auth')` call → browser fires the Digest prompt natively → user enters password → IP gets stamped via the v39 mechanism → EventSource opens cleanly. Same pattern that the firmware-update + HomeKit-pair flows already use to gate sensitive actions.

**Out of scope (still deferred)**

- Boot-time heap exhaustion → `tiT` IllegalInstruction in mDNS receive — architectural, requires boot-time-allocator profiling.
- W18 (`xTaskGetHandle("Tmr Svc")` lookup mismatch), W20 (service_timer_loop OTA gate), W21 (ESP8266 #ifdef portability), W23 (helperForceClose redundant clamp), W24 (handle_unsubscribe String alloc), W25 (web_loop rate limit) — deferred to a future audit pass; not blocking real-world behavior on current installs.

### v3.4.4-forceclose.39 (2026-05-05)

Hotfix release. Fixes the regression v37 introduced in the device-UI live log viewer for users with a password set.

**Fixed**

- **SSE log subscribe via EventSource — auth restored without breaking the browser.** v37 added `if (logViewer) AUTHENTICATE();` to `handle_subscribe` to gate the live SSE log feed behind the same Digest auth as `/showlog`. The mechanism worked for `curl --digest -u user:pass` but **hard-broke the browser's EventSource API** — every `?log=1` subscribe got a 401, and EventSource has no way to participate in Digest challenge/response (no API for the page to set custom headers, no built-in retry on 401). Result: the device-UI live log viewer was unreachable for any password-protected install on v37/v38. v39 keeps the security gate but routes around the EventSource limitation:
    - `AUTHENTICATE()` macro in `web.cpp:771-792` (both ESP8266 and ESP32 variants) now records the client IP into a 4-slot `authAllowlist` table after a successful Digest challenge. 5-minute TTL, oldest-evict on overflow. ~96 B BSS — zero heap impact, no malloc/free, no thread-local storage.
    - `handle_subscribe` log-stream branch (`web.cpp:2358`) replaces the broken `AUTHENTICATE()` call with a check against the allowlist via `isAuthAllowedForIP(server.client().remoteIP())`. Returns 401 with a clear "open an authenticated page first" message if the IP isn't in the table.
    - Browser flow: user navigates to `/showlog` (or any other auth'd page like `/setgdo` settings) → Digest challenge → enters password → AUTHENTICATE() succeeds → IP recorded in allowlist with 5-min TTL. Web UI's JS opens EventSource to `?log=1` from the same origin → IP-allowlist check passes → live SSE stream opens cleanly. An attacker on a different LAN IP cannot read the SSE log feed without first succeeding Digest from their own IP. No-password installs short-circuit at `getPasswordRequired() == false`, same as the rest of the auth pipeline.

This invents a small per-IP-allowlist mechanism (no precedent in the codebase) but the existing Digest auth doesn't work over EventSource at all, so SOME new mechanism was unavoidable. Rejected alternatives: (a) revert v37's SSE auth gate entirely — would leave the live log feed publicly readable, inconsistent with `/showlog`'s Digest gate; (b) full session-cookie machinery — significantly more code than the IP-allowlist for the same threat model on a home LAN.

**Out of scope (still deferred)**

- Boot-time heap exhaustion → `tiT` IllegalInstruction in mDNS receive (architectural). v36 fragmentation incident from "v37 follow-up findings" — separate concern, no v39 action.

### v3.4.4-forceclose.38 (2026-05-05)

Audit cleanup pass — closes all seven W1-W7 findings from the v36 post-closeout fresh-eye review (`audit-notes/2026-05-04-fork-vs-upstream-attribution.md`, "v36 post-closeout fresh-eye review" section). Three "Important"-severity items (W1, W4, W7) and four "Nit"-severity (W2, W3, W5, W6).

**Fixed (concurrency)**

- **W1 — `SSEBroadcastState` cross-task `removeSSEsubscription` deferred.** `SSEBroadcastState` is invoked from two task contexts: loopTask (web_loop's RATGDO_STATUS path) and any task calling ESP_LOGx (LOG::logToBuffer's LOG_MESSAGE fanout — Ticker callbacks, WiFi event task, comms-callback paths). The "client gone" branch (`web.cpp:2613`) called `removeSSEsubscription(&subscription[i])` directly, which descends into `client.stop()` → lwIP socket close. The v22 `SSEheartbeat` fix established the discipline that all SSE slot teardown happens on loopTask via `pendingRemove` + `process_sse_pending_removes`; v38 extends that discipline to this site. One-line change: `subscription[i].pendingRemove = true;` instead of the inline removal. The next service tick reaps it on loopTask.
- **W2 — `forceCloseGapPendingArmMs` writer release-store / drain consume RELAXED.** V3 in v36 closeout established the writer-uses-`__ATOMIC_RELEASE` / drain-uses-`__ATOMIC_ACQUIRE` discipline. This fourth flag was missed. Three sites now consistent: writer at `comms.cpp:2733` is `__atomic_store_n(..., __ATOMIC_RELEASE)`, clear path at `comms.cpp:2649` is `__ATOMIC_RELEASE`, drain consume at `comms.cpp:2671-2672` is `__ATOMIC_RELAXED` (reads after the acquire-load above are already ordered).
- **W3 — `cachedAutoClose*` and `hkCfg*` family release-acquire pair.** Same drift from V3 discipline as W2, but for the watchdog-config and auto-close-config caches. Writers (`comms_refresh_auto_close_config`, `homekit_refresh_watchdog_config`) RELAXED-store the four secondary values then RELEASE-store the primary flag (`cachedAutoCloseEnabled` / `hkCfgEnabled`). Readers ACQUIRE-load the primary flag first, then RELAXED-load the rest. Closes a one-tick window where a settings-save mid-Ticker could be observed partially-applied (e.g. new `cachedAutoCloseEnabled=true` visible but old `cachedAutoCloseMinutes=0` still cached).

**Fixed (HTTP)**

- **W4 — `enforce_same_origin` honors `X-Forwarded-Host` for reverse-proxy deployments.** Pre-v38, users behind nginx/Caddy/Traefik/Tailscale Funnel got 403 on every state-changing POST when the proxy forwarded `Host:` as the upstream IP rather than the public hostname (a common `proxy_pass` configuration). Browser sends `Origin: https://gdo.example.com`, proxy forwards `Host: 192.168.1.10`, exact-host match fails → 403, no diagnostic except the syslog line. v38 reads `X-Forwarded-Host` (or first comma-separated value, RFC 7239 / nginx convention for proxy chains), applies the same lowercase + bracket-aware port-strip + IPv6 canonicalization pipeline as `Host`, and tries the Origin/Referer match against THAT. Same-LAN attacker model unchanged: an attacker on the LAN can hit the device directly via Host=device-IP and bypass the proxy entirely; honoring `X-Forwarded-Host` doesn't strengthen that path. Diagnostic: rejection log now includes `X-Forwarded-Host` value when present, so post-deploy issues are diagnosable. New header registered in `setup_web()`'s `collectHeaders()` list.
- **W7 — `writeBuffer` stack-localized in `SSEBroadcastState` (ESP32 only).** Pre-v38 `SSEBroadcastState` filled the file-scope `writeBuffer[512]` then called `clientWriteEx` per subscriber. With the v24 log-mutex-release-before-broadcast fix, two concurrent broadcasts from different tasks (loopTask RATGDO_STATUS + esp_timer-task LOG_MESSAGE via `homekit_health_log` ESP_LOGW) overwrote each other's `snprintf` payload mid-write — subscribers saw truncated/garbled SSE events, observed in the wild as occasional "weird" log lines on log-viewer clients. Stack-local `char wb[512]` per invocation eliminates the race. Stack delta on ESP32 is +512 B per `SSEBroadcastState` call frame; loopTask (8 KB) and esp_timer (4 KB) both have headroom. ESP8266 keeps the static `writeBuffer` — single-task cooperative scheduling means the race doesn't exist there, and ESP8266's 4 KB main stack is too tight to absorb +512 B.

**Hardening (small)**

- **W5 — `firmwareUpdateSub` declared `volatile`.** Pointer is written by `handle_firmware_upload` (loopTask) and read by the HK watchdog Ticker callback (esp_timer task) via `firmware_update_in_progress()` — the F5 OTA-inhibit path from v32. Without `volatile`, an aggressive optimizer could hoist the load out of `firmware_update_in_progress`. The watchdog re-enters every 180s so a missed tick is self-correcting, but the discipline cost of `volatile` is one keyword.
- **W6 — `doorOpenedAtMillis` block comment corrected.** The pre-v38 comment claimed the variable held wallclock seconds (`time(NULL)`); the implementation uses `millis()` and relies on unsigned-subtraction wrap-safety for the 49.7-day rollover. The behavior matches the comment's promise but the implementation differs — future readers would have either been confused or "corrected" the code in the wrong direction. Comment-only change at `comms.cpp:166-178`.

**Out of scope (still deferred to a future release)**

- Boot-time heap exhaustion → `tiT` IllegalInstruction in mDNS receive (architectural, requires profiling of boot-time allocators — see audit-notes "v37 follow-up findings" for the v33 crash log excerpt and v38 candidate fixes to investigate).

### v3.4.4-forceclose.37 (2026-05-05)

Hotfix release. Three items: log-endpoint auth hardening, the device-side OTA path (broken since v34's CORS regression), and crash-log hygiene cleanups surfaced when the user inspected an existing crash log on a v33 device. Heap-exhaustion-at-boot finding from the same crash log is documented but deferred to v38.

**Security**

- **Logs now require auth when a password is set.** `/showlog`, `/showrebootlog`, `/crashlog`, and the SSE log-viewer subscribe path (`/rest/events/subscribe?log`) all go through `AUTHENTICATE()` as of v37. Pre-37 these were the only log-bearing endpoints not behind Digest auth — `/setgdo`, `/reboot`, `/update`, `/reset`, `/reconnectHomeKit`, `/refreshHomeKitMDNS`, `/dumpHomeKitState`, and `/clearcrashlog` all already required auth, but the message log, the saved reboot log, and the crash log were readable by anyone on the LAN. Crash logs are the highest-value leak: stack traces + firmware version make CVE-based exploit narrowing trivial on a known-vulnerable device. Message logs additionally expose internal IPs, mDNS hostnames, and HomeKit pairing chatter. Behavior for users without a password configured is unchanged — `AUTHENTICATE()` is a no-op when `getPasswordRequired() == false`. Door-status SSE (subscribe without `log`) remains open, matching `/status.json`'s public policy. The `if (logViewer) AUTHENTICATE();` branch lives between argument parsing and slot allocation in `handle_subscribe` so unauth requests are rejected before any slot state is mutated. `viewlog.sh` is unchanged on purpose — it matches the existing project convention (`reboot.sh`, `upload_firmware.sh`, `verify_firmware.sh` also hit auth-protected endpoints with naked curl); users with a password set will need to add `--digest -u user:pass` to both curl invocations themselves.

**OTA / web UI**

- **`functions.js` device-side OTA fetch URL — github.com → GitHub Pages (CORS regression fix).** v34's hotfix at `functions.js:1043` switched `serverStatus.downloadURL` from a Pages-relative URL to `https://github.com/<gituser>/<gitrepo>/releases/download/<tag>/<asset>`. github.com responds to release-download requests with a **302 redirect** to `objects.githubusercontent.com`. The redirect target carries `Access-Control-Allow-Origin: *`, but the **302 response itself does not** — and browser CORS rules require the header on every response in the chain, including redirects. Result: the device's "Update from GitHub" button (`fetch()` from origin `http://<device-ip>` to `github.com/...`) was hard-blocked by CORS the entire v34/v35/v36 lifetime; nobody noticed because the local-file OTA path and the web installer at github.io worked. v37 reverts to `https://<gituser>.github.io/<gitrepo>/firmware/<asset>`. GitHub Pages serves with `Access-Control-Allow-Origin: *` directly (no redirect chain) so cross-origin `fetch()` from the device-UI succeeds. The four flash bins (firmware.bin / bootloader.bin / partitions.bin / firmware.md5) are committed to `docs/firmware/` on every release tag (MH7 redux, `release.yml:179-206`) so the Pages URL is reliably populated. **Manifest URLs stay at github.com release attachments** — those are consumed by the web installer, which uses ESP Web Tools' fetch semantics and handles the 302 chain correctly.

**Crash log hygiene**

- **Truncated `Firmware version: 3.4.4-forceclos`.** `crashVersion` was `RTC_NOINIT_ATTR char[16]` (`log.cpp:150`). `AUTO_VERSION` for v36 is `"v3.4.4-forceclose.36"` = 20 chars + NUL — `strlcpy` with `sizeof=16` truncated to 15 chars + NUL → `"3.4.4-forceclos"`. Bumped to `[32]` to fit `"v3.4.4-forceclose.999"` with headroom. Cost: +16 B in RTC_NOINIT (~0.2% of 8 KB region). Side effect: bumping a `RTC_NOINIT_ATTR` field's size shifts subsequent field offsets, so `resetMagic` lands at a new offset post-OTA — the LOG constructor's `resetMagic != RESET_MAGIC` check fails on first v37 boot and runs the cold-init branch. Net: any v36-captured crash log is discarded on the v37 OTA boot. Acceptable for a hotfix — same effect as a power cycle.

- **Crash log header uptime didn't match buffer's last-line uptime.** Crash log header reported `Server uptime: 16228 ms` while the message buffer payload contained log lines stamped up to `(00:00:20.321)`. Mechanism: `panic_handler` samples `crashUpTime` and `memcpy`'s `msgBuffer` to `rtcCrashLog`, but other FreeRTOS tasks on different cores (lwIP `tiT`, mDNS, HomeSpan autoPoll) keep running until the chip actually resets — their late `ESP_LOGx` writes raced into `msgBuffer` after the snapshot was taken, polluting the captured ring with post-snapshot timestamps. Fix: new `volatile bool panicSnapshotDone` flag set at the END of `panic_handler` after the memcpy/strlcpy block (`log.cpp:213`); `LOG::logToBuffer` early-returns if it's set (`log.cpp:265-274`). `SERIAL_PRINT` (UART) output remains for serial-console debuggers since `panic_handler` already uses `esp_rom_printf` directly. Bounds the mismatch to one in-flight line that already passed the gate before the flag was set.

- **Steady-state crash logs no longer carry HomeKit/WiFi/mDNS init noise.** Pre-v37, `msgBuffer` was a fixed-size ring buffer that retained log lines from device boot all the way through whenever the crash captured the snapshot. For a crash 5 minutes into operation, the operator wanted "the last 5 minutes" — but if log volume hadn't been enough to wrap the buffer, they got the previous 12 seconds of HomeKit service configuration + WiFi connect chatter + mDNS service registration + initial GDO panel-detection retries on top. New `LOG::clearMessageBuffer()` method (`log.h`/`log.cpp`) is invoked from the post-IP one-shot init block in `loop()` immediately after the `=== Initialization complete` log line (`ratgdo.cpp:351`), zeroing the ring buffer once boot is done. Early-boot crashes still preserve full boot trace (the clear hasn't fired yet) — useful for debugging init-time faults like the `tiT` mDNS-OOM crash. The clear takes the log mutex so it's race-safe against concurrent `logToBuffer` writers, and is gated on `!panicSnapshotDone` so a panic post-cleanup never re-arms.

**Out of scope (deferred to v38)**

- **Boot-time heap exhaustion → `tiT` crash.** A crash log inspected during v37 development showed the device's heap dropping from 194 KB at boot → 80 KB after `setup_web` → 108 bytes during mDNS announce flood, with `mdns_networking: Cannot allocate memory (receive(176))` immediately preceding an `IllegalInstruction` in lwIP's `tiT` task. Different code path from v36's V4 fix (which was the logger, not lwIP). Architectural — needs profiling of boot-time allocators (HomeSpan accessory DB, mDNS service registration, WiFi station init, SSE pool allocation) before a fix can be designed. Tracked alongside W1-W7 audit findings in private fork audit notes; v38 will batch them.
- **W1-W7 audit findings** (v36 post-closeout fresh-eye review). W1 (`SSEBroadcastState` cross-task `removeSSEsubscription`), W4 (`enforce_same_origin` reverse-proxy rejection), W7 (`writeBuffer` global shared across SSE-broadcast callers) are the three "Important"-severity items. W2/W3/W5/W6 are nit-level. All deferred to v38; v37 stays scoped to the OTA-broken-since-v34 hotfix + log auth + crashlog hygiene.

### v3.4.4-forceclose.36 (2026-05-04)

Closeout pass on the v34/35 fresh-eye audit (`audit-notes/2026-05-04-fork-vs-upstream-attribution.md`, "v34 fresh-eye review" section). All seven open findings (V1, V2, V3, V4, V5, V6, V7) and the MH7 redux are addressed in this PR. After v36 ships and bakes for a few days, the open-items table goes to zero and the fork moves into observation mode.

**Fixed (concurrency)**

- **V1 + V2 — `WiFiUDP syslog` lifetime + `endPacket()` race.** Both findings collapse to the same fix: a single `syslogMutex` (FreeRTOS `SemaphoreHandle_t`) now guards the whole `logToSyslog` body — the lazy-allocate path (V1: `delete syslog; syslog = nullptr;` was racing against a concurrent caller mid-`beginPacket`/`print`/`endPacket`) and the actual UDP send sequence (V2: two callers serialized `beginPacket()` and `endPacket()` separately could interleave their print*() output into the same packet, ending with one mangled UDP datagram and one lost). Restructured to single-return goto-cleanup pattern (precedent: `comms.cpp:1658 readIn:`) so the take/give pair stays balanced across all early-exit paths.
- **V3 — `autoCloseRescheduleRequested` release/acquire ordering.** `request_auto_close_reschedule` (esp_timer-context, called from SNTP `time_is_set` callback per F6) was a plain `volatile bool = true;` write paired with a plain `volatile bool` read in `auto_close_drain_pending_reschedule` (loopTask). On Xtensa the load is single-instruction-atomic but there's no memory ordering — drain could observe the flag set without observing the writes that established the new wallclock state. Now `__atomic_store_n(&flag, true, __ATOMIC_RELEASE)` writer / `__atomic_load_n(&flag, __ATOMIC_ACQUIRE)` reader, matching the Finding C pattern from v32.
- **V6 — `WiFi.isConnected()` guard in F7 stage 2.** When `homekit_drain_pending_reconnect_stage2` fires, some chipsets / supplicant configs have already auto-reconnected during the 250ms gap. Calling `WiFi.reconnect()` on an already-associated interface is documented as idempotent but in practice triggers `esp_wifi_disconnect`+`esp_wifi_connect` under the hood — briefly disrupting the just-established connection. Stage 2 now skips `WiFi.reconnect()` when `WiFi.isConnected() == true` and logs the skip via `HK_DIAG_LOG`. State machine returns to idle either way.
- **V7 — `homekit_force_reconnect` rapid re-entry guard.** Two near-simultaneous triggers (watchdog auto-recover firing while user clicks `/reconnectHomeKit`, or two watchdog recoveries back-to-back) could both call `homekit_force_reconnect` within the 250ms stage-1 window. Second call would overwrite `reconnectStageStartMs`, deferring the stage-2 `WiFi.reconnect()` indefinitely (kept getting reset to "now"). Early-return added at top: if `__atomic_load_n(&reconnectStage, __ATOMIC_ACQUIRE) == 1`, log a WARN and return without touching state. First reconnect cycle completes; subsequent triggers can re-enter only after stage 2 has cleared the stage flag.

**Reverted**

- **V4 + V5 — F4 `thread_local outLine`.** v33 made `LOG::logToBuffer`'s 256 B `outLine[]` `thread_local` to move it off-stack into TLS. ESP-IDF GCC's `thread_local` for non-trivial-construct storage uses `emutls`, which heap-allocates the TLS area on first access. Under the exact conditions where the watchdog-recovery log is most useful (heap exhaustion during a recovery window), TLS alloc could fail and trigger `__cxa_throw_bad_alloc()` → abort — the recovery log's own buffer would be the OOM trigger. F4 reverted to plain stack-local `char outLine[LINE_BUFFER_SIZE];`. Both loopTask (8 KB) and esp_timer (4 KB) stacks have headroom for the 256 B frame.

**Workflow**

- **MH7 redux — exclude `.elf` from `docs/firmware/`.** `release.yml`'s "Commit firmware bins to docs/firmware/" step (re-instated in v35 hotfix) was committing all five build artifacts including the multi-MB `.elf` on every release cut. The device-side OTA path (`functions.js`) only fetches `.firmware.bin` / `.bootloader.bin` / `.partitions.bin` / `.firmware.md5`. The `.elf` is needed only by the crash-backtrace decoder, which already gets it from the GitHub release attachments and the workflow artifact. Trimmed `git add` to the four flash bins. The `.elf` continues to ship as a release attachment (existing `Attach all firmware artifacts to release` step) and as a workflow artifact. Net `.git` growth per release drops by ~30 MB.

**Out of scope (intentionally NOT in v36)**

- MH1 ping-pong status_json — verified unsafe in v33 (esp_timer SSEheartbeat writer races loopTask reader on flip). Permanently parked.
- MH2 PSTR-wrap — ESP8266-only optimization; no upstream PR pending.
- MH5 `-flto` — incompatible with `-Wl,--wrap=esp_panic_handler`. Permanently parked while the panic-handler wrap exists (load-bearing for v22+ crash-log preservation).
- MH6 `STATUS_JSON_BUFFER_SIZE` retune — instrumentation shipped in v33; needs days/weeks of `jsonPeak=…B` data from real installs before retuning. Holding pattern.

**Verification**

- ESP32 build clean against existing test bench. End-to-end OTA test from v35 → v36 via device web UI and via web installer is REQUIRED before merge — see PR checklist.

### v3.4.4-forceclose.35 (2026-05-05)

**Hotfix release.** v34's CI build failed at the link step — the `-flto` flag added in MH5 is fundamentally incompatible with the fork's existing `-Wl,--wrap=esp_panic_handler` flag (used for ESP32 panic capture). LTO inlines / eliminates the wrapped function so the linker can't resolve `__wrap_esp_panic_handler` (and `app_main` got eliminated too). Net: v34 release exists with manifest only, no bins → users can't OTA to v34.

**v35 reverts only `-flto`.** Other v34 changes (hkVerboseLogs toggle + UI, F7 split-stage WiFi reconnect, the v33 device-side OTA URL hotfix in `functions.js`, `-Os`/`-ffunction-sections`/`-fdata-sections`/`-Wl,--gc-sections` size flags) all stay. Net flash savings reduce from estimated 5-15% (full LTO) to ~1-3% (gc-sections only) — correctness preserved, modest size win still in.

The fork can't enable LTO without dropping the panic-handler wrap, which is load-bearing for the v22+ crash-log preservation feature. Marking MH5 LTO as **not pursuable in this firmware**.

### v3.4.4-forceclose.34 (2026-05-05)

The "needs-build-validation" bucket of audit follow-ups, plus a user-spec'd watchdog log gating toggle, plus a HOTFIX for the v33 device-side OTA breakage.

**⚠️ HOTFIX — device-side OTA URL was broken in v33**

v33 ship MH7 (manifest paths → absolute GitHub release URLs) but missed updating `src/www/functions.js:1019`, which constructs the device's `Update from GitHub` button download URL from a Pages-relative pattern. After v33 shipped, devices on v32 trying to OTA to v33 via the device's web UI got an MD5 mismatch error because the `firmware.bin` Pages URL no longer existed. **v34 patches functions.js to construct the github.com release-download URL.** Workaround until v34 ships: use the web installer at `https://haglerd.github.io/homekit-ratgdo32/` (which fetches `manifest.json` directly with the correct URLs).

**Added (user-spec)**

- **`hkVerboseLogs` toggle (default OFF).** Gates the periodic 180s `HomeKit health` / `HomeKit diag-sse` / `HomeKit diag-hk` lines + post-action narration (mDNS refresh complete, HomeSpan state dump complete, HomeKit reconnect WiFi cycling) + `HomeKit watchdog config refreshed` behind a user setting. **Event-occurred lines stay unconditional**: auto-recover (1/2 / 2/2 / give-up), hint-level transitions (iOS extended idle / gone quiet / silent), HAP reads resumed, pair-state-changed, controller-list-changed, WiFi disconnect, mDNS-refresh-requested intent log, dump-state-requested intent log, reconnect-requested intent log. Implementation: new `cfg_hkVerboseLogs` config key + cached `hkCfgVerboseLogs` static + `HK_DIAG_LOG()` dual-level macro that emits at INFO when toggle ON, DEBUG when OFF (so a developer can still see them by setting global log level to DEBUG without flipping the user toggle). Surfaced in Settings → HomeKit Watchdog → Verbose Logs row + `/status.json`. Honest framing: this does NOT reclaim BSS or flash (format strings still compiled in). What it buys: reduced log ring-buffer churn → more useful crash-log context (16 KB ring wraps every ~80 min of normal operation; quiet watchdog preserves much more pre-crash context), reduced SSE broadcast traffic to live log subscribers, reduced syslog UDP traffic, cleaner default-config syslog feed.

**Build / portability (NEEDS BUILD VALIDATION before deploy)**

- **MH5 — `-flto` + size-opt build flags.** Added `-Os`, `-flto`, `-ffunction-sections`, `-fdata-sections`, `-Wl,--gc-sections` to `[env:ratgdo_esp32dev]` build_flags. Expected `.text` reduction 5-15% on this firmware size. **Risk**: LTO can surface latent ODR violations or undefined-behavior that separate compilation tolerated. Audit recommended budgeting half a day for the first LTO build to triage any LTO-surfaced issues.

**Concurrency (state-machine change — NEEDS HARDWARE SOAK)**

- **F7 — split-stage WiFi reconnect, no more `delay(250)` on loopTask.** `homekit_force_reconnect` previously called `WiFi.disconnect(false); delay(250); WiFi.reconnect();` — the 250ms `delay()` blocked loopTask, stalling concurrent HTTP requests, comms_loop, and SSE broadcasts for the full window. v34 splits into stages: stage 1 issues `WiFi.disconnect()`, records timestamp, returns. New `homekit_drain_pending_reconnect_stage2()` (called every service_timer_loop tick on loopTask) checks elapsed time and fires `WiFi.reconnect()` when ≥250ms have passed. Net loopTask block: ~0ms (just the disconnect call). Audit had this as "no action / acceptable" but user explicitly requested it for v34.

**Memory hygiene (instrumentation continuation)**

- **MH6 — retune deferred to runtime data.** v33 added the `jsonPeak=…B` instrumentation; v34 does NOT retune `STATUS_JSON_BUFFER_SIZE` yet — needs days/weeks of jsonPeak data from real installs (v33+) to inform a safe cap. Audit explicitly says "do not apply without measuring."
- **MH2 PSTR-wrap** — out of scope (ESP8266-cherry-pick work; not pursuing upstream PR currently).

**Out of scope NOT in v34**: MH2 PSTR (ESP8266-only, no upstream PR pending).

### v3.4.4-forceclose.33 (2026-05-05)

Mechanical cleanup pass — zero-build-risk audit follow-ups bucketed for this PR. **No behavior change for users.** Code review: GREEN.

**Fixed (concurrency)**

- **F6 — NTP/DST sync re-arms auto-close.** `time_is_set` SNTP callback now calls `request_auto_close_reschedule()` after updating `clockSet`. NTP correction or DST jump shifts the auto-close window-start boundary; the existing one-shot Ticker (armed against the pre-correction wallclock) would wake at the wrong absolute time. Drain runs on loopTask and re-detaches/re-attaches the Ticker safely. Both ESP8266 + ESP32 callback paths covered.

**Memory hygiene**

- **F4 — `thread_local` outLine in `LOG::logToBuffer`.** Was `char outLine[LINE_BUFFER_SIZE]` stack-local (256 B per concurrent ESP_LOGx caller's stack frame). Now `thread_local` for ESP32 — each task gets its own buffer in TLS instead of paying repeated stack-frame growth. ESP8266 falls back to stack-local (single-task cooperative RTOS, no per-task TLS). Net: same RAM total, but moved off-stack, which matters for tight stacks like Tmr Svc.
- **MH3 follow-up — free `WiFiUDP syslog` when toggled off.** v32 added lazy-allocate; v33 adds the symmetric free path: when a future `logToSyslog` call sees `syslogEn=false` AND a previously-allocated `syslog` pointer, `delete syslog; syslog = nullptr;`. Re-allocates next time syslog is enabled. ~200 B heap reclaimed when the user toggles syslog off after enabling it.
- **MH6 instrumentation (NOT a retune).** New `volatile uint32_t statusJsonPeakLen` — CAS-loop atomic-max writer in `handle_status` after each successful build, atomic-exchange-zeroed by `homekit_health_log` each window. Added as `jsonPeak=…B` to the `HomeKit diag-sse` log line. Purpose: collect peak data across days/weeks of real usage so v34 can decide whether `STATUS_JSON_BUFFER_SIZE` (currently 2560 B ESP32 / 2048 B ESP8266) can be retuned safely without flying blind. Audit explicitly says "do not apply MH6 retune without measuring" — this is the measurement.

**Workflow**

- **MH7 option 1 — release.yml: bins on GitHub release attachments only, manifest paths absolute.** The per-release "Commit firmware bins to docs/firmware/" workflow step is removed. Manifest path-update steps now write absolute `https://github.com/Haglerd/homekit-ratgdo32/releases/download/<tag>/<filename>` URLs instead of relative `firmware/<filename>` paths. Bins continue to upload to the GitHub release attachments via the existing `upload-release-assets` step. Net effect: bounded `.git` growth going forward (was ~5 MB/release × ~50 releases/year). Older bins in `docs/firmware/` stay (historical).

**Intentionally NOT in v33 (analysis revealed audit was wrong)**

- **MH1 ping-pong status_json** — audit's premise was "all writers/readers on loopTask." Verified incorrect: `SSEheartbeat` is a `status_json` writer (`web.cpp:1992-2020`) running from Ticker / esp_timer task. 2-buffer ping-pong races a loopTask reader against the esp_timer writer when the active flag flips mid-fanout. Current 3-buffer design (status_json + per-reader localJson + status_json_send) is correct and necessary. Skipping MH1 — savings (~2.5 KB BSS) not worth a new race surface.

### v3.4.4-forceclose.32 (2026-05-05)

Audit follow-up cleanup of v31 — concurrency hygiene the v31 work surfaced after deployment, plus two memory-headroom items and a sweep of the version-prefix comment debt that accumulated v22-v31. **No new features, no behavior changes for users.** All atomic ordering pairs verified, all call sites swapped, all headers in sync. Code review: GREEN, ship it.

**Fixed (concurrency)**

- **Audit findings A + G — `clear_force_close_state` deferred to loopTask.** v31 deferred the gap-timer ARM but `clear_force_close_state` was still called directly from esp_timer task at four TTC-timer-callback sites in `send_force_close_release_then_maybe_retry` / `send_force_close_press`. That meant `Ticker.detach()` could still race on the gap timer between loopTask's `force_close_drain_pending_arm` and esp_timer's `clear_force_close_state` — same physical-world door-reversal failure mode v31 was meant to close, just relocated. New `request_force_close_clear(reason)` setter for esp_timer-context callers + `force_close_drain_pending_clear()` drained on loopTask via `service_timer_loop`. `clear_force_close_state` now loopTask-only. Reason is a string-literal pointer (single 32-bit pointer write — atomic on Xtensa).
- **Audit finding B — acquire-load on `forceCloseInProgress` in drain.** v31's `__atomic_test_and_set` writer was paired with a plain volatile load in `force_close_drain_pending_arm`, breaking the release/acquire chain. Both drains and `clear_force_close_state` now use `__atomic_load_n(&forceCloseInProgress, __ATOMIC_ACQUIRE)`.
- **Audit finding C — release/acquire on HomeKit deferred-flag setters/drains.** Setters (`homekit_request_reconnect`, `_request_refresh_mdns`, `_request_dump_state`) now write `reason` first, then `__atomic_store_n(&flag, true, __ATOMIC_RELEASE)`. Drains use `__atomic_load_n(&flag, __ATOMIC_ACQUIRE)` then read reason — guarantees the reason is never stale relative to the observed flag.
- **Audit finding D — atomic counter writers.** v31 made the readers atomic via `__atomic_exchange_n` but writers were still plain RMW. Writer for `logMtxMaxWaitMs` is now a CAS loop atomic-max via `__atomic_compare_exchange_n` (RELAXED ordering — diagnostic counter, no ordering needed). All three `sseOrphansReaped` writers in `sweep_sse_orphans` now use `__atomic_fetch_add(..., 1, __ATOMIC_RELAXED)`.
- **Audit finding F5 — watchdog inhibited during OTA.** `homekit_health_log`'s auto-recover branch (when `hkAutoRecover=true`) could fire during a slow OTA upload — iOS HAP reads typically pause during the upload, `last_hap_read_ago` keeps growing past the trigger threshold, watchdog cycles WiFi mid-upload → upload aborts → device falls into the rollback path. New `firmware_update_in_progress()` helper exposed from web.cpp; the watchdog branch now gates on `&& !firmware_update_in_progress()`. Single pointer load = atomic on Xtensa, no synchronization needed for a hint-quality signal.

**Memory hygiene**

- **MH3 — Lazy-allocate `WiFiUDP syslog`.** Was 200 B BSS always. Now `static WiFiUDP *syslog = nullptr` + lazy `new (std::nothrow)` on first `logToSyslog` call when `syslogEn=true && WiFi.isConnected()`. Default install (syslogEn=false) saves ~196 B BSS forever. Once enabled, allocates 200 B heap.
- **MH4 — `enforce_same_origin` shared host buffer.** Per-call `originHost[64]` + `refererHost[64]` consolidated to one `extractedHost[64]` — Origin and Referer are checked sequentially and `extractHostFromUrl` zeros the buffer on entry. Saves 64 B stack/POST. Cross-origin rejection log still uniquely identifies the attempt (Origin + Referer + Host headers retained).

**Cleanup (no functional change)**

- **Comment debt sweep**: 151 → 87 fork-versioned comment leaders (`// v22:` … `// v31.2:`). Deleted multi-paragraph evolution blocks targeted by the audit: `enforce_same_origin` 40-line block, `sweep_sse_orphans` 23-line skew-detection block, `handle_subscribe` 7-line v27 ORDER NOTE + 20-line v28 INADDR_NONE block, force-close 17-line v31 deferred-arm block, HomeKit deferred-flag 16-line v24→v31 block, `comms.cpp` v23 deferred-reschedule + v31.2 cache-extension blocks, `ratgdo.cpp` drain-comment narration. Historical context belongs in commit messages and `audit-notes/`, not source.
- **ESP_LOG version-prefix strip**: `"v27: heartbeat=0 coerced to %u..."` → `"heartbeat=0 coerced to %u for %s"` (web.cpp:2285). `"v27: unsubscribe beacon..."` → `"unsubscribe beacon for UUID %s on channel %u"` (web.cpp:2447). Implementation-detail parentheticals trimmed: `(via main-loop drain)`, `(deferred to main-loop arm)`, `expect '(re)connected to AP' shortly`.
- **ESP_LOGI → ESP_LOGD demotes** for routine state (force-close release-sent + press hold; auto-close scheduler narration in 5 sites + boot/refresh/bootstrap; HK watchdog config refresh; controller list change; WiFi reconnected; mDNS refresh complete; HomeSpan state dump; HomeKit reconnect WiFi cycling). KEPT INFO on entry/completion lines: `FORCE CLOSE: starting`, `skipping second press`, `2-attempt sequence complete`, plus the 3 HomeKit health/diag-sse/diag-hk lines and all WARN-level orphan reaps + CSRF rejections.

**Out of scope (intentionally deferred)**: MH1 (ping-pong status_json, risky), MH2 PSTR-wrap (ESP8266-specific, schedule for upstream PR), MH5 build flags (-flto needs build verification), MH6 buffer retune (needs measurement first).

### v3.4.4-forceclose.31 (2026-05-05)

External-audit-driven cleanup of fork-introduced concurrency, security, and portability debt accumulated across v22-v30. All ten audit findings + three follow-up review items addressed. ESP32 build verified clean against the existing test bench; behavior changes are conservative (no new features, only correctness/observability/portability hardening).

**Fixed (concurrency / runtime correctness)**

- **SSE orphan sweep silent-disable regression introduced by v30.** v30's `int32_t` cast on `(now - timestamp)` fixed the TOCTOU race against writers but flipped any slot legitimately aged past 2³¹ ms (~24.85 days) to a negative comparison → `negative > timeout` is false → slot never reaped (sweep silently disables itself). v31 replaces the cast with explicit skew detection: `int32_t skew = (int32_t)(stamp - now); if (skew > 0) continue;` then unsigned `age = now - stamp` for the timeout test. Race fix preserved, long-uptime regression eliminated, mod-2³² wrap still handled correctly.
- **Force-close shared state race (door-reversal risk).** `forceCloseInProgress`/`forceCloseAttempt`/`forceCloseGapTimer` were mutated from both the comms loop (loopTask, `update_door_state(CURR_CLOSING)` → `clear_force_close_state`) and the TTCtimer Ticker callback chain (release → retry/cleanup) without synchronization. Observed race: Ticker decided "schedule attempt 2" → loopTask preempted and cleared force-close state → Ticker resumed and re-armed the gap timer → 1.5 s later attempt 2 fired on a door already physically closing → wall-button-press toggled it back toward Open. Fix: deferred-arm pattern — Ticker sets `forceCloseGapPendingArmMs`, new `force_close_drain_pending_arm` runs on loopTask under `service_timer_loop`, re-checks `forceCloseInProgress` before arming. Plus `__atomic_test_and_set` on the busy flag itself replaces the v22 check-then-set, eliminating the dual-core race where two cores could both pass the check before either set true (doubled-relay-press scenario).
- **`handle_status` torn-buffer race (Homebridge-visible).** v24's mutex-release-before-`server.send_P` mitigated broadcast-stall but introduced a TOCTOU: web_loop on the loopTask was free to take the mutex and overwrite `status_json` mid-send, so Homebridge intermittently saw torn JSON (half-old, half-new) → schema-rejected accessory updates. Fix: snapshot `status_json` into a dedicated `status_json_send` buffer (BSS on ESP32, mutex-held-across-send on ESP8266 to save heap) before releasing the mutex. Both invariants preserved.
- **`handle_status` ESP8266 portability gate.** Above fix is `#ifndef ESP8266` — ESP8266's heap can't afford another 2 KB BSS, and its SSE path can't sustain enough concurrent subscribers to expose the deadlock surface that motivated v24's mutex release. ESP8266 holds the mutex across the send, with a defense-in-depth `json[BUFSZ-1] = '\0'` against `build_status_json` regressions.
- **Auto-close Ticker self-reschedule anti-pattern.** `update_auto_close_schedule`'s outside-window branch armed itself as the one-shot's callback, repeating the FreeRTOS self-detach pattern that crashed `SSEheartbeat` in v22 (uxListRemove panic). Routed via `request_auto_close_reschedule` → main-loop drain.
- **`userSettings::get` data-race mitigation (Ticker-context auto-close reads).** `userSettings::get` is mutex-free upstream while `set` takes the mutex (variant-tear race when read concurrent with write). The fork's new `checkAutoClose` Ticker reader and `autoCloseInWindow` made this reachable in practice. Mitigation: `comms_refresh_auto_close_config` caches all 5 auto-close keys into volatile statics, refreshed at boot + on `/setgdo` save. All Ticker-context reads now hit the cache. Upstream issue draft kept in `audit-notes/` for the underlying class fix.
- **HomeKit watchdog sustained-recovery gate.** `hkRecoverAttempts` reset on a single sub-60 s read in any 180 s window, so a flapping iOS hub delivering one sporadic read could re-arm the watchdog forever (hours of repeated WiFi cycles, no escalation cap). Tightened to require `HK_HEALTHY_TICKS_TO_RESET = 3` consecutive ticks below `hkQuietSecs` before clearing — at 180 s cadence that's ≥9 min sustained healthy reads, well past typical hub flap interval.
- **HomeSpan-from-web-task entry points deferred.** New fork endpoints `/refreshHomeKitMDNS` and `/dumpHomeKitState` called `homeSpan.updateDatabase`/`processSerialCommand` from the WebServer task — outside HomeSpan's documented autoPoll-task safe zone. Routed through new request-flag + drain pattern matching v24's reconnect deferral. Same applied to the watchdog auto-recover's `homekit_refresh_mdns` call (was running from esp_timer task). Upstream's pre-existing `helperFactoryReset` and OTA `vTaskDelete(getAutoPollTask())` deliberately left alone (audit issue C, upstream-side).
- **Per-task log recursion guard.** v24's single-slot `static volatile TaskHandle_t inFnTask` was wiped when a second concurrent task entered, then a re-entry by the first task could recurse through `clientWriteEx`'s slow-write `ESP_LOGW` — exactly the load v24 was deployed to instrument. Replaced with an 8-slot CAS-protected table (`__atomic_compare_exchange_n` for slot acquisition, `__atomic_store_n` release on exit). Per-task isolation; table-exhaustion silently drops broadcast/syslog rather than risk recursion.
- **`reconnectHKReason` torn-write race.** `strncpy` from web handler concurrent with main-loop drain produced occasional garbled log strings. Replaced `char[64]` with `volatile HomekitDeferredReason` enum (single-byte, atomic on Xtensa). Two-writer last-wins semantic preserved; torn writes eliminated. Same enum pattern reused for the new mdns/dump-state defer pairs.
- **`logMtxMaxWaitMs` lost-sample race.** Read-then-zero from `homekit_health_log` was non-atomic; logger task could write the max between the read and the zero. Replaced with `__atomic_exchange_n(&logMtxMaxWaitMs, 0, __ATOMIC_RELAXED)` — single Xtensa instruction. Same pattern applied to `sseOrphansReaped`.

**Fixed (security)**

- **`enforce_same_origin` rewritten — two real bypasses closed.** Pre-v31:
  1. **Substring match.** `origin.indexOf(hostOnly) >= 0` passed any URL whose path/query merely contained the device's hostname as a substring. With `Host: ratgdo`, an attacker page at `http://evil.example/?ratgdo` cleared the check.
  2. **Missing-headers passthrough.** When both `Origin` and `Referer` were absent, the guard returned true. Default-no-password installs reach state-changing endpoints (incl. `/setgdo` `forceClose`) with this guard as the only CSRF defense, so a `<form>` POST with `Referrer-Policy: no-referrer` bypassed it from any same-LAN page.
  
  v31 parses `Origin`/`Referer` URLs with a real host extractor, compares lowercased + port-stripped host fields exactly, and treats absence of both `Origin` AND `Referer` as a hard fail. Bracket-aware IPv6 parsing (`[::1]:8080` → `[::1]`), RFC 6874 zone-ID stripping (`[fe80::1%eth0]` → `[fe80::1]`), degenerate-input fail-close (`[]` → reject). Companion zero-heap rewrite: stack-local `char[]` buffers throughout, no Arduino String allocations — eliminates the 6 `String` allocs per state-changing POST that were the dominant per-request heap-fragmentation surface on ESP8266.

**Added (observability)**

- **HomeKit health log split into 3 lines** (under 256-byte LINE_BUFFER_SIZE — the original combined line was already truncating mid-token at `sseOrphansReaped` pre-v31). New schema:
  - `HomeKit health: wifi=…  rssi=…  heap=…  maxBlock=…  uptime=…  paired=…  controllers=…  last_hap_read_ago=…`
  - `HomeKit diag-sse: logMtxMaxWait=…  sseSlowWrites=…  sseBufferFullSkips=…  sseSlotsAlloc=…  sseOrphansReaped=…`
  - `HomeKit diag-hk:  recoverAttempts=…  hintLevel=…  hkHealthyTicks=…  loopHWM=…B  tmrHWM=…B  apHWM=…B  tickDrift=…ms`
- **Watchdog state visibility:** `recoverAttempts`/`hintLevel`/`hkHealthyTicks` on every health line — confirms tuning is right without grepping for WARN-level auto-recover lines.
- **Per-task stack high-water marks (bytes):** `loopHWM`/`tmrHWM`/`apHWM` for loopTask, FreeRTOS Tmr Svc, and HomeSpan autoPoll — climbing toward zero indicates near-overflow. ESP-IDF wrapper returns bytes natively (no `* sizeof(StackType_t)` multiplier needed).

**Changed (build / portability)**

- **Advisory compiler warnings:** `-Wsign-compare`, `-Wsign-conversion`, `-Wshadow` added to `[env:ratgdo_esp32dev]` build_flags (intentionally NOT errors — library deps emit some that we can't fix in the fork). Would have caught the v30 `int32_t` cast bug at compile time.
- **`web_loop` SSE fanout buffer stack→BSS.** `localJson[2560]` per-loop stack alloc → `static`. ESP32: 31% of loopTask stack reclaimed; ESP8266 cherry-pick critical (50% of cont-task's ~4 KB).
- **`audit-notes/` gitignored** — local fork-attribution scratch space, not for upstream.

**ESP8266 portability notes (for cherry-pick to `Haglerd/homekit-ratgdo`)**

The skew-detection (`#0`), atomic counter ops (`#9`/`#10`/F1), per-task recursion guard (F3), and same-origin URL parsing (`#2b`) are all fully portable — stdint types, GCC built-ins, and standard C string ops only. The HomeKit watchdog/health-log block, the HomeSpan defer pairs, and the new stack-HWM observability are ESP32-only (depend on HomeSpan, FreeRTOS task handles, and esp_heap_caps). The `handle_status` ESP8266 branch (mutex-held-across-send + defense-in-depth terminator) is the recommended port; the ESP32 dedicated-buffer variant is too heap-expensive for ESP8266.

### v3.4.4-forceclose.30 (2026-05-04)

**Fixed (critical)**
- **SSE orphan sweep was reaping healthy slots due to a uint32 underflow.** Symptom in the wild on v29: `SSE orphan (idle) channel=0 ... idle=4294967294ms — reaping`, repeated continuously seconds after boot. Cause: TOCTOU race between `sweep_sse_orphans` capturing `now` once at the top of the loop and writers (Ticker callbacks for `SSEheartbeat`, `BUFFER_FULL` stamps from `SSEBroadcastState`) updating `subscribedAt` / `lastActivity` *after* the snapshot. When a writer's timestamp was slightly newer than our `now`, `(uint32_t)(now - timestamp)` wrapped to ~4.29 billion ms (`UINT32_MAX - delta`) — a value vastly larger than `SSE_IDLE_TIMEOUT_MS` (300000) or `SSE_PREHANDSHAKE_TIMEOUT_MS` (5000), so a healthy slot was reaped one tick after handshake on a busy device. Fix: cast both age computations to `int32_t` so a future timestamp produces a *negative* age, which fails the `> timeout` comparison and leaves the slot alive — the next sweep tick sees the up-to-date state and applies the real check. Also updates the comment on the `now = (uint32_t)_millis()` line to document the new semantic. The 49.7-day uint32 wraparound itself is still handled correctly by mod-2^32 arithmetic — only the sub-second TOCTOU window changes behaviour.

**ESP8266 portability notes (for cherry-picking to `Haglerd/homekit-ratgdo`)**
- The v30 fix is in `web.cpp::sweep_sse_orphans` and is fully portable — no ESP32-specific APIs. Cherry-pick the `int32_t preAge` / `idleAge` cast lines and the matching `> (int32_t)<TIMEOUT>` comparison change. The bug exists wherever a uint32 timestamp is subtracted from `now` without a signed cast.
- Reviewed the v22-v29 fork delta against the ESP8266 codebase: SSE infrastructure (sweep, drain, unsubscribe, `enforce_same_origin`, `clientWriteEx` tri-state return, BUFFER_FULL flow-control diagnostics) is portable. The HomeKit watchdog + 180-s health log block (`homekit.cpp::homekit_health_log` and the `pairedControllersCount` cache + `hap_controller_change_cb` HomeSpan hook) is ESP32-only — it depends on HomeSpan's `setControllerCallback` and `esp_heap_caps`. ESP8266 builds should skip that block; the watchdog config setters and ~30 B BSS for the cache aren't worth the heap on an ESP8266 anyway.

### v3.4.4-forceclose.29 (2026-05-04)

**Fixed**
- **SSE idle-reap was misclassifying TCP-flow-controlled slots as idle.** Real-world: a `logs.html` viewer over a Tailscale (WireGuard) tunnel had its slot reaped every 120 s in a continuous loop. The v28 Nit-5 fix correctly only stamped `lastActivity` on successful writes — but the `availableForWrite() < len` fast-path at `clientWrite` returned `false` without distinguishing "lwIP send buffer full" (flow control, peer is alive but slow) from "write actually failed." Both paths skipped the stamp; on a chronically-congested tunnel the slot's `lastActivity` never advanced and class-5c reaped a healthy slot. Fix: `clientWrite` returns `SseWriteResult` (OK / BUFFER_FULL / FAILED). Callers stamp `lastActivity` on OK or BUFFER_FULL — both mean "broadcast loop reached this slot and tried." Only FAILED (`client.write` returned 0 after lwIP accepted bytes for delivery) skips the stamp. The `printf` fallback paths in `SSEBroadcastState` keep their `pwrote > 0` gate — different layer, that IS the wedge signal we want exposed.
- 5b protection (`client.connected() == false`) is unchanged and is the actual safety net for "TCP socket dead, lwIP cached state stale." 5c was always belt-and-suspenders for that case. A truly wedged slot still gets reaped by 5b once lwIP KeepAlive fires (typically <60s).

**Changed**
- `SSE_IDLE_TIMEOUT_MS` raised 120000 → 300000 (2 min → 5 min). Defense in depth on top of the BUFFER_FULL stamping fix above. 5 min is past most NAT-binding refresh intervals; a genuinely-wedged slot still gets caught well before users notice stale data.

**Added**
- `sseBufferFullSkips` health-log counter. Counts `availableForWrite() < len` skips since boot. Lets us see at a glance whether a subscriber is chronically flow-controlled vs. occasionally — drives whether v30 needs per-slot adaptive timeouts.

### v3.4.4-forceclose.28 (2026-05-04)

**Fixed (critical)**
- **The actual root cause of "no free slots available" wedge.** The SSE-subscribe free-slot scan at `web.cpp:2003` was `if (!subscription[channel].clientIP)`, which (via `IPAddress::operator bool()`) is true ONLY when the address is 0.0.0.0 — NOT when it's `INADDR_NONE` (0xFFFFFFFF). But `setup_web` and `removeSSEsubscription` mark slots free by setting `clientIP = INADDR_NONE`. The orphan sweep at line 1642 correctly compares `clientIP == IPAddress(INADDR_NONE)`; the subscribe scan disagreed. Net effect: the very first 8 subscribes worked (initial slots have dword=0 from struct init), but every slot subsequently freed via `removeSSEsubscription` was permanently invisible to the scan. After all 8 slots had been used and freed once, every new subscribe returned 503 "no free slots" — even though the sweep correctly reported `sseSlotsAlloc=0`. Pre-existing bug all the way back to v22; v22-v26 hit it identically but the v22 SSE deadlock crashed the device before slot 9 was attempted, masking the symptom. v27's deadlock fix exposed it. Scan now compares `== IPAddress(INADDR_NONE)` to match the canonical "free" marker; same fix applied to `handle_unsubscribe`'s slot-lookup.

**Fixed (audit findings on v27)**
- **`SSESubscription.subscribedAt` and `lastActivity` were `int64_t` tearing risk.** Changed to `volatile uint32_t` (truncated `_millis()` cast) — eliminates race between sweep (main loop) and writers in Ticker / SSEBroadcastState. Wrap-safe subtraction handles ~49.7-day rollover; intervals (15s/120s) fit comfortably in 32 bits.
- **`handle_unsubscribe` missing `enforce_same_origin`.** v27 comment claimed sendBeacon can't set custom headers — true for X-* headers, but browsers DO populate Origin/Referer/Host on sendBeacon POSTs (which is what `enforce_same_origin` actually checks). Added the guard; blocks drive-by cross-origin closes without breaking legitimate beacons.
- **`/reset` and `/reboot` missing same-origin guards.** v23 added `enforce_same_origin` to `/setgdo`, `/reconnectHomeKit`, `/refreshHomeKitMDNS`, `/dumpHomeKitState`. `/reset` (un-pair + reboot) and `/reboot` (full reboot) were missed — a cross-origin LAN page could trigger either with a single sendBeacon. Both now consistent. `/reboot` additionally gained `AUTHENTICATE()` (no-op when no www password set, enforces password otherwise — matches `/reset`).
- **Printf-path `lastActivity` stamps now gated on success.** `SSEBroadcastState`'s oversized-message printf fallback was stamping `lastActivity` regardless of write success, masking the orphan-sweep idle check (5c) by ~3min on a wedged subscriber. Captures `printf` return; only stamps when bytes were actually written.
- **Firmware-update SSE write now stamps `lastActivity` on success.** Was the one v27 SSE write site that didn't refresh the timestamp. Negligible in practice (updates run <30s) but matches the rest of the SSE write paths.

**Changed (storm robustness)**
- **`SSE_PREHANDSHAKE_TIMEOUT_MS` reduced from 15000 → 5000.** Real-world reconnect storm observed where browser EventSource auto-retry filled all 8 slots faster than the 15s sweep could drain them. 5s is well above the typical EventSource handshake (<500ms even on cellular) — slow legitimate clients just hit a fresh subscribe retry, but storms can't outpace the sweep.
- **`functions.js` (home page) gets the same UUID-localStorage + sendBeacon-on-unload + `heartbeat=10` treatment v27 applied to logs.js.** Home page reload no longer leaks a fresh slot per load.

**Changed (consistency)**
- **`isPaired` annotated `volatile`** for cross-context consistency with `hapLastReadSec` / `pairedControllersCount`. No behavior change — single-byte writes already atomic on ESP32; volatile just prevents compiler hoisting.



### v3.4.4-forceclose.27 (2026-05-02)

**Fixed**
- **SSE slot leak — device wedges 25s after every boot, `logs.html` unreachable.** Four interacting bugs:
  1. **`subscriptionCount` desync.** `handle_subscribe()` incremented the counter mid-validation. Subsequent rejection paths (heartbeat range, low heap, dead client) returned without decrementing, so the counter drifted up over time and falsely tripped the "no free slots" capacity check while real slots sat free. Fix: all validations moved before the slot/counter mutations; counter only bumps after every rejection path is exhausted.
  2. **No liveness driver for `heartbeat=0` clients.** `logs.html` subscribed with `heartbeat=0`, which produced a slot with no `Ticker` running. The only cleanup path (`SSEheartbeat()` failing 5x → `pendingRemove`) never fired, so `logs.html` slots leaked forever once the page navigated away. Fix: server-side coerce `heartbeat=0 → 30` and add an orphan sweep that runs from `service_timer_loop` independent of the Ticker.
  3. **No pre-handshake timeout.** A slot allocated by `handle_subscribe()` but whose `EventSource` never came back to `/events/N` (browser closed mid-flight, GET hung) had no timeout — it sat as `clientIP=set, SSEconnected=false` until the next reboot. Fix: orphan sweep reaps any pre-handshake slot older than 15s.
  4. **No idle timeout for connected slots.** A slot whose TCP socket dropped without an RST or whose subscriber stopped reading (`client.connected()` still true, no broadcast ever fails) was never cleaned up. Fix: orphan sweep reaps connected slots idle for >120s.
- **`controllers=0` cosmetic in health log.** HomeSpan does not invoke `setControllerCallback` for pairings loaded from NVS at boot — only for live add/remove events. The health log reported `controllers=0` from boot until the next live pairing change (often forever). Fix: explicitly call `hap_controller_change_cb()` once at the end of `setup_homekit` to seed the cached count.

**Added**
- `POST /rest/events/unsubscribe?id=UUID` — best-effort beacon endpoint. `logs.js` calls this via `navigator.sendBeacon()` on `beforeunload`, releasing the SSE slot immediately on page navigation instead of waiting for the orphan sweep timeout. No auth, no CSRF (sendBeacon can't set custom headers; worst case is closing your own session). Browsers don't guarantee delivery — the orphan sweep is still the authoritative cleanup path.
- Low-heap rejection at the top of `handle_subscribe()`. New SSE subscriptions are refused with 503 when free heap is below 16KB. Stops the cascade where heap pressure → write failures → leaked slots → more heap pressure.
- New SSE health-log fields: `sseSlotsAlloc` (live snapshot of allocated slots, refreshed every service tick) and `sseOrphansReaped` (count of slots reaped by the sweep this 180s window). `sseSlotsAlloc=8 + sseOrphansReaped=0 + new clients getting "no free slots"` = sweep is broken.

**Changed**
- `logs.js` now persists its SSE client UUID in `localStorage` (`ratgdo-logs-uuid`). Pre-v27 every page reload generated a fresh UUID, which combined with the orphan sweep's 15s pre-handshake timeout meant ~6 fast reloads could fill all 8 channels. Falls back to a per-session UUID if storage is blocked (iOS private browsing).
- `logs.js` heartbeat changed from `0` → `10`. Keeps `lastActivity` fresh so the connected client never trips the 120s idle reap, and gives `SSEheartbeat()` a Ticker for class-5b cleanup.

**Note: missing v23-v26 CHANGELOG entries.** Versions 23 through 26 shipped without a CHANGELOG entry at the time. Quick recap — v23 added the same-origin/CSRF guard for state-changing endpoints + the auto-close reschedule deferral + watchdog config defensive clamps; v24 added SSE write-side `SO_SNDTIMEO`, the `clientWrite` slow-write counter + `availableForWrite` fast-path, the `pairedControllersCount` cache, and the `homekit_drain_pending_reconnect` deferral; v25 fixed an `INADDR_NONE` overload ambiguity that surfaced after a header-include shuffle; v26 was a release-pipeline-only change. Source-of-truth is the git log on `main` for those tags.

### v3.4.4-forceclose.22 (2026-05-02)

**Fixed**
- **SSE self-detach crash in `loopTask`** — `SSEheartbeat()` called `removeSSEsubscription()` from inside the Ticker callback, which `Ticker.detach()`'d its own running heartbeat → `vTaskDelete` on the Ticker's task → `uxListRemove` panic. Long-running `logs.html` sessions would eventually hit this on any version. Fix: `SSESubscription.pendingRemove` flag set from inside the callback, drained from `service_timer_loop()` in main-loop context.
- Auto-close: ticker now scheduled window-aware. Outside the configured window, no recurring tick — a one-shot timer sleeps until window-start, then the 60s tick attaches. Re-armed on `/setgdo` settings save. Skip-paths are silent (no more `AUTO-CLOSE: tick #N — door not Open, skipping` every 60s).
- HomeKit watchdog config now cached at boot and on settings save instead of taking the `userConfig` mutex inside the Ticker callback every 60s.

**Changed**
- `/status.json` request log demoted `ESP_LOGI → ESP_LOGD`. The Homebridge plugin polls every 3s by default; the previous INFO line was ~20/min of pure noise. The 95%-buffer-full WARN stays at WARN (still actionable).
- HomeKit / WiFi / HomeSpan log lines are now **exclusive to the HomeKit tab** in `logs.html` — they no longer also clutter the System Log tab.
- Health log interval bumped 60s → 180s (purely diagnostic; doesn't need 1-min resolution).

### v3.4.4-forceclose.21 (2026-05-02)

**Added**
- **HomeKit watchdog settings UI** — Settings page now exposes a toggle (`hkAutoRecover`) and four threshold inputs (`hkAutoRecoverSecs`, `hkHintQuietSecs`, `hkHintStaleSecs`, `hkHintLikelyNRSecs`). Replaces the v19/v20 compile-time constants. Defaults preserve v19/v20 behaviour exactly: auto-recover OFF, 5/15/30-minute hint tiers, 30-minute trigger.

### v3.4.4-forceclose.20 (2026-05-02)

**Fixed**
- CI: replaced 5 separate `wow-actions/download-upload` Contents-API commits with a single `git checkout -fB main origin/main` + `git push`. Branch protection ("Block force pushes / Restrict deletions") was timing out the Contents-API rule eval, leaving `docs/firmware/` stale by one release on every build (v18 + v19 both shipped without bins committed). The single direct push works inside a 1s window.

**Changed**
- New `auto-release.yml` — fires on push to `main` that touches `docs/manifest.json`. Reads version, creates tag + release, dispatches `release.yml`. Net effect: bumping `docs/manifest.json` on a feature branch + merging the PR runs the entire release pipeline hands-free.

### v3.4.4-forceclose.19 (2026-05-02)

**Changed**
- HomeKit watchdog ships **disabled by default** with tiered diagnostic hints. Real-world iOS read cadence is highly variable (gaps of 6+ min observed during normal idle), so a low fixed threshold caused false-trigger recoveries on healthy connections. v19 keeps the threshold-checking logic and emits hint logs at 5/15/30 min, but no recovery action runs unless the user opts in.

### v3.4.4-forceclose.18 (2026-05-02)

**Added**
- **HomeKit self-healing watchdog** — periodic health check (every 60s) tracking `last_hap_read_ago`. When iOS goes silent past the trigger threshold AND WiFi is connected AND we have paired controllers, escalates: mDNS refresh first (cheap, no outage), then WiFi reconnect (~5s outage). Stops after 2 attempts; never auto-reboots.

### v3.4.4-forceclose.17 (2026-05-02)

**Fixed**
- `clear_force_close_state()` was firing on every `CURR_OPEN` / `CURR_CLOSED` / `CURR_STOPPED` status poll, wiping `forceCloseAttempt` mid-sequence. Symptom: "attempt 0 release sent" then full sequence ran twice. Removed the over-defensive terminal-state cleanup; kept only the `CURR_CLOSING` transition fix from .16.

**Added**
- HomeKit recovery buttons on home page (Refresh mDNS / Reconnect HomeKit / Dump State) in addition to the logs.html ones.

### v3.4.4-forceclose.16 (2026-05-02)

**Fixed**
- `forceCloseInProgress` flag leak — fork's force-close override could get stuck "in progress" if the firmware took an unexpected path during the 2-press sequence, blocking subsequent close commands until reboot. Added `clear_force_close_state()` triggered on `CURR_CLOSING` transition (the door reached the desired state — clear our in-progress marker).

**Added**
- HomeKit visibility (WiFi connect/disconnect events with reason codes, periodic 60s health log, three HomeSpan callbacks tracking `hapLastReadSec`).
- Recovery endpoints: `POST /reconnectHomeKit` (cycles WiFi, HomeSpan re-attaches), `POST /refreshHomeKitMDNS` (re-broadcast mDNS without dropping WiFi), `POST /dumpHomeKitState` (dump HomeSpan CLI status / accessory DB / diag to log).
- New "HomeKit" tab in `logs.html` with three recovery buttons + filtered HomeKit/WiFi/HomeSpan event view.

### v3.4.4-forceclose.15 (and earlier)

Initial fork releases adding the `forceClose` HTTP primitive (single POST → 2-press hold-to-close override at the Sec+1.0 protocol level), the firmware-side auto-close timer (with optional time-of-day window), security hardening (input validation, CSRF on `/setgdo`, busy-flag guard on force-close), and basic UI polish (widened time inputs, fork README header).

---

## Upstream releases (`v3.x.x`)

The following are upstream `ratgdo/homekit-ratgdo32` releases the fork tracks via daily auto-sync. Each fork release includes everything from upstream `v3.4.4` plus the fork-specific changes above.

## v3.4.4 (2026-02-??)

### What's Changed

* Bugfix: Inform browser whenever IP address is set or changed
* Bugfix: Fix setting keyname mismatch between browser and server (homekitLight / lightHomeKit)
* Feature: Allow user to set NTP server URL
* Other: ESP32 only, update HomeSpan library to version 2.1.7

### Known Issues

* ESP32 (ratgdo32) only... Some users may get an error during OTA upload that firmware is too large for the OTA partition. See [README.md](https://github.com/ratgdo/homekit-ratgdo32/blob/main/README.md#upgrade-failures) for work-around.
* Sec+ 1.0 doors with digital wall panel (e.g. 889LM) sometimes do not close after a time-to-close delay. Please watch your door to make sure it closes after TTC delay.
* Sec+ 1.0 doors with "0x37" digital wall panel (e.g. 398LM) not working.  We now detect but will not support them. Recommend replacing with 889LM panel.
* When creating automations in Apple Home the garage door may show only lock/unlock and not open/close as triggers. This is a bug in Apple Home. Workaround is to use the Eve App to create the automation, it will show both options.

## v3.4.3 (2026-01-11)

### What's Changed

* Bugfix: (Sec+2.0 only) door not closing if ratgdo thinks it is still opening (rightly or wrongly). https://github.com/ratgdo/homekit-ratgdo32/issues/131
* Bugfix: Escape backslash and double quotes inside JSON strings.  https://github.com/ratgdo/homekit-ratgdo32/issues/134
* Bugfix/feature: (Sec+2.0 only) allow user to select sending TOGGLE command instead of CLOSE. https://github.com/ratgdo/homekit-ratgdo32/issues/131
* Feature: Hardwired Sec+ GPIO Controls Mirror Wall Panel, Optional TTC Bypass. https://github.com/ratgdo/homekit-ratgdo32/pull/136
* Feature: Publish ratgdo and door status over mDNS
* Other: Update settings page visuals to disable/enable options rather than hide/show.

### Known Issues

* ESP32 (ratgdo32) only... Some users may get an error during OTA upload that firmware is too large for the OTA partition. See [README.md](https://github.com/ratgdo/homekit-ratgdo32/blob/main/README.md#upgrade-failures) for work-around.
* Sec+ 1.0 doors with digital wall panel (e.g. 889LM) sometimes do not close after a time-to-close delay. Please watch your door to make sure it closes after TTC delay.
* Sec+ 1.0 doors with "0x37" digital wall panel (e.g. 398LM) not working.  We now detect but will not support them. Recommend replacing with 889LM panel.
* When creating automations in Apple Home the garage door may show only lock/unlock and not open/close as triggers. This is a bug in Apple Home. Workaround is to use the Eve App to create the automation, it will show both options.

## v3.4.2 (2025-12-13)

### What's Changed

* Feature: Query the state of emergency back up battery on boot and every 55 minutes (Sec+2.0 only).
* Feature: User can select whether to create HomeKit accessories for motion sensor and light switch (ratgdo32 only). With thanks to https://github.com/DaveLinger
* Bugfix: If firmware upload error detected before update begins, do not require a reboot.
* Other: Average vehicle distance over larger sample size (now 50) to smooth out spurious readings (ratgdo32-disco only).

## v3.4.1 (2025-11-22)

### What's Changed

* Bugfix: Vehicle departing motion sensor may not trigger correctly (ratgdo32-disco only)
* Bugfix: re-Announce ratgdo mDNS every two minutes, so that we remain visible on network (default TTL is 2 minutes)
* Bugfix: The home icon at top/right of the system logs page was not always returning to ratgdo main page https://github.com/ratgdo/homekit-ratgdo/issues/318
* Feature: Add a [webmanifest](https://developer.mozilla.org/en-US/docs/Web/Progressive_web_apps/Manifest) file and update all browser favorite icons for better visuals
* Feature: Add support for Captive Network Assistant (CNA) so that Apple and Android devices will automatically load WiFi provisioning page when connecting to ratgdo Soft Access Point (Soft AP mode)
* Feature: Add a warning and countdown timer to web page when Sec+2.0 doors have automatic door close (TTC) active
* Other: Display "Off" instead of "0" when settings sliders are set to zero seconds/minutes
* Other: Improved web page design for iPhone and iPad devices
* Other: Attempt to recover from out-of-sync Sec+2.0 rolling code https://github.com/ratgdo/homekit-ratgdo/issues/315
* Other: HomeSpan library updated to version 2.1.6 (ratgdo32 only)
* Other: Various log message cleanup to make debugging easier and reduce log clutter at default Info level

## v3.4.0 (2025-11-01)

### What's Changed

* Bugfix: User selected syslog facility not restored on startup. https://github.com/ratgdo/homekit-ratgdo32/issues/116
* Bugfix: Crash when HomeKit tries to open or close a dry contact door. https://github.com/ratgdo/homekit-ratgdo32/issues/117
* Bugfix: Sec+2.0 only, not handling packet transmit errors during initialization
* Feature: Sec+2.0 only, support garage door automatic close after selected delay, [SEE README](https://github.com/ratgdo/homekit-ratgdo32/blob/main/README.md#automatic-close)
* Feature: Add time-to-close countdown timer to web page
* Other: Allow user to disable triggering motion from Sec+2.0 wall panel motion sensors
* Other: Ratgdo-disco only, update vehicle presence algorithm for no vehicle present to match ESPhome
* Other: Adjust some Info-level log messages to Debug- or Error-level... reduces log clutter at default Info level

## v3.3.9 (2025-10-24)

### What's Changed

* Bugfix: Update function that calculates median door open/close duration. https://github.com/ratgdo/homekit-ratgdo/issues/309
* Bugfix: Do not cancel time-to-close if second door close request received. https://github.com/ratgdo/homekit-ratgdo32/issues/112
* Other: Ratgdo-disco only, update vehicle presence algorithm to match ESPhome https://github.com/ratgdo/esphome-ratgdo/pull/496
* Other: Additional Serial CLI commands for development and debugging to e.g, provision WiFi SSID and password.

## v3.3.8 (2025-10-19)

### What's Changed

* Bugfix: dry contact doors not reporting status correctly on web page. https://github.com/ratgdo/homekit-ratgdo32/issues/109
* Bugfix: Sec+1.0 add timeout when waiting for GDO reply to poll commands https://github.com/ratgdo/homekit-ratgdo32/issues/111
* Other: Save door open/close durations so not reset to unknown on a reboot
* Other: Add timers to check that door starts to open/close and reaches fully opened/closed state in expected time
* Other: Sec+2.0 use MotorOn packet to error correct if we miss notification packet of door opening or closing
* Other: Add serial CLI commands to scan WiFi networks and reset door ID & rolling codes
* Other: Remove known issues list from prior versions in CHANGELOG.md... because they are now repeating

## v3.3.7 (2025-10-12)

### What's Changed

* Bugfix: Sec+2.0 doors not opening or closing. Issue https://github.com/ratgdo/homekit-ratgdo/issues/305

## v3.3.6 (2025-10-12)

### What's Changed

* Bugfix: HomeKit pairing failed with out-of-compliance error. https://github.com/ratgdo/homekit-ratgdo/issues/300
* Bugfix: Web wage status occasionally getting out-of-sync with actual light/lock/door state.
* Bugfix: Door open/close duration calculation not handling cases where door reverses before reaching open/close state.
* Bugfix: Setting syslog port not taking effect until after reboot. https://github.com/ratgdo/homekit-ratgdo/issues/304
* Bugfix: do not attempt to act on Sec+2.0 packet that failed to decode. Issue https://github.com/ratgdo/homekit-ratgdo32/issues/106
* Feature: Change WiFi and MDNS hostname when user changes GDO name. https://github.com/ratgdo/homekit-ratgdo32/issues/93
* Feature: Add clipboard copy icon to IP address and mDNS name.
* Other: Detect (but do not support) 0x37 wall panels like LiftMaster 398LM. https://github.com/ratgdo/homekit-ratgdo32/issues/95
* Other: ESP8266 (original ratgdo) only... suspend GDO communications during HomeKit pairing process.
* Other: ESP8266 (original ratgdo) only... move more constants into PROGMEM and optimize use of system stack.

## v3.3.5 (2025-09-28)

### What's Changed

* Bugfix: Add error handling for a blank SSID... force boot into Soft AP mode, https://github.com/ratgdo/homekit-ratgdo/issues/295
* Bugfix: Buffer overrun that caused Improv setup to fail, https://github.com/ratgdo/homekit-ratgdo/issues/298
* Bugfix: Aog messages that are truncated for exceeding buffer size not null terminated
* Other: Add simple serial console CLI (when HomeSpan CLI disabled) to allow setting debug level, displaying saved logs and request reboot.

## v3.3.4 (2025-09-27)

### What's Changed

* Bugfix: Activity LED blink was not obeying user preferences (e.g. always off). https://github.com/ratgdo/homekit-ratgdo/issues/292
* Bugfix: Ignore implausibly long door opening / closing times. https://github.com/ratgdo/homekit-ratgdo32/issues/98
* Bugfix: Room occupancy state not set on motion (ratgdo32 only). https://github.com/ratgdo/homekit-ratgdo32/issues/96
* Feature: Allow user selection of Syslog facility number (Local0 .. Local7). https://github.com/ratgdo/homekit-ratgdo32/issues/94
* Feature: Add MDNS service for _http._tcp to allow local network name discovery. https://github.com/ratgdo/homekit-ratgdo32/issues/93
* Other: Disable HomeKit and garage door communications during OTA firmware update.
* Other: Miscellaneous stability improvements.

## v3.3.3 (2025-09-20)

### What's Changed

* Bugfix... Date and time on web page now displayed in the time zone of the server (NTP server feature must be enabled).
* Bugfix... Log level was not getting set correctly (should take effect immediately but it required a reboot).
* Bugfix... Park assist laser was not activating on vehicle arrival (since v3.3.0).
* Bugfix... Displaying last reboot log was missing time stamps.
* New Feature... last door open and close date and time is displayed under opening/closing status (NTP server feature must be enabled).
* Other... Add a "home" button to system logs page because iOS and iPad OS 26 have removed the "done" button.

## v3.3.2 (2025-09-14)

### What's Changed

* Other... Add release notes to firmware update dialog.
* Other... Reduce unnecessary network traffic during firmware update

## v3.3.1 (2025-09-13)

### What's Changed

* Bugfix... Updating from GitHub was not finding the firmware download file.

## v3.3.0 (2025-09-13)

Version 3.3.0 is a significant upgrade for ESP32-based ratgdo boards. Almost all source files for the ESP8266 and ESP32 versions of ratgdo have been merged which results in minor changes to the underlying features and function for ESP32 versions. The main benefit is for the original ESP8266-based ratgdo boards.

While source files have been merged there remain significant differences between the two board types, most notably in the library used to communicate with HomeKit which are completely different.

* Before an Over-The-Air (OTA) upgrade it is good to first reboot your current version.

### What's Changed

* Other... Significant source code changes to support move towards single code base for ESP8266 and ESP32
* Other... Change garage door communications from using GDOLIB library to same code as used for ESP8266

## v3.2.1 (2025-07-??)

### What's Changed

* Bugfix: IPv6 support should now be working (updated HomeSpan to release 2.1.3).
* Bugfix: Boot safely when no GDO connected (updates GDOLIB to release 1.2.1).

## v3.2.0 (2025-07-04)

### What's Changed

* Bugfix: Doors without obstruction sensor may not respond to door action CLOSE commands, use TOGGLE instead (Issue #81).

## v3.1.12 (2025-06-15)

### What's Changed

* Bugfix: Sec+ 1.0 doors may not close at end of time-to-close delay (Issue #77).

## v3.1.11 (2025-06-13)

### What's Changed

* Feature: Make IPv6 optional, disabled by default.

## v3.1.10 (2025-06-08)

### What's Changed

* Feature: Enable IPv6 (PR #74) with thanks to https://github.com/apearson

## v3.1.9 (2025-05-11)

### What's Changed

* Bugfix: Hostname not set correctly to comply with RFC952 (Issue #72)
* Bugfix: Door close duration not reported correctly on web page
* Other: Add console CLI command to allow setting debug message log level
* Other: Add console CLI command to facilitate testing moving door to position between 0% and 100%
* Other: Update to use version 2.1.2 of upstream HomeSpan library
  
## v3.1.8 (2025-04-27)

### What's Changed

* Bugfix: show motion status and controls when any motion trigger type is set
* Bugfix: room occupancy and motion reset time not working as expected

### Known Issues

* Still testing... Future updates MAY include breaking changes requiring a flash erase and re-upload.

## v3.1.7 (2025-04-27)

### What's Changed

* Feature: Allow user setting to disable light flashing during time-to-close
* Feature: Add a HomeKit occupancy sensor triggered by motion that is active for user specified duration between 0 and 120 minutes, Requested in issue #45
* Bugfix: Do not run setup function(s) if already run.  Hope that this fixes issue #51
* Bugfix: Obstruction sensor not triggering HomeKit motion, fix issue #65
* Other: Additional error checking and changes to log verbosity for some messages.

### Known Issues

* Still testing... Future updates MAY include breaking changes requiring a flash erase and re-upload.

## v3.1.6 (2025-04-01)

Version 3.1.x include major change to garage door opener (GDO) communications.  This has major benefit in separating out the details of communicating with the garage door from HomeKit and the ratgdo user interface, greatly simplifying our code.

### What's Changed

* Feature: Allow user setting for dry contact debounce timer from 50ms to 1000ms
* Bugfix: HomeKit door status not always correct, reported in issue #57
* Bugfix: Fis time-to-close light flashing to work as intended (flash every 1/2 second)

## v3.1.5 (2025-03-31)

Version 3.1.x include major change to garage door opener (GDO) communications.  This has major benefit in separating out the details of communicating with the garage door from HomeKit and the ratgdo user interface, greatly simplifying our code.

### What's Changed

* Bugfix: Dry contact wall panel button not working, fixes issue #57

### Known Issues

* Still testing... Future updates MAY include breaking changes requiring a flash erase and re-upload.

## v3.1.4 (2025-03-30)

Version 3.1.x include major change to garage door opener (GDO) communications.  This has major benefit in separating out the details of communicating with the garage door from HomeKit and the ratgdo user interface, greatly simplifying our code.

### What's Changed

* Feature: Allow selection between software serial port emulation and hardware UART.  May help with issue #48
* Feature: Add debug terminal command to print out buffered message log
* Bugfix: Default to always use ratgdo's own timer for door time-to-close (TTC) and not the TTC built-in to garage door opener.  Partially fixes issue #50
* Bugfix: Remove user interface setting for built-in time-to-close, also addresses issue #50
* Bugfix: Garage door opening/closing status not reported correctly to HomeKit, fixes issue #53
* Buffix: Sec+ 1.0 garage door was unexpectedly opening on reboot when switching between hardware UART and s/w serial
* Other: Add additional debug and error checking to assist with fixing issue #51

### Known Issues

* Still testing... Future updates MAY include breaking changes requiring a flash erase and re-upload.

## v3.1.0 - v3.1.3 (2025-03-07)

Version 3.1.x include major change to garage door opener (GDO) communications.  This has major benefit in separating out the details of communicating with the garage door from HomeKit and the ratgdo user interface, greatly simplifying our code.

As this is a major change, thorough testing is required.

### What's Changed

* Feature: Updated [HomeSpan](https://github.com/HomeSpan/HomeSpan) to version 2.1.1
* Feature: Use new [library](https://github.com/dkerr64/gdolib) for garage door communications
* Feature: For Sec+2.0 use GDO's built-in Time-to-Close (TTC).  Option on user interface to disable and use ratgdo timer.
* Feature: Add user option to disable HomeKit motion and occupancy accessories for vehicle presence.
* Feature: Add door opening and closing duration to web page (calculated first time door operated after reboot).
* Feature: Added message log level to user interface, defaults to INFO.
* Bugfix: Dry contact should be working now, fixes issue #41
* Other: Replace all our RINFO()/RERROR() message log macros with standard ESP_LOGx() macros.

Note: The built-in TTC feature operates slightly differently than ratgdo's timer.  If you set the TTC to e.g. 20 seconds, then on requesting a door close it will wait, in silence, for 20 seconds then sound its internal beeper and flash the lights for 10 seconds before closing the door... thus 30 seconds before the door closes.  The TTC timer in ratgdo will immediately sound its buzzer and flash lights, and close the door after 20 seconds.

### Known Issues

* Still testing... Future updates MAY include breaking changes requiring a flash erase and re-upload.

## v3.0.7 (2025-02-07)

### What's Changed

* Feature: Report door open/close cycle count and emergency backup battery charging/full status to web page (Sec+2.0 only).
* Bugfix: Problem with time-to-close and Sec+1.0 on doors with digital wall panel, issue #35
* Other: Support logging of ESP_LOGx() messages through our logger facility.

### Known Issues

* Still testing... Future updates MAY include breaking changes requiring a flash erase and re-upload.

## v3.0.6 (2025-01-17)

### What's Changed

* Bugfix: Crash in esp_timer.c possibly related to multi-thread use of Ticker for LED timer (Issue #30).
* Bugfix: Motion sensor status should not show if there is no motion sensor.
* Bugfix: Crash reported at end of time-to-close delay (Issue #32).
* Bugfix: Could not set vehicle threshold above 200cm
* Feature: Set default time-to-close delay to 5 seconds and add warning if user selects lower value (Issue #33).
* Feature: Dynamically create HomeKit QR setup ID, and QR code graphic for pairing.

### Known Issues

* Still testing... Future updates MAY include breaking changes requiring a flash erase and re-upload.

## v3.0.5 (2025-01-05)

### What's Changed

* Feature: Add user setting to enable/disable parking assist laser, and set duration of assist laser.
* Feature: Added support to save and view message log on crash (Issue #2).
* Bugfix: Remove multiple copies of web page content from the firmware binary.
* Bugfix: Use 64-bit integer to handle milliseconds since last boot.
* Bugfix: Handle more possible return codes from vehicle distance sensor.
* Bugfix: Door status incorrectly reported to HomeKit if close requested for already closed door (Issue #28).
* Bugfix: Last door change date/time was not always been set correctly after reboot.
* Bugfix: Activity LED options not correctly shown in web page.
* Bugfix: Activity LED constantly on with Sec+ 1.0 protocol.
* Bugfix: Remove wait for incoming serial packet before starting to detect Sec+ 1.0 digital wall panel.
* Updated copyright statement(s) to include year 2025.

### Known Issues

* Still testing... Future updates MAY include breaking changes requiring a flash erase and re-upload.

## v3.0.4 (2024-12-21)

### What's Changed

* Feature: Add HomeKit light switch and web page button for parking assist laser
* Feature: Change web page separated buttons for on/off, open/close, etc. to single buttons

### Known Issues

* Still testing... Future updates MAY include breaking changes requiring a flash erase and re-upload.

## v3.0.3 (2024-12-19)

### What's Changed

* Bugfix: Soft AP list of available WiFi networks not properly terminated
* Bugfix: Setting static IP address did not set correct subnet mask
* Bugfix: Crash in soft AP because memory buffer was not allocated
* Feature: Add support Improv-based WiFi provisioning
* Feature: Add the 10 minute timeout on soft AP mode
* Feature: Add the check for WiFi connectivity 30 seconds after change to static IP

### Known Issues

* Still testing... Future updates MAY include breaking changes requiring a flash erase and re-upload.

## v3.0.2 (2024-12-16)

### What's Changed

* Feature: Dry contact support, with thanks to @tlhagan
* Feature: Add support for HomeKit Identify characteristic
* Feature: Support for ratgdo32 as well as ratgdo32-disco
* Bugfix: Vehicle distance sensor logic

### Known Issues

* Still testing... Future updates MAY include breaking changes requiring a flash erase and re-upload.

## v3.0.1 (2024-12-11)

### What's Changed

* Feature: Beeps during time-to-close delay
* Feature: Vehicle presence, arriving, departing sensing, and parking assist laser
* Bugfix: Web-based flash installer now working
* Bugfix: OTA update from GitHub now working
* Bugfix: Blue LED was not blinking
* Bugfix: Device name not initialized to default on startup

### Known Issues

* Still testing... Future updates MAY include breaking changes requiring a flash erase and re-upload.

## v3.0.0 (2024-11-30)

### What's Changed

* New release for Ratgdo32 - DISCO

### Known Issues

* THIS IS PRE-RELEASE FIRMWARE for testing purposes. Future updates MAY include breaking changes requiring a flash erase and re-upload.

