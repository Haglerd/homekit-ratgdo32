# /log-audit

Pull the ratgdo Pi syslog, analyze for new issues since last checkpoint, append findings to QUEUE.md.

## Steps

1. Invoke the `log-auditor` agent
2. Auditor reads `.claude/.log_audit_state` for last-checked timestamp
3. Auditor SSHes to Pi (`dakot@100.121.96.114` via `~/.ssh/pi_key`), pulls syslog since checkpoint
4. Auditor pattern-matches across:
   - **P0**: panics, OOM, watchdog resets, reboot loops
   - **P1**: heap leaks, SSE counter desync, WiFi flapping, HomeKit pair loss, force-close stuck
   - **P2**: prolonged HomeKit silence, mDNS rate limits, web_loop throttle bursts
   - (P3 informational events not queued — too noisy)
5. Auditor cross-references existing QUEUE.md to flag recurrences vs new findings
6. Auditor appends new items as `log-audit-YYYYMMDD-NNN` with log evidence in Notes
7. Auditor updates `.claude/.log_audit_state` with current UTC timestamp
8. Reports: window, lines analyzed, findings by severity, recurrences, new items added

## Cadence options

- **Manual**: type `/log-audit` whenever you want a check
- **In-session loop**: `/loop 6h /log-audit` — runs every 6 hours while the session is alive
- **Truly unattended**: wrap in a Windows Task Scheduler job that opens Claude Code with `/log-audit` (the scheduled-remote-agent path doesn't work because Anthropic's infra can't reach your Pi over Tailscale)

## After /log-audit

Run `/queue-next` to start consuming the new findings through the pipeline.

## Don't

- Don't run /log-audit and /queue-next in the same turn — keep them decoupled so you can review what got queued before agents start fixing
- Don't manually clear `.claude/.log_audit_state` unless you intentionally want to re-process old logs (it's gitignored, never committed)
