# homekit-ratgdo32 work queue

Priority-ordered. Top = next. Detailed analysis lives in `audit-notes/` (gitignored fork-internal).

## Active — fork-internal feature work

### [P2] HK-FC — Native HomeKit Force-Close GarageDoorOpener accessory
**Status:** queued — full plan embedded in issue body
**Source:** user request 2026-05-06; planner produced full layered plan
**Issue:** Haglerd/homekit-ratgdo32#79 (embedded plan — software-engineer goes direct, no planner re-invocation needed)
**Acceptance:** new optional `forceCloseHomeKit` toggle (default OFF) registers a second `GarageDoorOpener` accessory; Open mirrors normal open; Close fires `door_command_force_close(<configured hold ms>)`; state mirrors primary in lockstep across Current/Target/Obstruction characteristics. Web UI checkbox + `forceCloseHoldMs` number input (clamped 1000-10000). No regression on existing primary tile. Heap budget verified: ~28 B BSS when OFF, ~1 KB heap when ON (within user-stated thresholds: ≤500 B disabled, ≤1.5 KB enabled).
**Notes:** Enables eventual deprecation of the `homebridge-ratgdo-forceclose` npm plugin (sunset path documented in issue body, ~6-month horizon). ESP32-only (HomeSpan path); ESP8266 short-circuited via `#ifdef`. Sec+1.0 only — Sec+2.0 falls through to normal close per existing `comms.cpp:2842` gate (tracked separately under `Sec2-FC`). Fallback path (Option A: modify primary tile's Close to call force-close, zero heap delta but every-close relay-wear cost) baked into issue body if on-hardware heap measurement exceeds 1.2 KB. ~250-350 LoC across 8 files; medium complexity.

---

## Active — fork-internal (W41-W48 audit cleanup)

### [P3] W41 — Move `extern volatile uint32_t` declarations to header
**Status:** queued — **DIRECTION: option (b) — all 7 declarations including `syslogDrops`** (currently same-TU-only in log.cpp; audit recommends preemptive add)
**Source:** audit, v45 plan
**Acceptance:** new `src/instrumentation.h` (flat src/ layout, no src/include/) holds all 7 declarations (`logMtxMaxWaitMs`, `sseSlowWrites`, `sseBufferFullSkips`, `sseSlotsAlloc`, `sseOrphansReaped`, `statusJsonPeakLen`, `syslogDrops`); `git grep "extern volatile uint32_t"` returns zero hits in `src/*.cpp`.
**Notes:** zero behavior change. Zero RAM impact — `extern` declarations are name-binding only; definitions stay in their current `.cpp` files.

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

### [P2] ~~log-audit-20260506-003~~ — SSE wedged-on-flow-control reaper churn, same UUIDs reaped 28+ times
**Status:** DONE — PR https://github.com/Haglerd/homekit-ratgdo32/pull/78 (branch `log-audit-003-sse-wedge-dampener`)
**Source:** log-audit 2026-05-06 (Pi syslog)
**Issue:** Haglerd/homekit-ratgdo32#71 (closed via PR)
**Acceptance:** Per-UUID 60s 429 dampener landed in `handle_subscribe`. `recentReaps[8]` table (~352 BSS, zero heap) populated by wedged-flow-control reap path. Wedged-reap log line extended with `wedgedFor=Xms`. Code-review fixes folded in: `id=-1` sentinel + String temp lifetime guard.
**Notes:** Soak (24h Pi syslog watching for <5 wedged-reaps per UUID) deferred to next user device-flash window.

---

## Deferred — need measurement / soak data first

### [P3] Boot-time heap exhaustion → `tiT` mDNS-OOM crash
**Status:** deferred (architectural)
**Source:** v37 follow-up — preserved crash log
**Acceptance:** boot-time heap allocator profile produced; one fix candidate validated (stagger mDNS-vs-HomeSpan, cap mDNS service-record size, defer SSE pool, etc.).
**Notes:** different code path from V4's logger-OOM fix. Heap goes 194212 → 80672 → 108 bytes in 9 seconds at boot. NOT in v45 scope.

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

### [P2] Sec2-FC — Force-close override is Sec+1.0-only; verify Sec+2.0 protocol path
**Status:** queued — investigation gate
**Source:** user observation 2026-05-06 — physical hold-button on Sec+ 2.0 wall panel closes obstructed door, but firmware's `door_command_force_close()` at `comms.cpp:2842` early-returns to a normal close on `doorControlType != 1`. User has a second device running Sec+ 2.0; current behavior on that device is "force-close button = normal close, photo eye still blocks."
**Acceptance:** confirm or refute the inherited comment at `comms.cpp:2660` ("Sec+2.0 has no equivalent protocol message"). EITHER document why Sec+2.0 truly can't do the override and update the warning log to say so explicitly, OR implement the Sec+ 2.0 hold-button packet path. State machine (forceCloseInProgress / forceCloseAttempt / gap timer / preempt_force_close) is reused verbatim; only the per-press packet emission differs. No infrastructure rework.
**Notes:** comment likely inherited from upstream and never validated for the fork. Sec+ 2.0 button-press packets carry button-state (press/release/held); wired wall panels emit the held variant when physically held, and the GDO honors it for the photo-eye override. If true, the firmware can emit the same packet sequence. Investigation: read `secplus2.h` packet definitions + check what a wired Sec+2.0 panel actually transmits during a hold (Pi syslog from a Sec+ 2.0 unit's RX-monitor mode would settle it).

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
