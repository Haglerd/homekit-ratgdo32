# homekit-ratgdo32 work queue

Priority-ordered. Top = next. Detailed analysis lives in `audit-notes/` (gitignored fork-internal). Done items are removed — git log is the durable record.

## Active

### [P1] log-audit-20260515-010 — heap dip to **4396** (maxBlock=12788) — deepest dip ever observed, fragmentation signal, .79 4KB heap-floor is sole barrier to OOM-chain
**Status:** in-progress (planner routing 2026-05-15)
**Source:** log-audit 2026-05-15 (Pi syslog window 2026-05-15T06:01Z -> 2026-05-15T21:05Z, 15h, 1112 lines)
**Issue:** (not filed)
**Acceptance:** 72h soak with no `HomeKit health: heap=` sample below 10000 AND no sub-25000 sample with `maxBlock < heap/2` (fragmentation indicator). Either identify and bound the transient allocator (mDNS rx-burst / HAP request batch / SSE write buffer / TCP retransmit queue) responsible, or land a periodic heap-defragmentation/compaction hint. Acceptance also requires a heap-sample-on-demand endpoint so the 3-min health-sampler isn't the only visibility window — the actual dip may go deeper than 4396 between samples.
**Notes:** P1 latent — no user-visible failure (.79 heap-floor short-circuit at 4096B prevents OOM panic chain that bit `.77` per mdns_networking crash). But the safety margin collapsed 73% vs prior worst:
- **Deepest dip this audit: heap=4396 maxBlock=12788 rssi=-41dBm** at 03:52:26 CDT (single 3-min health sample, recovered to 42172 by next sample — burst lasted <3 min)
- **Prior worst dip (2026-05-14 audit, .82): heap=15332** — this is **3.5x deeper** at 4396, only 300B above the 4096B heap-floor
- maxBlock=12788 vs typical 36852 = **65% fragmentation** of remaining heap; this is a different failure mode than the prior dips (which were pressure, not fragmentation)
- 2 additional sub-25K dips same window: 14516@11:22 (maxBlock=11764 — also fragmented), 24560@11:55 (maxBlock=36852 — pressure only). Five total samples below 35K across 302 samples (~1.7% — up from 1.4% in prior audit).
- The 03:52 dip coincided with RSSI degradation (-34dBm baseline -> -41dBm at the dip sample) suggesting a WiFi RX-burst / lwIP buffer expansion may be the proximate consumer. iOS was extended-idle (last_hap_read_ago=2553s, threshold-level=3 watchdog hint firing concurrently) — so this is NOT an iOS request flood.
- Door operations during window: 7 open/close cycles, all within 12047-12061ms tolerance (no degraded performance correlated with heap dips).
Representative lines:
```
2026-05-15T03:49:26 heap=42144 maxBlock=36852 rssi=-40dBm  (3 min before)
2026-05-15T03:52:26 heap=4396  maxBlock=12788 rssi=-41dBm  hintLevel=3  (the dip)
2026-05-15T03:55:26 heap=42172 maxBlock=36852 rssi=-40dBm  (3 min after — full recovery)
```
Cross-reference: existing **[P3] HEAP-PRESSURE-WATCH** (deferred) was justified by "30-50 KB headroom is enough now that the panic chain is fixed; overnight soak shows 38-42 KB" — that assumption no longer holds when 4396 with fragmentation appears in normal idle. Recommend escalating P3 -> P1 attention. Auto-fix-eligibility: **needs-human-planning** (intersects heap policy, lwIP rx buffer sizing, mDNS task gating, RSSI-correlated allocations).

### [P2] log-audit-20260515-011 — `SEC1 TX send failed, exceeded max retry` recurring at close-command time (3 events in 15h, all immediately preceding Open->Closing transitions, door closes correctly each time)
**Status:** queued (new finding, observed on .82)
**Source:** log-audit 2026-05-15 (Pi syslog window 2026-05-15T06:01Z -> 2026-05-15T21:05Z, 15h, 1112 lines); .1 log shows 5 more in prior 24h (2026-05-14)
**Issue:** (not filed)
**Acceptance:** Either capture the SEC1 retry attempt count + reason (collision / no-ack / parity?) in the log line so root cause is identifiable, OR confirm via correlation that this is pure GDO-bus contention with a remote (wired) keypad / wall console and add suppression to drop the `E` to `W`. Acceptance: 48h with either a structured retry log OR an explanation in audit-notes that the event is benign-and-expected, with no user-visible delayed-close.
**Notes:** P2 observability/diagnostic. End-to-end outcome correct in all 3 events:
- 2026-05-15T06:22:01 — SEC1 fail, 1.5s later Door state Open->Closing, close took 12048ms (median)
- 2026-05-15T08:13:12 — SEC1 fail, 1.3s later Door state Open->Closing, close took 12048ms (median)
- 2026-05-15T15:29:41 — SEC1 fail, 1.5s later Door state Open->Closing, close took 12047ms (median)
All 3 fired immediately before a HK-initiated close that completed normally. The May-14 log already includes a suppression hint `[+2 suppressed in last 34653722ms — obstructed door / busy bus]` indicating firmware has bus-busy rate-limit logic — but the underlying cause (lost ack? collision? local door obstruction sensor wiggle?) is not in the log. Door open/close durations are extremely stable (12048ms median across 5+ samples each side), so this is unlikely to be a real obstruction. Most plausible: SEC1 protocol retry collision with internal GDO traffic (motion/light status broadcasts from the head unit) — the first press packet gets clobbered, the retry succeeds 1-1.5s later, door responds normally on the retry. Auto-fix-eligibility: **auto-fixable** (instrument the log line with retry-attempt-count + last-rx-byte timestamp; severity downgrade to W if benign).

