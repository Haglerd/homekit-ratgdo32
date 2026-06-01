# /log-audit

Pull the **full available** ratgdo Pi syslog, analyze trends over the whole span, and append any genuinely-new findings to QUEUE.md.

**Window model:** the audit ALWAYS analyzes the full retained log span for trends (reboots, heap slope, L3 rate, EAGAIN fd). The checkpoint is only a newness cursor — it decides what's a NEW finding vs a recurrence, it never truncates the analysis window. (A fresh run with no checkpoint still reads everything retained.)

**Retention reality:** the Pi's logrotate keeps `rotate 4` with `delaycompress`, so on-disk history is only ~4 days (the newest archive stays uncompressed), NOT 7. That's by design — every trend metric below is flat over multiple audits and slope direction is clear within 3–4 days, and the frequent audit cadence + newness cursor (not window length) is what prevents re-flagging. Don't treat a <7-day span as a coverage gap. If you ever genuinely need a longer window, bump `rotate 4`→`8` in `/etc/logrotate.d/ratgdo` (log2ram is now 1 GB, so there's room).

## Steps

1. Invoke the `log-auditor` agent
2. Auditor reads `.claude/.log_audit_state` for the newness cursor (NOT to limit the window)
3. Auditor SSHes to Pi (`dakot@100.121.96.114` via `~/.ssh/pi_key`), assembles the full retained chronological stream from all rotated archives (`.log` + `.1` + `.2.gz`/`.3.gz`/…; ~4 days on disk). Each `.gz` is decompressed in its **own** `zcat` invocation — a multi-file `zcat` aborts at the first corrupt archive and silently drops the rest (this once masqueraded as a multi-day gap). Then sanity-checks date coverage and pulls the device endpoints for current state
4. Auditor pattern-matches across:
   - **P0**: panics, OOM, watchdog resets, reboot loops
   - **P1**: heap leaks, SSE counter desync, WiFi flapping, HomeKit pair loss, force-close stuck
   - **P2**: prolonged HomeKit silence, mDNS rate limits, web_loop throttle bursts
   - (P3 informational events not queued — too noisy)
5. Auditor computes trend metrics (reboot/uptime-reset count, heap floor + slope, L3-rate trajectory, EAGAIN-fd progression) over the whole retained span
6. Auditor cross-references existing QUEUE.md; evidence after the cursor = candidate new finding, evidence before = recurrence (don't re-queue)
7. Auditor appends genuinely-new items as `log-audit-YYYYMMDD-NNN` with log evidence in Notes
8. Auditor updates `.claude/.log_audit_state` with current UTC timestamp (advances the cursor only)
9. Reports: trend window + newness cursor, lines analyzed, trends, findings by severity, recurrences, new items added

## Cadence options

- **Manual**: type `/log-audit` whenever you want a check
- **In-session loop**: `/loop 6h /log-audit` — runs every 6 hours while the session is alive
- **Truly unattended**: wrap in a Windows Task Scheduler job that opens Claude Code with `/log-audit` (the scheduled-remote-agent path doesn't work because Anthropic's infra can't reach your Pi over Tailscale)

## After /log-audit

Run `/queue-next` to start consuming the new findings through the pipeline.

## Don't

- Don't run /log-audit and /queue-next in the same turn — keep them decoupled so you can review what got queued before agents start fixing
- Don't manually clear `.claude/.log_audit_state` unless you intentionally want to re-process old logs (it's gitignored, never committed)
