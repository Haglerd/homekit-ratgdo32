# homekit-ratgdo32 work queue

Priority-ordered. Top = next. Detailed analysis lives in `audit-notes/` (gitignored fork-internal). Done items are removed — git log is the durable record.

## Active

### [P1] log-audit-20260513-007 — force-close release callback runs after `clear_force_close_state` → counter=0 misattribution
**Status:** shipped in .80 (commit a37a0b3 + 6046884) and carried into .81/.82/.83; **soak validation still pending** — race window not yet exercised (5 sequences observed cumulatively, all non-race-path)
**Source:** log-audit 2026-05-13 (18h, 1440 lines); re-checked 2026-05-14 (3.5h, 281 lines); re-checked 2026-05-15 (24h, 1888 lines) on .82
**Issue:** (not filed)
**Acceptance:** force-close release callback short-circuits when `forceCloseInProgress` was cleared mid-hold; no `release sent (2-attempt mechanic, attempt 0/2)` log; the new `FORCE CLOSE: release sent, sequence already cleared (door began Closing during hold)` log line appears when the race fires; 7+ force-close sequences over 24h soak with no door-reverses-after-close-command behaviour.
**Notes:** P1 user-visible failure. Fix verified in source at comms.cpp:2813-2850 (entry-check on forceCloseInProgress + ESP_LOGI "sequence already cleared"). Device on .82 (status.json firmwareVersion="3.4.4-forceclose.82", uptime ~27.5h at 2026-05-15 audit end, crashCount=0). **5 HK-initiated FC sequences observed cumulatively across .80+.81+.82, ALL took the non-race path** (Closing-during-hold/TTCtimer.detach). New on this audit: FC#3 (2026-05-14 16:25:30, Closing concurrent with press), FC#4 (2026-05-14 18:48:08, Closing 1.37s into 2500ms hold), FC#5 (2026-05-14 20:00:12, Closing 1.40s into hold). The `sequence already cleared` race log line has **NEVER appeared** — the user's GDO consistently enters Closing within 1.0-1.5s, well inside the 2500ms hold, so the post-clear release-callback race window has not been naturally exercised. Code review confirms fix is correct; soak metric ("7+ sequences with no reverse") trivially met by the non-race path but the targeted race log condition (the actual acceptance criteria) remains untriggered. Auto-fix-eligibility: **shipped, awaiting telemetry** — consider re-classifying acceptance to "verified via static review" since natural race-trigger frequency may be measured in months. NOTE: finding 009's reentry symptom now covered separately by the .83 FC cooldown gate.

---

## Recently completed (.83 bundle, 2026-05-16, PR #130 / commit 9dd52ea)

