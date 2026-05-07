# homekit-ratgdo32 work queue

Priority-ordered. Top = next. Detailed analysis lives in `audit-notes/` (gitignored fork-internal).

## Active — fork-internal (W41-W48 audit cleanup)

Per the v45 cleanup plan in `audit-notes/2026-05-04-fork-vs-upstream-attribution.md`. Recommended sequencing: tooling sweeps first (W45/W46/W47) so any new findings they surface fold into the same release.

### [P2] ~~W45~~ — Build with `-Wshadow=local -Werror=shadow`, triage shadow warnings
**Status:** DONE (branch `w45-wshadow-triage`)
**Source:** audit, v45 plan
**Acceptance:** met — `pio run -e ratgdo_esp32dev` exits 0; `-Wshadow=local` permanent in `build_src_flags` (src-only scope); `-Werror=shadow` reverted. Two fork shadows fixed (`DEV_Light::DEV_Light`, `DEV_Motion::DEV_Motion` ctor params); four library include sites pragma-suppressed (`Arduino.h`, `HomeSpan.h`, `SoftwareSerial.h`, `HTTPClient.h`).
**Notes:** Triage results in audit-notes "v45 W45 triage results" subsection; full status entry in Whats-Done file.

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
