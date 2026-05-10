# homekit-ratgdo32 work queue

Priority-ordered. Top = next. Detailed analysis lives in `audit-notes/` (gitignored fork-internal). Done items are removed — git log is the durable record.

## Active

### [P1] AUDIT-001 — current_millis > timeout raw comparisons regress at 25-day uptime
**Source:** codebase-audit 2026-05-10
**Complexity:** S
**Description:** Two timer sites still use the unsafe direct-compare pattern instead of the wrap-safe (int32_t)(now - timeout) > 0 form used elsewhere. After ~25 days _millis() stays well within unsigned range but the future-stamped timeout wraps before current_millis does, inverting the comparison so the action either never fires (motion/occupancy clear) or fires every loop tick (WiFi reconnect storm).
**Acceptance:** both sites use the wrap-safe (int32_t)(now - X) >= 0 pattern OR the unsigned now - X > THRESHOLD form. Cross-check comms.cpp:4072 for the canonical fork-internal idiom.
**Files:** src/comms.cpp:2391 (room occupancy clear), src/ratgdo.cpp:523 (WiFi connect timeout)

### [P3] AUDIT-002 — SSEBroadcastState LOG_MESSAGE / RATGDO_STATUS branches duplicate ~80 lines each
**Source:** codebase-audit 2026-05-10
**Complexity:** M
**Description:** The two branches in SSEBroadcastState (web.cpp:3324, 234 LoC) contain near-identical write paths: oversize-payload snprintf_P(NULL,0,...)+malloc+free+counter bookkeeping AND normal-path clientWriteEx + tri-state OK / BUFFER_FULL / FAILED counter updates. ~160 duplicated lines. Refactor candidate: extract sse_write_payload(SSESubscription&, const char* prefix, const char* data) returning SseWriteResult; both branches collapse to ~4 lines each.
**Acceptance:** SSEBroadcastState body shrinks meaningfully (target <100 lines from current 234); LOG_MESSAGE/RATGDO_STATUS branches share one helper for the oversize-payload + counter-update logic; no behaviour change in 24h SSE soak.
**Files:** src/web.cpp:3324-3554

### [P3] AUDIT-003 — handle_setgdo 3 near-identical clamp blocks
**Source:** codebase-audit 2026-05-10
**Complexity:** S
**Description:** handle_setgdo (web.cpp:2271, 204 LoC) contains three separate if (key == "...") { strtol -> clamp -> to_string } blocks for autoClose-, hkAutoRecover/Hint-, and forceCloseHoldMs key sets. All identical shape. Collapsing to a static const struct { const char* key; long lo, hi; } intClamps[] table walked once per arg drops 41 lines to ~15 and centralizes clamp policy.
**Acceptance:** single integer-clamp table replaces the three if-chains; new clamp keys take 1 row instead of 6 lines; behaviour preserved (POST autoCloseMinutes=99999999 still clamps to AUTO_CLOSE_MAX_MINUTES).
**Files:** src/web.cpp:2316-2356

### [P3] AUDIT-004 — Dead #defines in ratgdo.h
**Source:** codebase-audit 2026-05-10
**Complexity:** S
**Description:** DEVICE_NAME, MANUF_NAME, SERIAL_NUMBER, CHIP_FAMILY are defined but never referenced in src/. Leftovers from a HomeKit registration pattern superseded by HomeSpan APIs. MODEL_NAME IS still used (web.cpp:1963 mDNS service-txt) — keep that one.
**Acceptance:** four unused macros removed; build clean; grep -r confirms no remaining references in src/ or platformio.ini.
**Files:** src/ratgdo.h:52,53,54,57,60

### [P3] AUDIT-005 — Unchecked malloc returns in setup_config
**Source:** codebase-audit 2026-05-10
**Complexity:** S
**Description:** Nine consecutive static_cast<char *>(malloc(...)) calls in setup_config with no NULL check before the very next strlcpy(buf, ...). Runs at boot when heap is plentiful so practical risk is low, but pattern survived MH4/W42 audit passes that tightened similar code elsewhere. NULL return -> next strlcpy is a NULL-deref panic on a fresh boot with allocator pressure.
**Acceptance:** each malloc paired with NULL check + ESP_LOGE + abort()/safe-fallback, OR a single batch-allocate helper that fail-fast logs once. No behaviour change on success path.
**Files:** src/config.cpp:447-463

