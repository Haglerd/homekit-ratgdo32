# /log-audit-and-fix

Combined log audit + autonomous fix pipeline. Used by the scheduled task for unattended monitoring.

## Pipeline

1. Invoke `log-auditor` agent → pulls device + Pi logs since checkpoint, appends new findings to QUEUE.md
2. If new findings count > 0, evaluate top eligible item against safety rails (below)
3. If eligible, invoke pipeline: fetch the linked issue's plan (it was generated at audit time), skip planner, route directly to `software-engineer` → `code-review` → `unit-tester` → `/pr` (with `Closes #<issue-number>`)
4. If no eligible item, report "queued N findings, none auto-fixable, awaiting human triage" and exit

## Safety rails — auto-fix eligibility

A finding can be picked for auto-fix ONLY IF all of these are true:

- **Severity** is P0 OR P1
- **Status** is `queued`
- **Source** is `log-audit` (don't auto-fix human-curated audit findings — those wait for /queue-next manually)
- **Has linked issue with embedded plan** (issue body includes "Recommended fix (planner sub-agent output)" with actual content, not "Needs human planning")
- **Auto-fix eligibility marker** in the issue body says `auto-fixable` (NOT `needs-human-planning`)
- **NOT** touching force-close / auto-close state machines
- **NOT** a heap-budget change > 5KB delta
- **NOT** touching > 3 files
- **Recurrence count >= 2** OR **severity P0**

If multiple items pass, pick highest priority + earliest recurrence. Only ONE auto-fix per run.

## Hard stops

- Branch-guard hook flags branch shift → abort, surface to user
- Fork-PR hook would block (would happen at gh pr create) → abort
- AI-attribution hook would block → abort, fix message
- pio build fails locally before commit → leave WIP, mark item `in-progress (build failed)`, exit

## After auto-fix

- PR opened against `Haglerd/homekit-ratgdo32` main with full description (log evidence + plan summary + heap impact)
- QUEUE.md item marked `done <pr-url>`, moved to "Recently completed"
- Checkpoint state file updated
- User reviews the PR in the morning, merges or closes

## Don't

- Don't merge the PR. Ever. PR is the human review gate.
- Don't run more than 1 fix per scheduled invocation. Preserves quality control.
- Don't auto-pick force-close / auto-close state-machine items. The fork's own rule says these need state diagrams and planner review. The planner can produce them, but an autonomous chain can drift; better to wait for a human-in-loop session.
- Don't run /audit (the broad code audit) automatically. That's a separate manual decision.
