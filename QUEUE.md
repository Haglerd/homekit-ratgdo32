# homekit-ratgdo32 work queue

Priority-ordered. Top = next. Detailed analysis lives in `audit-notes/` (gitignored fork-internal).

## Active — fork-internal feature work

### [P2] ~~HK-FC~~ — Native HomeKit Force-Close GarageDoorOpener accessory
**Status:** DONE — PR https://github.com/Haglerd/homekit-ratgdo32/pull/82 (branch `homekit-force-close-accessory`)
**Source:** user request 2026-05-06; planner produced full layered plan
**Issue:** Haglerd/homekit-ratgdo32#79 (closed via PR)
**Acceptance:** New optional `forceCloseHomeKit` toggle (default OFF) registers a second `GarageDoorOpener` accessory; Open mirrors normal open; Close fires `door_command_force_close(<configured hold ms>)`; state mirrors primary in lockstep. Web UI checkbox + `forceCloseHoldMs` number input (clamped 1000-10000). 307 LoC across 10 files. **Build measured: BSS +8 B (toggle OFF) — well under 100 B halt threshold**; Flash 96.0% (~80 KB headroom). ESLint clean.
**Notes:** Enables eventual deprecation of `homebridge-ratgdo-forceclose` npm plugin. ESP32-only; ESP8266 short-circuited via `#ifdef`. Force-close FSM untouched (only call site reused). User-driven smoke-test (10 steps in issue body) deferred to next device flash window — covers tile pair behavior, mirror correctness, primary-tile non-regression, OTA-from-default-OFF compatibility.

---

## Active — fork-internal (W41-W48 audit cleanup)



---

## Active — log-audit findings (2026-05-06)



## Active — log-audit findings (2026-05-07)

### [P0] ~~log-audit-20260507-005~~ — BOOT-OOM-MDNS recurring on .65 (PR #89 fix INCOMPLETE — wrong code path)
**Status:** done — PR https://github.com/Haglerd/homekit-ratgdo32/pull/105 (merged 2026-05-07)
**Acceptance:** root cause was NOT mDNS OOM. addr2line on the .65 stack trace showed the panic was in `tiT` task at `_strerror_r` from `__assert_func`, with the chain `mdns_mem_calloc` → `ESP_LOGE` → our `esp_log_hook` → `LOG::logToBuffer` → `logToSyslog` → `WiFiUDP::beginPacket` → `socket()` from inside `tcpip_thread`. lwIP's socket API forwards every call as a tcpip-message to `tcpip_thread` and waits for a reply; if the caller IS `tcpip_thread`, the wait is for itself → `LWIP_ASSERT` → panic. PR #89 deferred mDNS *service registration* but the crash had nothing to do with service-registration timing. Fix in PR #105: cache `tiT`/`wifi`/`sys_evt` task handles in `LOG::logToBuffer`, skip `SSEBroadcastState` + `logToSyslog` calls when originating task matches. Belt-and-suspenders `xPortInIsrContext()` gate also added.
**Notes:** PR #89's heap-floor service-registration deferral kept (orthogonal heap-pressure mitigation). 24 h soak verification on-device pending after release.71.

### [P2] ~~log-audit-20260507-004~~ — `errno 11 "No more processes"` recurrence beyond browser fan-out (post log-audit-002 fix)
**Status:** done — PR https://github.com/Haglerd/homekit-ratgdo32/pull/94 (merged 2026-05-07)
**Source:** log-audit 2026-05-07 (Pi syslog) — user-surfaced, multi-boot pattern
**Acceptance:** root cause identified — NOT fd-exhaustion; fd 51/52 are long-lived SSE TCP sockets (LWIP_SOCKET_OFFSET=50). errno 11 = EAGAIN. Two underlying bugs fixed: (1) `clientWriteEx` fast-path used `client.availableForWrite()` which is `Print::availableForWrite()`'s default 0 on Arduino-ESP32 (no override in `NetworkClient`) → returned BUFFER_FULL on every call without writing; (2) Oversized broadcasts hit framework's `NetworkClient::write` retry loop which logs `ESP_LOGE` on every benign EAGAIN, up to 10 lines per write. ESP32-only rewrite: direct `lwip_send(MSG_DONTWAIT)` in clientWriteEx + heap-buffered clientWriteEx for oversized payloads. ESP8266 unchanged. Soak verification deferred to next 24 h on-device.
**Notes:** PR #77 (browser-fanout) was a misdiagnosis treating a symptom. fd 51/52 stay across reboots because they're the 1st/2nd lwIP socket allocated post-boot. Force-close FSM untouched.

---

## Deferred — need measurement / soak data first

### [P3] W25 — `web_loop()` 10/sec rate limit on `server.handleClient()`
**Status:** deferred (needs soak data)
**Source:** v39 round-3
**Acceptance:** burst-reconnect-storm soak data demonstrating impact; either remove throttle or scope per-IP.
**Notes:** mechanism real, impact unverified. No field evidence today.

