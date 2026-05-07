# /codebase-audit

Weekly static-analysis sweep over the firmware source for refactoring opportunities. **Read-only audit + queue items only. NEVER auto-fixes — refactor PRs require human review.**

Different from:
- `/log-audit-and-fix` — runtime issues from Pi syslog (daily, executes fixes for P0/P1)
- `/audit` (manual, broad) — on-demand deep audit run by user
- This command — narrow, scheduled, file-only

## Pipeline

1. Invoke `auditor` agent (read-only) with scope: `src/*.cpp`, `src/*.h`, `src/www/*`, `platformio.ini`, `partitions.csv`, `sdkconfig.defaults`. **Skip:** vendor headers, `audit-notes/` (gitignored), `.pio/`, `build/`.

2. **Persist auditor changes to git BEFORE evaluating any follow-up.** If `git status --short QUEUE.md` shows modifications, branch (`audit/codebase-audit-<YYYY-MM-DD>-findings`), commit (`queue: codebase-audit <date> findings (<N> items: <comma-list>)`), push, open a PR via `/pr`, squash-merge after CI green. Same anti-loss pattern as `/log-audit-and-fix`.

3. **Dedupe against existing queue items.** Each new finding's title + acceptance fingerprint compared against current QUEUE.md. Skip if already queued.

4. **File new findings as queue items only.** No PRs against firmware code. No auto-fixes. Each finding gets:
   - Title + brief description
   - Source: `codebase-audit <date>`
   - Suggested priority (default P3 for refactors; P2 for security-adjacent dead code; P1 only if the audit catches a latent fatal like the Logger::getInstance() pattern)
   - Complexity guess (S/M/L)
   - Cross-references to related queue items if any

## Scope of findings the auditor looks for

| Category | Examples in this repo's history |
|---|---|
| Dead code | Unused symbols, files only included by deleted call sites |
| Duplication | Multiple impls of same pattern (CSRF dual-impl, dual GameRepository style) |
| Oversized files / functions | Per-file LoC budget exceeded; functions > N lines |
| Architectural drift | New code using a different pattern than the surrounding subsystem |
| Magic numbers / hardcoded paths | Hardcoded `/home4/...` paths, raw timeouts, raw `0x...` |
| Memory hygiene | Unguarded `static` mutables, suspect alloc-no-free patterns, missing `__atomic_*` on cross-task state |
| Force-close FSM hazards | Any new caller of force-close primitives without the v40+ atomicity discipline |
| Heap-budget regressions | Static analysis that suggests >100 B BSS impact on next build |

## Safety rails (binding)

- **NEVER auto-fix.** This command files findings, period. Even "obvious" refactors need human approval before SE picks them up.
- **NEVER touch force-close FSM** even in finding-only mode — flag as `force-close-touch` and surface to user; do not include in regular queue items.
- **No DROP / no DELETE / no DDL** anywhere; this is a static analysis, not a migration.
- **Don't run more than once per 7 days.** Refactor findings don't decay fast; weekly cadence avoids noise + duplicate filings.
- **Cap: 10 findings per run.** If the auditor surfaces more, keep the top 10 by impact and surface the rest as a single "audit found N more items, run /audit manually for full list" entry.

## Schedule (suggested)

Weekly Sunday 03:00 CT (after the daily `/log-audit-and-fix` cycle wraps). Local Windows Task Scheduler. If user wants cloud-routine instead (free, no API spend), note that the auditor reads source files via `gh api repos/.../contents/...` works fine in a sandboxed env — so this command is cloud-routine-eligible if/when you set it up.

## Recovery from hook fires

Same as `/log-audit-and-fix`: 3-retry budget per hook+item. AI-attribution scrub, branch-shift recovery, etc. After exhaustion, mark the finding `auto-fix exhausted: <hook>` (even though we don't auto-fix this stays in the queue body for context) and continue.

## Hard stops

- Cap reached (10 findings/run)
- Auditor returns no new findings (queue is up-to-date — clean week, no action)
- 3 hook auto-recovery attempts in a row failed

## Don't

- Don't auto-fix any finding from this command.
- Don't open PRs against firmware code from this command.
- Don't file duplicates of existing queue items.
- Don't include force-close FSM findings in regular flow — flag separately.
