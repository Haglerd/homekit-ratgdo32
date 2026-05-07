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



### [P3] W43 — `writeBuffer` rename + invariant comment
**Status:** queued
**Source:** audit, v45 plan
**Acceptance:** rename to `loopTaskScratchBuf512` (or similar); add comment block documenting loopTask-only invariant; ESP8266 alias preserved.
**Notes:** zero behavior change. Option-A (per-caller stack buffers) rejected for ESP8266 stack pressure.

### [P3] W44 — DST spring-forward edge case in auto-close schedule
**Status:** queued — verification gate first
**Source:** audit, v45 plan
**Acceptance:** EITHER cap `autoCloseTicker` wake horizon at 30 min (option-A) OR drop the commit if `autoCloseInWindow()` compares in UTC seconds (verified non-applicable).
**Notes:** SNTP `time_is_set` callback misses DST transitions on every ESP-IDF version (DST is a localtime view, not a clock event).

### [P3] W48 — `_C` change-tracked status field consistency audit
**Status:** queued — documentation-only
**Source:** audit, v45 plan
**Acceptance:** inventory table appended; written conclusion (Conclusion A or B); knock-on classification of W40.
**Notes:** default expected outcome — Conclusion A: existing split is deliberate, audit looked at wrong file when raising W40, W48 closes with documentation comment.

---

## Active — log-audit findings (2026-05-06)



## Active — log-audit findings (2026-05-07)

### [P1] BOOT-OOM-MDNS — Boot-time heap exhaustion → `tiT` mDNS-OOM crash (CONFIRMED root cause)
**Status:** queued — needs-human-planning
**Source:** v37 preserved crash log + log-audit-20260507-001 + user-provided fresh crash trace 2026-05-07 (consolidates these into one finding — they are the same bug)
**Acceptance:** boot-time heap profile shows mdns_networking receive() allocations have headroom across the 9-second 194212→108B descent; reproduce-and-recover smoke covering >=5 OTA cycles with no second-boot crash; 24h post-OTA soak with no IllegalInstruction in tiT.
**Notes:** **Root cause confirmed from fresh crash 2026-05-06 23:53:12 CDT (uptime 89s, firmware v3.4.4-forceclose.61):**
- Reset reason: **IllegalInstruction** in task `tiT` (lwIP TCP/IP task)
- Last log line: `E (00:01:33.672) mdns_networking: Cannot allocate memory (receive(176), free heap: 220 bytes)`
- Stack trace: `0x4008EBBC 0x4008EB81 0x400955E9 0x401E5D9F 0x4010B3D3 0x4010B65D 0x4010B71E 0x40095331 0x401711E6 0x40144771 0x40147D47 0x4014CE52 0x4013BE72 0x4008FB41`
- Pattern: post-OTA boot completes init successfully, ~89s later mdns_networking receive() can't allocate 176 B (only 220 B free), tiT task hits illegal instruction (likely null-deref on the failed allocation return).

**This is the SAME bug as the previously-deferred V37 mDNS-OOM item** — the 194212→80672→108 B boot-heap descent leaves no margin for the post-OTA mDNS service announcement / receive burst. Recurrence: >=3 occurrences over 7 days (May 5 03:00:11→03:00:29 silent boot pair; May 6 23:51:43→23:53:12 confirmed crash with trace; plus historical V37 sample).

**Fix candidates** (planner picks one with rationale, validates heap delta):
- (a) Defer mDNS service registration until heap recovers above N KB threshold (e.g. 50 KB)
- (b) Cap mDNS service-record string length (TXT records can be 200-500 B per service)
- (c) Stagger HomeSpan + mDNS bring-up so both don't peak-alloc simultaneously
- (d) Pre-allocate lwIP/mDNS receive buffer pool at boot before anything else uses heap
- (e) Reduce `CONFIG_LWIP_TCP_RCVMBOX_SIZE` / mDNS-related sdkconfig knobs

**Pre-req:** log-audit-20260507-003 (`esp_reset_reason()` logging fix) lands FIRST so future occurrences self-attribute via syslog. Without that, we debug blind for the next sample.

Force-close FSM untouched. NOT auto-fixable — needs planner + heap budget review + on-device soak.

### [P2] log-audit-20260507-002 — `hkConsecutiveHealthyTicks` always reports 0 when auto-recover disabled
**Status:** queued — auto-fixable
**Source:** log-audit 2026-05-07 (Pi syslog)
**Acceptance:** counter increments on every healthy tick regardless of `hkAutoRecover` setting; diag-hk log line shows non-zero values during normal HomeKit activity.
**Notes:** `homekit.cpp:835` increments `hkConsecutiveHealthyTicks` only inside `else if (hkRecoverAttempts > 0)` branch. With `hkAutoRecover=false` (user's config; default), `hkRecoverAttempts` stays 0 forever, so counter never increments and diag-hk reporting is misleading — it falsely suggests HomeKit is unhealthy. Observed on 110 consecutive diag-hk lines over ~5h: every line shows `hkHealthyTicks=0` despite `controllers=4 paired=yes wifi=connected` and observed iOS reads (`last_hap_read_ago=44s` at times). Fix: hoist the increment+reset logic to run independently of recoverAttempts. Cosmetic/observability only — does not affect actual HomeKit recovery. Force-close FSM untouched. Single function in single file.

### [P2] log-audit-20260507-003 — `esp_reset_reason()` log fires pre-syslog, never reaches Pi
**Status:** queued — auto-fixable
**Source:** log-audit 2026-05-07 (Pi syslog)
**Acceptance:** `System restart reason: <code>` appears in Pi syslog after a non-power-on boot; silent post-OTA reboots become attributable.
**Notes:** `ratgdo.cpp:189-203` calls `esp_reset_reason()` and `ESP_LOGI("System restart reason: %d", r)` during very early init, **before** the syslog forwarder is bound. Confirmed: `grep "System restart"` across 7 days of Pi syslog returns ZERO hits despite 5 boots in the last 24h alone. Without this signal we cannot distinguish panic-reboots from sw-resets from brownouts. Fix options: (a) persist reset_reason to NVS in early boot, log it AFTER syslog is up; (b) buffer the log line and re-emit later. Option (a) is cleaner. Pre-req for log-audit-20260507-001 root cause. Force-close FSM untouched. Single boot-init function.

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

### [P3] R-?-fork — Verify `homeSpan.processSerialCommand` thread-safety, fix if unsafe
**Status:** queued — investigation gate
**Acceptance:** read HomeSpan repo + docs for re-entrancy contract on `processSerialCommand` while `autoPoll` is running. If documented thread-safe → close as non-finding. If unsafe → defer through a flag drained on the autoPoll task (same v24 reconnect-pattern).
**Notes:** caller is `helperFactoryReset` in `web.cpp`. Confidence on the bug is LOW — could turn out to be a non-issue.

---

## Upstream filing — DO NOT FILE

The fork's bug fixes (R1-R4 in `audit-notes/UPSTREAM_CHERRY_PICK_PLAN.md`) are already shipped in the fork. Their upstream-applicability is tracked in audit-notes/ for awareness only — **fork work proceeds regardless of upstream interest**.

**Hard rule:** No upstream PRs, no upstream issue filings, no cherry-picks to upstream. Issue #170 was filed historically; do not let it set precedent. Future agents reading this queue: if you find yourself drafting a `gh pr create --repo ratgdo/...`, STOP — that's not how this fork operates.

---

## Recently completed

_(roll commits in here as W4x/Rx items land — keep last 10)_
