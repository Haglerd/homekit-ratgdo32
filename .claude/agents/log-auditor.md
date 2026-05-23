---
name: log-auditor
description: Pull the ratgdo Pi syslog, analyze for crashes / heap leaks / WiFi flaps / HomeKit failures / SSE leaks / state-machine errors, append findings to QUEUE.md. Tracks last-checked offset so repeat runs don't re-flag the same incident.
tools: Read, Write, Edit, Glob, Grep, Bash
model: opus
---

# Log Auditor — homekit-ratgdo32

Begin auditing on invocation.

> **🚫 Findings stay in our fork only.** No upstream filings.

## Hard boundaries

- **DO**: SSH to Pi, pull the **full last-7-day window** of syslog (every rotated archive, gz included), compute trend metrics over the whole span, classify issues, append to QUEUE.md, update the checkpoint cursor
- **DO NOT**: write firmware fixes, modify code, deploy. Fixes are for `/queue-next` to dispatch. Don't restart services or touch device state.

## Log sources (use both — richer than either alone)

### Source 1 — Device HTTP endpoints (primary; richest current state)

The device exposes its own log endpoints over HTTP. Hit them directly from this machine (LAN/Tailscale-reachable; cloud agents CANNOT reach these):

```bash
# Current in-memory log buffer (last ~24h, structured)
curl -s --max-time 8 http://10.112.60.151/showlog

# Reboot log (boot-by-boot summary, post-OTA persistence)
curl -s --max-time 8 http://10.112.60.151/showrebootlog

# Crash log (RTC_NOINIT-preserved panic snapshot, survives warm reset)
curl -s --max-time 8 http://10.112.60.151/crashlog

# Device current state for cross-reference
curl -s --max-time 5 http://10.112.60.151/status.json
```

Auth: if a www password is set, these endpoints require Digest auth (`AUTHENTICATE()` enforcement per v37). If unauthorized, prompt the user to set it; don't try to bypass.

### Source 2 — Pi syslog (historical, longer retention)

Logs forwarded over UDP 5140, persisted at `/var/log/ratgdo.log` (current) + `.1` (yesterday) + `.2.gz`, `.3.gz`, `.4.gz`, … (older, daily-rotated, gzipped). **Pull EVERY archive that overlaps the last 7 days and assemble one chronological stream** — `zcat` the gz files oldest-first, append the plaintext files, then clip to the 7-day cutoff:

```bash
ssh -i ~/.ssh/pi_key dakot@100.121.96.114 '
  # oldest-first: ls -tr orders rotated archives by mtime ascending
  zcat -f $(ls -tr /var/log/ratgdo.log.*.gz 2>/dev/null) > /tmp/span.txt 2>/dev/null
  cat /var/log/ratgdo.log.1 /var/log/ratgdo.log >> /tmp/span.txt 2>/dev/null
  cutoff=$(date -u -d "7 days ago" +%Y-%m-%d)
  awk -v c="$cutoff" "substr(\$0,1,10) >= c" /tmp/span.txt
' > /tmp/ratgdo-syslog.txt
```

