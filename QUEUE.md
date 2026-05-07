# homekit-ratgdo32 work queue

Priority-ordered. Top = next. Detailed analysis lives in `audit-notes/` (gitignored fork-internal).

## Active — fork-internal (W41-W48 audit cleanup)

Per the v45 cleanup plan in `audit-notes/2026-05-04-fork-vs-upstream-attribution.md`. Recommended sequencing: tooling sweeps first (W45/W46/W47) so any new findings they surface fold into the same release.

### [P2] ~~W45~~ — Build with `-Wshadow=local -Werror=shadow`, triage shadow warnings
**Status:** DONE — PR https://github.com/Haglerd/homekit-ratgdo32/pull/72 (branch `w45-wshadow-triage`)
**Source:** audit, v45 plan
**Acceptance:** clean `pio run -e ratgdo_esp32dev` with `-Wshadow=local` permanently in build_flags. Triage results appended to audit doc.
**Notes:** library-header shadows (arduino-esp32, HomeSpan) are accepted noise per user direction; only fix shadows in fork code. `-Werror=shadow` reverts after triage.

### [P2] W46 — Add eslint over `src/www/` + GitHub Actions CI lint
**Status:** queued
**Source:** audit, v45 plan
**Acceptance:** `npx eslint src/www/` exits 0; `.github/workflows/lint.yml` runs on push + PR. Vendored `marked.umd.js` + `qrcode.js` excluded.
**Notes:** include CI integration in same commit per user direction (don't defer).

### [P2] W47 — `Ticker.detach()` audit sweep with provenance comments
**Status:** queued — fourth same-shape leak candidate identified at `comms.cpp:3318`
**Source:** audit, v45 plan
**Acceptance:** every `.detach()` line carries a one-line provenance comment; `:3318` site fixed inline with `request_force_close_clear` (per user direction — TTC arm-fresh preempts in-flight force-close).
**Notes:** 24 production sites + 4 comment-only references inventoried in plan.

### [P3] W41 — Move `extern volatile uint32_t` declarations to header
**Status:** queued
**Source:** audit, v45 plan
**Acceptance:** new `src/instrumentation.h` (flat src/ layout, no src/include/) holds 7 declarations; `git grep "extern volatile uint32_t"` returns zero hits in `src/*.cpp`.
**Notes:** zero behavior change.

### [P3] W43 — `writeBuffer` rename + invariant comment
**Status:** queued
**Source:** audit, v45 plan
**Acceptance:** rename to `loopTaskScratchBuf512` (or similar); add comment block documenting loopTask-only invariant; ESP8266 alias preserved.
**Notes:** zero behavior change. Option-A (per-caller stack buffers) rejected for ESP8266 stack pressure.

### [P2] W42 — Add mutex to `userSettings::get()` + `getDetail()`
**Status:** queued
**Source:** audit, v45 plan
**Acceptance:** mutex-wrapped reads; cache pattern preserved as fast path; smoke-tested via config toggle + homekit_health_log read.
**Notes:** ESP8266 `TAKE_MUTEX/GIVE_MUTEX` are no-ops (cooperative scheduling) — ESP32 only sees runtime cost. Race exposure raised by fork's Ticker-context readers.

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

### [P2] log-audit-20260506-001 — SSE subscriptionCount=8 on every boot, reconciler hides root cause
**Status:** queued
**Source:** log-audit 2026-05-06 (Pi syslog, 7979 lines, 42h window)
**Issue:** Haglerd/homekit-ratgdo32#69
**Acceptance:** root cause identified; post-fix soak shows zero `counter=8 actual=0` warnings within 10s of `Initialization complete` across 5 consecutive reboots.
**Notes:** P2 — 24/24 reproducibility over 24 different boots. Functional impact nil (reconciler clamps), but counter=`SSE_MAX_CHANNELS` exact match suggests a deterministic init-path bump or BSS issue. **needs-human-planning** (investigation-shaped, root cause unknown).

### [P2] log-audit-20260506-002 — Socket fd exhaustion (errno 11) under browser concurrent-fetch burst
**Status:** queued
**Source:** log-audit 2026-05-06 (Pi syslog)
**Issue:** Haglerd/homekit-ratgdo32#70
**Acceptance:** zero `errno 11 No more processes` over 5 diagnostics-page loads; 3 reboots with browser open and no GDO init timeout.
**Notes:** P2 — 14× errno 11 events + 1× `Not enough memory to allocate buffer` co-incident with a `Garage door is not responding to initialization sequence (3000ms)` at 2026-05-06T16:38:05. Browser at 10.112.60.248 fans out 4-5 concurrent fetches (`/showlog`, `/showrebootlog`, `/crashlog`, `/site.webmanifest`, SSE) and exhausts the lwIP socket pool. **needs-human-planning** — three viable mitigations (raise CONFIG_LWIP_MAX_SOCKETS, sequentialize browser fetches, or 503-with-retry-after); user picks direction.

### [P2] log-audit-20260506-003 — SSE wedged-on-flow-control reaper churn, same UUIDs reaped 28+ times
**Status:** queued
**Source:** log-audit 2026-05-06 (Pi syslog)
**Issue:** Haglerd/homekit-ratgdo32#71
**Acceptance:** PR landing per-UUID rapid-recurrence dampener (60s 429 lockout) + extended log line; 24h soak shows <5 wedged-reaps per UUID (down from 28).
**Notes:** P2 — 80 `wedged on flow-control` reaps across 4 UUIDs from same client IP (10.112.60.248), same UUID re-subscribing after reap and re-wedging. Reaper functioning but client is misbehaving (likely iOS Safari background-tab SSE silencing). **auto-fixable** — ~30 LoC contained to `web.cpp` SSE block.

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