### [P3] W40 — `build_status_json` 11 fork-added fields not using `_C`
**Status:** deferred (needs bandwidth measurement)
**Source:** v39 round-3
**Acceptance:** `iftop`/`tcpdump` measurement of redundant tx; decision to convert or stay.
**Notes:** estimated ~240 KB/h redundant tx per device — worth measuring before paying complexity cost. Likely closes as non-finding under W48 Conclusion A.

---

## Fork-internal investigation items

These are findings whose fork-side fix is partial or unverified. Fork work proceeds regardless of upstream applicability.

_(R-?-fork closed as non-finding — see Recently completed)_

---

## Upstream filing — DO NOT FILE

The fork's bug fixes (R1-R4 in `audit-notes/UPSTREAM_CHERRY_PICK_PLAN.md`) are already shipped in the fork. Their upstream-applicability is tracked in audit-notes/ for awareness only — **fork work proceeds regardless of upstream interest**.

**Hard rule:** No upstream PRs, no upstream issue filings, no cherry-picks to upstream. Issue #170 was filed historically; do not let it set precedent. Future agents reading this queue: if you find yourself drafting a `gh pr create --repo ratgdo/...`, STOP — that's not how this fork operates.

---

## Recently completed

_(roll commits in here as W4x/Rx items land — keep last 10)_

- **log-audit-20260507-005** (P0) — gate `tiT`/`wifi`/`sys_evt` from SSE+syslog re-entry into lwIP. PR #89 (.65 release) was a misdiagnosis. Real cause: mDNS's harmless `ESP_LOGE("Cannot allocate memory")` from `tcpip_thread` ran through our log hook → `logToSyslog` → `socket()` from inside `tiT` → lwIP self-deadlock → `IllegalInstruction` panic. Fix: cache network-task handles, skip broadcast/syslog when current task is one of them. PR https://github.com/Haglerd/homekit-ratgdo32/pull/105 (merged 2026-05-07). 24 h soak pending on-device after release.71.
- **R-?-fork** — `homeSpan.processSerialCommand` thread-safety investigation. HomeSpan exposes `getMutex()` returning the `pollMutex` (`std::shared_mutex`); pollTask holds it during each iteration. Inspected all 4 fork call sites: `handle_reset` ('U' via `homekit_unpair`) and `helperFactoryReset` ('F') both reboot ~500 ms after the call, collapsing the race window; `homekit_dump_state` ('s'/'i'/'d') is read-only — torn-read cosmetic only. Mutex NOT added: pollTask iterations can take seconds, waiting from loopTask could trip the loop watchdog. **Closed as non-finding.** Doc comments added to `homekit_dump_state` and `helperFactoryReset`. PR https://github.com/Haglerd/homekit-ratgdo32/pull/104 (merged 2026-05-07).
- **W44** — auto-close DST mitigation. Verified applicable: `autoCloseInWindow` / `autoCloseSecsUntilNextStart` use `localtime_r` (DST-affected). Cap long-sleep horizon at 30 min in `autoCloseSecsUntilNextStart` so DST drift is bounded instead of ~23 h. PR https://github.com/Haglerd/homekit-ratgdo32/pull/102 (merged 2026-05-07).
- **W48** — `_C` vs raw `JSON_ADD_*` field consistency audit. Conclusion A: split is deliberate. Doc comment + audit-notes table. W40 closes as non-finding. PR https://github.com/Haglerd/homekit-ratgdo32/pull/99 (merged 2026-05-07).
- **W43** — rename file-scope `writeBuffer` → `loopTaskScratchBuf512` + invariant comment block. PR https://github.com/Haglerd/homekit-ratgdo32/pull/98 (merged 2026-05-07).
- **log-audit-20260507-002** — hoist `hkConsecutiveHealthyTicks` increment out of recover-attempts branch so it runs every tick. Type bumped uint8_t → uint32_t to avoid wrap. PR https://github.com/Haglerd/homekit-ratgdo32/pull/96 (merged 2026-05-07).
- **log-audit-20260507-004** — SSE clientWriteEx direct lwip_send rewrite (ESP32-only). Eliminates `errno 11 fail on fd N` syslog noise + fixes silent-broadcast bug from broken `availableForWrite` fast-path. PR https://github.com/Haglerd/homekit-ratgdo32/pull/94 (merged 2026-05-07). 24 h soak verification pending on-device after release.66.
- **BOOT-OOM-MDNS** — defer ratgdo mDNS service registration until heap >= 50 KB (option a from QUEUE candidates). ESP32-only; ESP8266 path unchanged. PR https://github.com/Haglerd/homekit-ratgdo32/pull/89 (merged 2026-05-07). Validates via post-flash syslog: expect `ratgdo mDNS deferred:` then `floor cleared` within 30 s; absence of `mdns_networking: Cannot allocate memory` and tiT IllegalInstruction. >=5 OTA cycle smoke + 24 h soak still pending on-device.
- **log-audit-20260507-003** — `esp_reset_reason()` re-emit after syslog bound — PR https://github.com/Haglerd/homekit-ratgdo32/pull/88 (merged 2026-05-07). Pre-req for BOOT-OOM-MDNS unblocked.