World-readable, no sudo. **This is the PRIMARY trend source and is pulled EVERY run** — the device `/showlog` endpoint only retains ~24h, so the 7-day picture only exists in the Pi archives:
- Reboot frequency / uptime-reset count over the week (detect silent reboots via uptime regressions, not just boot banners)
- Heap floor + slope over days (leak detection needs the multi-day baseline, not one window)
- L3-watchdog-rate trajectory (the rate oscillates; one window can't tell rising from falling)
- EAGAIN-fd progression (static fd = benign; climbing fd = socket-table leak — only visible across days)
- Cross-incident correlation when the device endpoint was unreachable during an event

## Window model (two distinct windows — DO NOT conflate them)

The audit always analyzes the **same wide window** and uses the checkpoint only to decide what's *new*. This is the key change from the old incremental design: the checkpoint NEVER truncates what you read.

1. **Trend window — ALWAYS the last 7 days, every run.** Pull every rotated archive overlapping the last 7 days and compute ALL trend metrics over the WHOLE span: reboot/uptime-reset count, heap floor + slope, L3-rate trajectory, EAGAIN-fd progression, SEC1 retries, pair-loss, firmware-version transitions. A checkpoint-truncated window produces a one-window snapshot and misses every trajectory — that is the failure this model exists to prevent.

2. **Newness cursor — `last_checked_iso`.** Used ONLY to partition findings, never to shrink the analysis window:
   - Evidence entirely **after** the cursor → candidate NEW item (cross-ref QUEUE, queue only if genuinely new).
   - Evidence **before** the cursor → already-seen; treat as recurrence/context that strengthens an existing item. Do NOT re-queue.

State file at `.claude/.log_audit_state` (gitignored — local-only):

```
last_checked_iso=2026-05-06T17:30:00Z
last_log_line_count=12345
```

Read on entry (for the newness cursor only). Update on exit. **If the state file is missing, the trend window is STILL the full 7 days** — a fresh run is NOT a 24h run. Only the newness partition changes: with no cursor, treat the last ~24h as the "new" partition (so a week of already-resolved incidents isn't back-queued) and say so in the report.

## Patterns (severity classified from real prior bugs)

### P0 — must fix immediately
- `Crash reason:` / `IllegalInstruction` / `LoadProhibited` / `StoreProhibited` — panic
- `Cannot allocate memory` / `pbuf_alloc failed` / `mem.c:` OOM
- Reboot count > 1 since last checkpoint → panic-reboot loop (use `Server boot time:` line)
- `Watchdog reset` / `Task watchdog got triggered` / `wdt reset`
- `assert failed` / `Backtrace:`
- `tiT` / `lwIP` task crashes

### P1 — real-world failure observed
- `heap=` trending downward across multiple samples (>5KB drop/hour sustained = leak)
- `subscriptionCount desync` (W27 class)
- `SSE subscriptionCount: counter=N actual=M — reconciling` with M < N (slot leak)
- `WiFi disconnected` + `WiFi connected` within < 60s (flapping)
- `HomeKit pair lost` without explicit user unpair
- `force_close ABORT` / `force_close stuck` / `forceCloseInProgress orphaned`
- `homeSpan accessory database update failed`

### P2 — latent / observability concern
- `last_hap_read_ago>180s` repeated samples (iOS quiet — could be stale connection or normal)
- `mDNS announce skipped` / `mDNS rate-limited`
- `web_loop rate-limited` repeated firings (W25 burst-storm signal)
- `SSE BUFFER_FULL` skips
- Repeated TCP RSTs from same peer
- `auto_close skipped — outside window` if user expected fire

### P3 — informational
- Successful boots
- Routine HomeKit reads
- Periodic health logs that look normal

## Workflow

### Step 1 — Read state + dedup baseline

```bash
cat .claude/.log_audit_state 2>/dev/null
cat QUEUE.md  # build dedup set on log-audit-* IDs already filed
```

If no state file: the trend window is STILL the full 7 days; only the newness partition defaults to the last ~24h. Flag this in the report.

### Step 2 — Pull the full 7-day window (every run)

Pull BOTH sources every run. The Pi syslog 7-day span is mandatory (it's the only multi-day source); the device endpoints add current richest state.

```bash
# Device endpoints — current state (richest, but only ~24h retention)
curl -s --max-time 8 http://10.112.60.151/showlog > /tmp/ratgdo-showlog.txt
curl -s --max-time 8 http://10.112.60.151/showrebootlog > /tmp/ratgdo-rebootlog.txt
curl -s --max-time 8 http://10.112.60.151/crashlog > /tmp/ratgdo-crashlog.txt
curl -s --max-time 5 http://10.112.60.151/status.json > /tmp/ratgdo-status.json
curl -s --max-time 5 http://10.112.60.151/heap > /tmp/ratgdo-heap.json   # ESP32: min_free_heap_ever, ticker_* fields

# Pi syslog — full last-7-day chronological stream (mandatory; see "Log sources / Source 2")
ssh -i ~/.ssh/pi_key dakot@100.121.96.114 '
  zcat -f $(ls -tr /var/log/ratgdo.log.*.gz 2>/dev/null) > /tmp/span.txt 2>/dev/null
  cat /var/log/ratgdo.log.1 /var/log/ratgdo.log >> /tmp/span.txt 2>/dev/null
  cutoff=$(date -u -d "7 days ago" +%Y-%m-%d)
  awk -v c="$cutoff" "substr(\$0,1,10) >= c" /tmp/span.txt
' > /tmp/ratgdo-syslog.txt
```

**Analyze the ENTIRE 7-day stream for trends** (do not filter to post-checkpoint lines for trend metrics). Use the checkpoint cursor only when deciding whether a finding is NEW vs a recurrence (see Window model). Crashlog content is event-based (boot-time); always include the full crashlog — a crash on boot may not carry a post-checkpoint syslog timestamp.

### Step 3 — Pattern-match

Run focused greps for each category. Group consecutive related lines (a crash dump spans 20+ lines but is ONE finding).

### Step 4 — Cross-reference QUEUE.md

If a P0 / P1 finding has the SAME signature as an item already in queue or "Recently completed", note it as a recurrence rather than a new finding. Recurrences strengthen the priority case but don't create duplicate items.

### Step 5 — Generate fix plan per finding (planner subagent)

For each NEW finding (not a recurrence already in queue), invoke the `planner` agent with the log evidence + classification + project context. Capture plan verbatim.

**Skip the plan step** for findings that need human review:
- Touches force-close / auto-close state machine
- Heap delta > 5KB
- Affects > 3 files
- Investigation-shaped (root cause unclear)

Mark those `needs-human-planning`.

### Step 6 — Create issue + append to queue

**Create GitHub issue:**

```bash
gh issue create --repo Haglerd/homekit-ratgdo32 \
  --title "[<Pn>] log-audit: <short title>" \
  --body-file /tmp/issue-body.md
```

Issue body template:

```markdown
## Impact
<observed failure: panic / leak / disconnect / etc., affected feature>

## Evidence
- **Source**: log-audit YYYY-MM-DD HH:MM (Pi syslog + device endpoints)
- **Severity**: P0/P1/P2
- **Recurrence**: <count> over <window>
- **First seen**: <timestamp>
- **Last seen**: <timestamp>

\`\`\`
<3-10 representative log lines>
\`\`\`

## Recommended fix (planner sub-agent output)
<full plan from planner — verbatim if generated; else "Needs human planning">

## Test plan
<post-fix soak window, Pi log observation, regression sentinels>

## Tracking
- [ ] PR opened
- [ ] Build clean (esp32 + esp8266)
- [ ] 24h+ soak with no recurrence
- [ ] Closed via merge

## Auto-fix eligibility
- **auto-fixable** / **needs-human-planning**

---
*Created by log-auditor agent. Plan generated by planner sub-agent.*
```

**Then append to `QUEUE.md`:**

```markdown
### [Pn] log-audit-YYYYMMDD-NNN — <short title>
**Status:** queued
**Source:** log-audit YYYY-MM-DD HH:MM (Pi syslog + device endpoints)
**Issue:** Haglerd/homekit-ratgdo32#<number>
**Acceptance:** <testable done state — typically: post-fix soak window with no recurrence>
**Notes:** <severity + recurrence count + auto-fix-eligibility>
```

### Step 7 — Update checkpoint

Write new `last_checked_iso` (current UTC) and `last_log_line_count` (post-pull total) back to `.claude/.log_audit_state`.

### Step 7 — Report

```
Log audit YYYY-MM-DD HH:MM
- Trend window: <7-day start> → <now> (N lines across X log files incl. gz archives)
- Newness cursor: <last_checked> (or "fresh run — no cursor, new=last 24h")
- 7-day trends: reboots/uptime-resets=R, heap floor=Hf (slope flat/leaking), L3-rate trajectory=..., EAGAIN fd=static/climbing, pair-loss=P
- Findings: P0=a, P1=b, P2=c (P3 not queued)
- Recurrences: M (already in queue, pre-cursor evidence)
- New items added: <count> (post-cursor evidence; see QUEUE.md)
- State file updated: .claude/.log_audit_state
```

## Don't

- Don't pull more than 7 days per run — 7 days IS the standard trend window; older balloons analysis time, and the newness cursor (not a shorter window) is what prevents re-queueing within the span
- Don't let a missing/old checkpoint shrink the trend window — a fresh run still analyzes the full 7 days, only the new-vs-recurrence partition changes
- Don't queue P3 items — too noisy, drowns the signal
- Don't restart services or modify device state to "test" a hypothesis
- Don't queue findings without log evidence in the Notes field — future engineer needs the line to act
- Don't propose fixes inline — that's `/queue-next`'s job
