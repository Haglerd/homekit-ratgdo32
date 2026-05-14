# homekit-ratgdo32 work queue

Priority-ordered. Top = next. Detailed analysis lives in `audit-notes/` (gitignored fork-internal). Done items are removed — git log is the durable record.

## Active

### [P1] log-audit-20260513-007 — force-close release callback runs after `clear_force_close_state` → counter=0 misattribution
**Status:** fixed in working tree (pending soak)
**Source:** log-audit 2026-05-13 (Pi syslog window 2026-05-13T06:03Z -> 2026-05-14T00:22Z, 18h, 1440 lines post-checkpoint)
**Issue:** (not filed)
**Acceptance:** force-close release callback short-circuits when `forceCloseInProgress` was cleared mid-hold; no `release sent (2-attempt mechanic, attempt 0/2)` log; 7+ force-close sequences over 24h soak with no door-reverses-after-close-command behaviour.
**Notes:** P1 user-visible failure. At 2026-05-13T18:45:33 CDT, HK close issued, door began closing (Open->Closing at 36.784), then physically reversed to Opening at 39.837 — door ended back at Open instead of Closed. Root cause: `send_force_close_release_then_maybe_retry` (comms.cpp:2813) does NOT re-check `forceCloseInProgress` at entry. The `door=Closing detected` path at comms.cpp:1122 fires `clear_force_close_state` (counter -> 0, inProgress -> false) mid-hold; the in-flight `delayFnCall`-scheduled release callback still runs, sees `forceCloseAttempt == 0`, falls through the "skip attempt 2" gate at line 2849 (`forceCloseAttempt >= 1 && CURR_CLOSING`), and schedules a phantom attempt 1/2. The gap-arm drain at line 2790 correctly drops the second press (re-checks inProgress), so no phantom press fires — but the misleading log + the release-packet-during-motion still leaves the GDO in a state where it reverses. **State-machine touch — needs human planning per CLAUDE.md hard constraint.** Compare working 10:58 sequence where door stayed Open until *after* release fired (counter correctly read as 1). Window: 1 of 4 FC sequences raced this way (08:59 OK, 10:58 OK, 17:58 OK silent-second-press, 18:45 RACE). Evidence in checkpoint notes. Auto-fix-eligibility: **needs-human-planning**.

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
