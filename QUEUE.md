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

### [P2] log-audit-20260507-002 — `hkConsecutiveHealthyTicks` always reports 0 when auto-recover disabled
**Status:** queued — auto-fixable
**Source:** log-audit 2026-05-07 (Pi syslog)
**Acceptance:** counter increments on every healthy tick regardless of `hkAutoRecover` setting; diag-hk log line shows non-zero values during normal HomeKit activity.
**Notes:** `homekit.cpp:835` increments `hkConsecutiveHealthyTicks` only inside `else if (hkRecoverAttempts > 0)` branch. With `hkAutoRecover=false` (user's config; default), `hkRecoverAttempts` stays 0 forever, so counter never increments and diag-hk reporting is misleading — it falsely suggests HomeKit is unhealthy. Observed on 110 consecutive diag-hk lines over ~5h: every line shows `hkHealthyTicks=0` despite `controllers=4 paired=yes wifi=connected` and observed iOS reads (`last_hap_read_ago=44s` at times). Fix: hoist the increment+reset logic to run independently of recoverAttempts. Cosmetic/observability only — does not affect actual HomeKit recovery. Force-close FSM untouched. Single function in single file.

### [P2] log-audit-20260507-004 — `errno 11 "No more processes"` recurrence beyond browser fan-out (post log-audit-002 fix)
**Status:** in-progress — investigation underway on `queue/log-audit-004-errno11-recurrence`
**Source:** log-audit 2026-05-07 (Pi syslog) — user-surfaced, multi-boot pattern
**Acceptance:** root cause identified for the non-browser-fanout occurrences; either second client-side mitigation OR firmware-side socket-pool / cleanup fix lands; 24 h soak with <2 errno 11 events per 6 h window.
**Notes:** log-audit-002 (PR #77, sequentialize browser fetches) handled the diagnostics-page concurrent-fetch pattern. errno 11 still recurring on .64+ across multiple boots:
- 2026-05-07 02:17:41 fd 52 — uptime 02:24h, no co-incident SSE / browser activity in surrounding lines
- 2026-05-07 05:20:12 fd 51 — uptime 00:01m, 22 s after `Initialization complete` (post-boot mDNS / HomeSpan race?)
- 2026-05-07 05:31:18 fd 52 — 35 s after force-close test (`FORCE CLOSE attempt 1` → close at :30:43 → errno at :31:18, no obvious bridge)
- 2026-05-07 05:56:48 / 05:58:39 / 06:06:12 / 06:16:09 / 06:17:18 fd 51 — five events on a single boot
- **05:58:39 is co-incident with SSE-wedge-reaper firing**: `05:58:34 SSE wedged-on-flow-control reap (UUID 73e0640b...)` → `05:58:36 SSE 429 dampener (ageMs=1936)` → `05:58:39 errno 11 fd 51` (3 s after dampener). Strong correlation suggests the reap-then-dampener path is itself opening a transient socket that hits the cap.

Hypothesis (planner picks): (a) reap-path leaks a socket fd before close; (b) HomeKit pairing burst at boot consumes lwIP sockets that don't return to the pool in time; (c) lwIP MEMP_NUM_TCP_PCB / MEMP_NUM_NETCONN tuned too low for HomeSpan + SSE + mDNS + browser concurrency; (d) Cloudflare-keepalive-style long-lived idle connections occupy slots.

Force-close FSM untouched. Pre-req for serious autonomous fix: log-audit-20260507-003 (`esp_reset_reason` syslog) is shipped (#88) so future tiT crashes correlated with this surface attribution. NOT auto-fixable — needs-investigation gate first to identify which hypothesis matches.

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

- **BOOT-OOM-MDNS** — defer ratgdo mDNS service registration until heap >= 50 KB (option a from QUEUE candidates). ESP32-only; ESP8266 path unchanged. PR https://github.com/Haglerd/homekit-ratgdo32/pull/89 (merged 2026-05-07). Validates via post-flash syslog: expect `ratgdo mDNS deferred:` then `floor cleared` within 30 s; absence of `mdns_networking: Cannot allocate memory` and tiT IllegalInstruction. >=5 OTA cycle smoke + 24 h soak still pending on-device.
- **log-audit-20260507-003** — `esp_reset_reason()` re-emit after syslog bound — PR https://github.com/Haglerd/homekit-ratgdo32/pull/88 (merged 2026-05-07). Pre-req for BOOT-OOM-MDNS unblocked.
