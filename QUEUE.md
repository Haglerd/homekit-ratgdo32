# homekit-ratgdo32 work queue

Priority-ordered. Top = next. Detailed analysis lives in `audit-notes/` (gitignored fork-internal). Done items are removed — git log is the durable record.

## Active

_(none queued)_

---

## Deferred — soak-pending only

### [P3] HEAP-PRESSURE-WATCH — `mdns_networking: Cannot allocate memory` events under sustained operation
**Status:** deferred (`.78` fixes the lethal symptom; root cause is heap pressure)
**Source:** 2026-05-09 .77 panic-loop investigation. Crash dump showed `free heap: 136 bytes` when mdns_mem_calloc failed. iOS hub state-sync + HomeSpan HAP + mDNS query bursts can momentarily consume nearly all heap. With `.78`'s fix the resulting log line is benign — device handles it cleanly.
**Acceptance:** identify what's consuming heap during these bursts (heap-trace, periodic snapshots) and either bump tcpip MEMP pools / mdns rx-buf / cap HAP transaction concurrency, OR conclude the existing 30-50 KB headroom is enough now that the panic chain is fixed.
**Notes:** panic chain was the priority — fixed at the syslog re-entry point so 100% heap exhaustion in mDNS no longer crashes. Heap pressure itself is pre-existing across many .x releases; investigate if device experiences other symptoms (slow HAP, dropped notifications) under sustained low-heap conditions. Not user-visible right now.

### [P2] HANG-WATCH — `.74` 11-min firmware hang
**Status:** deferred (likely fixed by `.78` redux gate; reopen if hang recurs on `.78+`)
**Source:** 2026-05-07T16:02-16:13 — `.74` device went silent for ~11 minutes (no panic logged, no watchdog reset, no WiFi disconnect), required power-cycle to recover.
**Updated 2026-05-09:** the `.77` panic loop (13 panics overnight, all `tiT` re-entry into lwIP from `mdns_mem_calloc → ESP_LOGE → logToSyslog → socket()`) shares root cause with the `.74` hang — broken cached-handle gate from PR #105. PR #122 (`.78`) replaces the broken cache with `pcTaskGetName(curTask)` + strcmp. Strongly expected to also resolve the `.74` hang shape.
**Acceptance:** `.78` runs 24h+ clean with iOS-quiet stretches AND mDNS OOM events handled without panic OR hang → declare both `.74` hang and `.77` panic-loop fixed. If `.78` hangs in the same shape, escalate.
**Notes:** Per-CPU1 watchdog flip (`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=y`) tracked as a defensive option if any future hang fires — would produce a reset reason instead of dying silent.

---

## Upstream filing — DO NOT FILE

The fork's bug fixes (R1-R4 in `audit-notes/UPSTREAM_CHERRY_PICK_PLAN.md`) are already shipped in the fork. Their upstream-applicability is tracked in audit-notes/ for awareness only — **fork work proceeds regardless of upstream interest**.

**Hard rule:** No upstream PRs, no upstream issue filings, no cherry-picks to upstream. Issue #170 was filed historically; do not let it set precedent. Future agents reading this queue: if you find yourself drafting a `gh pr create --repo ratgdo/...`, STOP — that's not how this fork operates.
