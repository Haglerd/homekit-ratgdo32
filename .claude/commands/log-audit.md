# /log-audit

Pull the **full last-7-day** ratgdo Pi syslog, analyze trends over the whole span, and append any genuinely-new findings to QUEUE.md.

**Window model:** the audit ALWAYS analyzes the last 7 days for trends (reboots, heap slope, L3 rate, EAGAIN fd). The checkpoint is only a newness cursor — it decides what's a NEW finding vs a recurrence, it never truncates the analysis window. (A fresh run with no checkpoint still reads the full 7 days.)

## Steps

1. Invoke the `log-auditor` agent
2. Auditor reads `.claude/.log_audit_state` for the newness cursor (NOT to limit the window)
3. Auditor SSHes to Pi (`dakot@100.121.96.114` via `~/.ssh/pi_key`), assembles the full last-7-day chronological stream from all rotated archives (`.log` + `.1` + `.2.gz`/`.3.gz`/… via `zcat`), and pulls the device endpoints for current state
4. Auditor pattern-matches across:
   - **P0**: panics, OOM, watchdog resets, reboot loops
   - **P1**: heap leaks, SSE counter desync, WiFi flapping, HomeKit pair loss, force-close stuck
   - **P2**: prolonged HomeKit silence, mDNS rate limits, web_loop throttle bursts
   - (P3 informational events not queued — too noisy)
5. Auditor computes 7-day trend metrics (reboot/uptime-reset count, heap floor + slope, L3-rate trajectory, EAGAIN-fd progression) over the whole span
6. Auditor cross-references existing QUEUE.md; evidence after the cursor = candidate new finding, evidence before = recurrence (don't re-queue)
7. Auditor appends genuinely-new items as `log-audit-YYYYMMDD-NNN` with log evidence in Notes
8. Auditor updates `.claude/.log_audit_state` with current UTC timestamp (advances the cursor only)
9. Reports: trend window + newness cursor, lines analyzed, 7-day trends, findings by severity, recurrences, new items added

## Cadence options

- **Manual**: type `/log-audit` whenever you want a check
- **In-session loop**: `/loop 6h /log-audit` — runs every 6 hours while the session is alive
- **Truly unattended**: wrap in a Windows Task Scheduler job that opens Claude Code with `/log-audit` (the scheduled-remote-agent path doesn't work because Anthropic's infra can't reach your Pi over Tailscale)

## After /log-audit

Run `/queue-next` to start consuming the new findings through the pipeline.

## Don't

- Don't run /log-audit and /queue-next in the same turn — keep them decoupled so you can review what got queued before agents start fixing
- Don't manually clear `.claude/.log_audit_state` unless you intentionally want to re-process old logs (it's gitignored, never committed)