- **log-audit-20260515-010** (P1) — heap visibility: new ESP32-only `GET /heap` endpoint + adaptive HomeKit-health sampler cadence (180s → 30s when free heap < 20KB, holds 5min after recovery). 72h soak acceptance pending in .83. **RECURRENCE 2026-05-16 audit (still on .82, .83 NOT yet flashed)**: NEW deepest-dip-ever observed — heap=4000 maxBlock=2164 at 2026-05-15T19:58:26 CDT (uptime 167403s, ~46.5h). 4000B is 396B above the 4096B heap-floor short-circuit (yesterday's record was 4396, only 300B above). maxBlock=2164 means 54% of remaining heap fragmented into sub-2KB chunks — worst fragmentation snapshot ever recorded. Recovered to 42204 by next 180s sample (so dip lasted <3min). Single user activity in window: HK door open/close ~6 minutes prior (18:21:33 / 18:31:05 CDT) — heap had recovered to 42180 baseline by 19:55:26 then crashed 24KB at 19:58:26 with iOS quiet (last_hap_read_ago=899s, hintLevel=1). RSSI was -43dBm (worst observation in window, baseline -38dBm) suggesting lwIP rx-path or WiFi-stack consumption under marginal signal. SECOND DIP same window: heap=19320 maxBlock=21492 at 23:13:26 CDT, also 3-5min after door operations (Closing 23:03:06, Closing 23:07:50). Deploy of .83 with adaptive 30s cadence and `/heap` endpoint is the right next step — would have captured ~6 fast-samples bracketing the 4000-dip recovery curve.
- **log-audit-20260515-011** (P2) — SEC1 retry log enrichment: both first-of-burst and `[+N suppressed]` lines now include `[cmd=0xNN retries=N bus_silent=Nms cts=N rx_pending=N]` for root-cause attribution. 48h structured-log acceptance pending in .83. **RECURRENCE 2026-05-16 audit (still on .82)**: 2 more SEC1 TX retry events at 2026-05-15T23:02:52 and 23:07:37 CDT, both 1.0-1.5s before successful Open→Closing transitions (cmd transition timing identical to prior pattern). Pre-.83 log line still missing diagnostic suffix — confirms enrichment is needed; not a new pattern, just continued observation. Door closed cleanly each time (12047/12049ms). Cumulative SEC1-failure-then-success count now ~10 across .80/.81/.82 with zero functional impact.
- **log-audit-20260515-009** (P1) — HK Close-burst dedup + force-close reentry cooldown. New `hk_target_is_redundant()` helper at HK callback layer + `forceCloseClearedAtMs` stamp set in `clear_force_close_state`, gate in `door_command_force_close` (3s window). Strictly additive to existing FC safety gates (.80 release-callback fix, 2-attempt mechanic, CURR_CLOSED/CLOSING/0xFF entry gate, TAS). Code-review verified all 10 FC safety properties PASS.
- **log-audit-20260514-008** (P1) — HK Open-burst dedup: same `hk_target_is_redundant()` helper handles Open path. Mid-cycle reversals preserved.
- **tickDriftMs cast fix** (code-review out-of-scope finding from PR #130 review) — `src/homekit.cpp:688` time math fixed (was producing garbage int32-overflow values at ~25-day uptime).

## Deferred — soak-pending only

### [P3] HEAP-PRESSURE-WATCH — `mdns_networking: Cannot allocate memory` events under sustained operation
**Status:** deferred (`.79` heap-floor short-circuit makes the symptom non-lethal; OOM bursts no longer observed during overnight soak). Re-evaluate after .83 `/heap` + adaptive-cadence data identifies the transient allocator.
**Source:** 2026-05-09 .77 panic-loop investigation. Crash dump showed `free heap: 136 bytes` when mdns_mem_calloc failed. iOS hub state-sync + HomeSpan HAP + mDNS query bursts can momentarily consume nearly all heap. With `.79`'s fix (mdns task gated + 4KB heap floor) any such event is silently dropped at the syslog hook.
**Acceptance:** identify what's consuming heap during these bursts (heap-trace, periodic snapshots) and either bump tcpip MEMP pools / mdns rx-buf / cap HAP transaction concurrency, OR conclude the existing 30-50 KB headroom is enough now that the panic chain is fixed. Overnight `.79` soak shows heap stable at 38-42 KB — no OOM events in 10h+ — leaning toward "no further action needed" unless symptoms reappear.
**Notes:** panic chain is the priority — fixed at the syslog re-entry point so 100% heap exhaustion in mDNS no longer crashes. Investigate only if device experiences other symptoms (slow HAP, dropped notifications) under sustained low-heap conditions. Not user-visible right now.

---

## Upstream filing — DO NOT FILE

The fork's bug fixes (R1-R4 in `audit-notes/UPSTREAM_CHERRY_PICK_PLAN.md`) are already shipped in the fork. Their upstream-applicability is tracked in audit-notes/ for awareness only — **fork work proceeds regardless of upstream interest**.

**Hard rule:** No upstream PRs, no upstream issue filings, no cherry-picks to upstream. Issue #170 was filed historically; do not let it set precedent. Future agents reading this queue: if you find yourself drafting a `gh pr create --repo ratgdo/...`, STOP — that's not how this fork operates.