### [P3] AUDIT-006 — Unsafe strcpy / sprintf in setup paths
**Source:** codebase-audit 2026-05-10
**Complexity:** S
**Description:** Two remaining unsafe-string-fn callers: strcpy(tBuffer, "[no time set]") in utilities.cpp where the surrounding code uses strlcpy/snprintf throughout, and sprintf(&qrPayload[16], "%-4.4s", &setupID[1]) in homekit.cpp. Both demonstrably safe today by argument shape, but they are the only strcpy/sprintf calls left in the codebase — silencing them buys forward warning-clean status under future stricter sweeps and matches surrounding discipline.
**Acceptance:** zero strcpy(, sprintf(, strcat(, gets( matches in src/*.cpp|h. Replace with strlcpy / snprintf taking the destination size.
**Files:** src/utilities.cpp:134, src/homekit.cpp:230

### [P3] AUDIT-007 — hkRecoverAttempts/hkLastHintLevel/hkConsecutiveHealthyTicks non-atomic across esp_timer / loopTask
**Source:** codebase-audit 2026-05-10
**Complexity:** S
**Description:** Three watchdog state vars (homekit.cpp:542-553) are non-volatile, non-atomic uint8/uint32. Written from homekit_health_log (esp_timer Ticker) AND from homekit_refresh_watchdog_config (loopTask, settings save). Author comment at line 605 acknowledges the torn-write tolerance. Flagged only because the surrounding hkCfg* family DID get the volatile + __atomic_* treatment (lines 558+); inconsistency invites future regressions.
**Acceptance:** either bring the three state vars into the existing atomic discipline (volatile + __atomic_load/store) for consistency, OR add an inline comment at the declarations pointing to the line-605 rationale so future readers do not mis-fix it.
**Files:** src/homekit.cpp:542,543,553 (declarations) + 605-612 (intentional torn writer)

### [P3] AUDIT-008 — Top-3 source files all >2x the 1500-LoC threshold
**Source:** codebase-audit 2026-05-10
**Complexity:** L
**Description:** comms.cpp 4187 LoC (setup_comms 240, update_door_state 201, comms_loop 159), web.cpp 3781 LoC (handle_firmware_upload 238, SSEBroadcastState 234, handle_setgdo 204), homekit.cpp 2282 LoC (homekit_health_log 240, setup_homekit 181). Not a fix in itself — a flag so future feature additions consider sibling-file extraction (e.g. comms_force_close.cpp, web_sse.cpp) instead of piling more into the largest files.
**Acceptance:** when next force-close or SSE feature work begins, planner first proposes whether the addition warrants a new sibling source file. No code change required from this finding alone.
**Files:** src/comms.cpp, src/web.cpp, src/homekit.cpp

### [P3] AUDIT-009 — loopTaskScratchBuf512 invariant relies on hand-audited callers
**Source:** codebase-audit 2026-05-10
**Complexity:** S
**Description:** File-scope 512-byte scratch buffer (web.cpp:430) reused across load_page (303 redirect URL formatting), OTA progress frames, and ESP8266 SSEBroadcastState. Comment block at 411-429 documents the loopTask-only writer invariant — this is exactly the W7/v38 race that motivated extracting the ESP32 SSEBroadcastState path to a stack-local. The invariant is enforced ONLY by reviewer discipline; a new Ticker-context caller would silently corrupt in-flight writes.
**Acceptance:** add assert(xTaskGetCurrentTaskHandle() == loopTaskHandleForHWM) (or a LOOPTASK_SCRATCH_ASSERT() macro that is a no-op in release) at every read/write site of loopTaskScratchBuf512, OR rename to LOOPTASK_ONLY_scratchBuf512 so casual greppers pause.
**Files:** src/web.cpp:430 (decl), 1557,1562,1564,1565,1566,1567 (load_page) + ESP8266-only SSEBroadcastState callers

### [P3] AUDIT-010 — crashptr / test_str definition gating possibly inconsistent with usage
**Source:** codebase-audit 2026-05-10
**Complexity:** S
**Description:** void *crashptr; and char *test_str = NULL; are file-scope globals at web.cpp:97-98. Their only readers are inside #ifdef CRASH_DEBUG blocks (handle_crash_oom at 3311, handle_forcecrash at 3320). Trace shows the declarations at 95-99 are wrapped in #ifdef CRASH_DEBUG — verify this is not double-defined or accidentally always-built; if always-built they are 8 bytes of unconditional BSS for a debug-only feature.
**Acceptance:** confirm crashptr and test_str are only emitted into the binary when CRASH_DEBUG is defined; otherwise tighten the gating.
**Files:** src/web.cpp:94-99,3303-3322

---

## Deferred — soak-pending only

### [P3] HEAP-PRESSURE-WATCH — `mdns_networking: Cannot allocate memory` events under sustained operation
**Status:** deferred (`.78` fixes the lethal symptom; root cause is heap pressure)
**Source:** 2026-05-09 .77 panic-loop investigation. Crash dump showed `free heap: 136 bytes` when mdns_mem_calloc failed. iOS hub state-sync + HomeSpan HAP + mDNS query bursts can momentarily consume nearly all heap. With `.78`'s fix the resulting log line is benign — device handles it cleanly.
**Acceptance:** identify what's consuming heap during these bursts (heap-trace, periodic snapshots) and either bump tcpip MEMP pools / mdns rx-buf / cap HAP transaction concurrency, OR conclude the existing 30-50 KB headroom is enough now that the panic chain is fixed.
**Notes:** panic chain was the priority — fixed at the syslog re-entry point so 100% heap exhaustion in mDNS no longer crashes. Heap pressure itself is pre-existing across many .x releases; investigate if device experiences other symptoms (slow HAP, dropped notifications) under sustained low-heap conditions. Not user-visible right now.

### [P2] HANG-WATCH — `.74` 11-min firmware hang
**Status:** deferred (rebaselined to `.79`; reopen if hang recurs on `.79+`)
**Source:** 2026-05-07T16:02-16:13 — `.74` device went silent for ~11 minutes (no panic logged, no watchdog reset, no WiFi disconnect), required power-cycle to recover.
**Updated 2026-05-09:** the `.77` panic loop (13 panics overnight, all `tiT` re-entry into lwIP from `mdns_mem_calloc → ESP_LOGE → logToSyslog → socket()`) shares root cause with the `.74` hang — broken cached-handle gate from PR #105. PR #122 (`.78`) added `pcTaskGetName(curTask)` + strcmp gate; `.78` ran 10h46m then panicked once on task `mdns` (not in the gated set). PR #126 (`.79`) closes the `mdns` hole and adds a 4 KB heap-floor short-circuit. 24h soak now restarts on `.79`.
**Acceptance:** `.79` runs 24h+ clean with iOS-quiet stretches AND mDNS OOM events handled without panic OR hang → declare `.74` hang, `.77` panic-loop, and `.78` mdns-task panic all fixed.
**Notes:** Per-CPU1 watchdog flip (`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=y`) tracked as a defensive option if any future hang fires — would produce a reset reason instead of dying silent.

---

## Upstream filing — DO NOT FILE

The fork's bug fixes (R1-R4 in `audit-notes/UPSTREAM_CHERRY_PICK_PLAN.md`) are already shipped in the fork. Their upstream-applicability is tracked in audit-notes/ for awareness only — **fork work proceeds regardless of upstream interest**.

**Hard rule:** No upstream PRs, no upstream issue filings, no cherry-picks to upstream. Issue #170 was filed historically; do not let it set precedent. Future agents reading this queue: if you find yourself drafting a `gh pr create --repo ratgdo/...`, STOP — that's not how this fork operates.