### [P1] log-audit-20260515-009 — HK redundant-target dispatch burst on Closed path → second `FORCE CLOSE: starting 2-attempt sequence` re-fires after first sequence clears mid-burst
**Status:** queued (new finding, observed on .82 at 2026-05-14T16:25:28 CDT)
**Source:** log-audit 2026-05-15 (Pi syslog window 2026-05-14T06:01Z -> 2026-05-15T06:01Z, 24h, 1888 lines)
**Issue:** (not filed)
**Acceptance:** 24h with no HK characteristic burst exceeding 3 dispatches/sec for the same target value, AND no double-`FORCE CLOSE: starting 2-attempt sequence` event for a single user-initiated close request. Fix must either dedup redundant target==current HK callbacks (same scope as finding 008) AND add a cooldown gate on `force_close_door()` entry (~3s after `forceCloseInProgress` clears) to prevent the second-sequence reentry observed here.
**Notes:** P1 — close-path mirror of finding 008's Open-path burst, plus a NEW symptom (second force-close sequence reentry). Single burst pattern at 16:25:28.626 -> 16:25:32.094 CDT on `.82`:
- iOS sent **15 redundant "door target: Closed" updates in 2.1s** (same dispatch-storm pattern as 008 but on close)
- 15 corresponding `HK-FC mode=2 — primary close dispatching force-close` entries fired
- Only first dispatch actually fired the press (FORCE CLOSE: press fired attempt 1/2 at 16:25:28.671)
- 14 subsequent dispatches rejected by comms layer: 1x `ignoring request — sequence already in progress`, 6x `refusing — door is already Closing`, and CRITICALLY:
- **A SECOND `FORCE CLOSE: starting 2-attempt sequence` + `press fired, attempt 1/2 holding for 2500ms` fired at 16:25:30.011** — exactly when door state transitioned Open->Closing and `forceCloseInProgress` was cleared by TTC timer cancel. Second press hit the GDO bus during an actively-closing door (correctly caught by `door is already Closing` no-op rejection downstream, but the press packet WAS emitted).
- End-to-end outcome correct (door Closed at 16:25:42, 13061ms — 1s slower than 12049ms median; could be sensor variance OR could be second press interference)
- NO TX queue >8 warnings in this audit (the comms-layer overlap-rejection caught the burst before saturating TX). If the GDO were slower, this could escalate to finding 008's TX-saturation outcome.
Cross-reference: Finding 008 dedup-at-HK-callback approach is the right primary fix. Additionally, the .80 race-fix gates only the release callback — it does NOT gate `force_close_door()` against rapid re-entry immediately after `forceCloseInProgress` clears, which is what enabled the second sequence here. Auto-fix-eligibility: **needs-human-planning** (intersects HK callback dedup + finding 007's FC entry path + finding 008's TX guard).

### [P1] log-audit-20260513-007 — force-close release callback runs after `clear_force_close_state` → counter=0 misattribution
**Status:** shipped in .80 (commit a37a0b3 + 6046884) and carried into .81/.82; **soak validation still pending** — race window not yet exercised (5 sequences observed cumulatively, all non-race-path)
**Source:** log-audit 2026-05-13 (18h, 1440 lines); re-checked 2026-05-14 (3.5h, 281 lines); re-checked 2026-05-15 (24h, 1888 lines) on .82
**Issue:** (not filed)
**Acceptance:** force-close release callback short-circuits when `forceCloseInProgress` was cleared mid-hold; no `release sent (2-attempt mechanic, attempt 0/2)` log; the new `FORCE CLOSE: release sent, sequence already cleared (door began Closing during hold)` log line appears when the race fires; 7+ force-close sequences over 24h soak with no door-reverses-after-close-command behaviour.
**Notes:** P1 user-visible failure. Fix verified in source at comms.cpp:2813-2850 (entry-check on forceCloseInProgress + ESP_LOGI "sequence already cleared"). Device on .82 (status.json firmwareVersion="3.4.4-forceclose.82", uptime ~27.5h at 2026-05-15 audit end, crashCount=0). **5 HK-initiated FC sequences observed cumulatively across .80+.81+.82, ALL took the non-race path** (Closing-during-hold/TTCtimer.detach). New on this audit: FC#3 (2026-05-14 16:25:30, Closing concurrent with press), FC#4 (2026-05-14 18:48:08, Closing 1.37s into 2500ms hold), FC#5 (2026-05-14 20:00:12, Closing 1.40s into hold). The `sequence already cleared` race log line has **NEVER appeared** — the user's GDO consistently enters Closing within 1.0-1.5s, well inside the 2500ms hold, so the post-clear release-callback race window has not been naturally exercised. Code review confirms fix is correct; soak metric ("7+ sequences with no reverse") trivially met by the non-race path but the targeted race log condition (the actual acceptance criteria) remains untriggered. Auto-fix-eligibility: **shipped, awaiting telemetry** — consider re-classifying acceptance to "verified via static review" since natural race-trigger frequency may be measured in months. NOTE: finding 009 newly identified a related symptom (second FC sequence reentry after first clears mid-burst) that the .80 fix does NOT cover — see 009 above.

### [P1] log-audit-20260514-008 — iOS HomeKit Open-command burst floods TX queue → release packets dropped, door-supposed-to-be-opening false-error
**Status:** queued (originally observed on .79 pre-OTA at 2026-05-13T19:50:03 CDT; **NO RECURRENCE** in 2026-05-15 audit window — recurrence count=1)
**Source:** log-audit 2026-05-14 (Pi syslog window 2026-05-14T00:22Z -> 2026-05-14T01:49Z, ~85min, 323 lines); re-checked 2026-05-15 (24h, 1888 lines) — only 2 isolated "door target: Open" events, no TX queue depth>8, no packet drops
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
