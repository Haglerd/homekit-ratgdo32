# homekit-ratgdo32 work queue

Priority-ordered. Top = next. Detailed analysis lives in `audit-notes/` (gitignored fork-internal). Done items are removed — git log is the durable record.

## Active

### [P1] log-audit-20260513-007 — force-close release callback runs after `clear_force_close_state` → counter=0 misattribution
**Status:** shipped in .80 (commit a37a0b3 + 6046884) and carried into .81/.82; **soak validation still pending** — race window not yet exercised
**Source:** log-audit 2026-05-13 (Pi syslog window 2026-05-13T06:03Z -> 2026-05-14T00:22Z, 18h, 1440 lines); re-checked 2026-05-14 02:30Z -> 06:01Z (3.5h, 281 lines) on .82
**Issue:** (not filed)
**Acceptance:** force-close release callback short-circuits when `forceCloseInProgress` was cleared mid-hold; no `release sent (2-attempt mechanic, attempt 0/2)` log; the new `FORCE CLOSE: release sent, sequence already cleared (door began Closing during hold)` log line appears when the race fires; 7+ force-close sequences over 24h soak with no door-reverses-after-close-command behaviour.
**Notes:** P1 user-visible failure. Fix verified in source at comms.cpp:2813-2850 (entry-check on forceCloseInProgress + ESP_LOGI "sequence already cleared"). Device now on .82 (status.json firmwareVersion="3.4.4-forceclose.82", uptime 12783s at audit-end). **Second HK-initiated FC observed on .82 at 2026-05-13T21:39:08 CDT** (10min51s into .82 uptime): HK target=Closed -> HK-FC mode=2 dispatched -> press fired attempt 1/2 (hold=2500ms) -> Door state Open->Closing at 21:39:10 (**1.17s into the 2500ms hold**) -> TTC delay timer canceled -> HK target reversed to Open at 21:39:11 -> Closed again at 21:39:16 (refused, "door already Closing") -> door fully Closed 21:39:23. **The .80 race-fix log line "sequence already cleared" did NOT appear** — this FC took the same Closing-during-hold/TTCtimer.detach path as the .80 FC, NOT the post-clear release-callback race window the .80 fix gates. Two HK FC sequences observed cumulatively (.80 20:51:57 + .82 21:39:08), both followed the same non-race path. The race fix is in flight but unverified — need a sequence where door does NOT reach Closing before the 2500ms hold completes. Auto-fix-eligibility: **shipped, awaiting telemetry** (longer soak with TTC-during-hold variance needed; consider a deliberate-test scenario where door starts farther from sensor).

### [P1] log-audit-20260514-008 — iOS HomeKit Open-command burst floods TX queue → release packets dropped, door-supposed-to-be-opening false-error
**Status:** queued (new finding, observed on .79 pre-OTA at 2026-05-13T19:50:03 CDT)
**Source:** log-audit 2026-05-14 (Pi syslog window 2026-05-14T00:22Z -> 2026-05-14T01:49Z, ~85min, 323 lines)
**Issue:** (not filed)
**Acceptance:** iOS HK Open-command bursts (12+ "Garage Door Characteristics Update, door target: Open" in <1s) must NOT cause TX queue depth >8 sustained or any "packet queue full, dropping door command release pkt" errors. Either dedup redundant identical-target HK updates (similar to the FC overlap-rejection rate-limit in commit ceb754f), or bump TX queue depth, or both. Acceptance: 24h with no `packet queue full, dropping` events and no `Door is supposed to be opening but is not` errors that follow a flood.
**Notes:** P1 user-visible degraded behavior — door eventually opened but with internal target/actual disagreement. Single ~10s burst pattern at 19:50:03 CDT:
- iOS issued ~12 redundant "door target: Open" updates in 800ms (likely Home app retry storm after a slow response)
- TX queue saturated to depth 14-16 (warning threshold 8)
- 65x `WARNING: message packets in TX queue is > 8 (N)` warnings, peak N=16
- 2x `E ratgdo-comms: packet queue full, dropping door command release pkt` (the release toggle after press — losing this leaves GDO in pressed state momentarily)
- Followed 2s later by `E ratgdo-comms: Door is supposed to be opening but is not. Current state: Opening` (target/actual mismatch from dropped packets)
- Door reached Open state normally at 19:50:12, so end-to-end correct outcome — but the internal state was corrupted briefly and a real GDO hardware fault would now be indistinguishable from a recoverable flood.
Cross-reference: commit `ceb754f` rate-limited the *log* for FC redundant-close burst, but did NOT throttle the underlying packet emission for non-FC HK Open commands. The garage_door_open path in homekit.cpp emits a fresh press packet (and scheduled release packet) for every HK target-state update even when target is already Open and the door is mid-transition. Fix scope: probably homekit_callback for door target-state, drop the call when (current==target || already-in-progress-toward-target). Auto-fix-eligibility: **needs-human-planning** (touches HK callback + comms TX path).

---

## Deferred — soak-pending only

### [P3] HEAP-PRESSURE-WATCH — `mdns_networking: Cannot allocate memory` events under sustained operation
**Status:** deferred (`.79` heap-floor short-circuit makes the symptom non-lethal; OOM bursts no longer observed during overnight soak)
**Source:** 2026-05-09 .77 panic-loop investigation. Crash dump showed `free heap: 136 bytes` when mdns_mem_calloc failed. iOS hub state-sync + HomeSpan HAP + mDNS query bursts can momentarily consume nearly all heap. With `.79`'s fix (mdns task gated + 4KB heap floor) any such event is silently dropped at the syslog hook.
**Acceptance:** identify what's consuming heap during these bursts (heap-trace, periodic snapshots) and either bump tcpip MEMP pools / mdns rx-buf / cap HAP transaction concurrency, OR conclude the existing 30-50 KB headroom is enough now that the panic chain is fixed. Overnight `.79` soak shows heap stable at 38-42 KB — no OOM events in 10h+ — leaning toward "no further action needed" unless symptoms reappear.
**Notes:** panic chain is the priority — fixed at the syslog re-entry point so 100% heap exhaustion in mDNS no longer crashes. Investigate only if device experiences other symptoms (slow HAP, dropped notifications) under sustained low-heap conditions. Not user-visible right now.

---

## Upstream filing — DO NOT FILE

The fork's bug fixes (R1-R4 in `audit-notes/UPSTREAM_CHERRY_PICK_PLAN.md`) are already shipped in the fork. Their upstream-applicability is tracked in audit-notes/ for awareness only — **fork work proceeds regardless of upstream interest**.

**Hard rule:** No upstream PRs, no upstream issue filings, no cherry-picks to upstream. Issue #170 was filed historically; do not let it set precedent. Future agents reading this queue: if you find yourself drafting a `gh pr create --repo ratgdo/...`, STOP — that's not how this fork operates.
