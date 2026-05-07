# /log-audit-and-fix

Combined log audit + autonomous fix pipeline. Used by the scheduled task for unattended monitoring.

## Pipeline

1. Invoke `log-auditor` agent → pulls device + Pi logs since checkpoint, appends new findings to QUEUE.md
2. If new findings count > 0, evaluate top eligible item against safety rails (below)
3. If eligible: fetch the linked issue's plan (or invoke planner if no plan / `needs-human-planning` flag — planner ALWAYS produces a plan), route to `software-engineer` → `code-review` → `unit-tester` → `/pr` (with `Closes #<issue-number>`) → **agent merges after CI green**: `gh pr checks <#> --watch` then `gh pr merge <#> --squash --delete-branch`. If CI red, leave open + comment.
4. If no eligible item, report "queued N findings, none auto-fixable, awaiting human triage" and exit.

## Safety rails — auto-fix eligibility

A finding can be picked for auto-fix ONLY IF all of these are true:

- **Severity** is P0 OR P1
- **Status** is `queued`
- **Source** is `log-audit` (don't auto-fix human-curated audit findings — those wait for /queue-next manually)
- **NOT** touching force-close / auto-close state machines (those need explicit human design review)
- **NOT** a heap-budget change > 5KB delta
- **NOT** touching > 3 files
- **Recurrence count >= 2** OR **severity P0**

If multiple items pass, sort by priority (P0 first, then P1) and earliest recurrence; auto-fix up to **5 items per run**. Each gets its own commit + PR. The branch-shift guard hook fires between items — if anything shifted the branch mid-batch, processing stops at that point.

## Recovery from hook fires (autonomy: fix the fix, don't abort)

| Hook | Auto-recovery |
|------|---------------|
| AI-attribution | strip forbidden patterns from commit message, retry |
| Branch-shift | `git checkout main && git pull --ff-only origin main`, reset stamp, retry |
| Fork-PR | rebuild `gh pr create` with `--repo Haglerd/homekit-ratgdo32`, retry |

3-retry budget per hook+item. After exhaustion, mark item `in-progress (auto-fix exhausted)` and continue to next item.

## Hard stops (last resort)

- Cap reached (5/run)
- Queue empty
- 3 planner-revision iterations on same item didn't converge
- 3 engineer+test iterations didn't pass on same item
- 3 hook auto-recovery attempts in a row failed on same hook+item
- pio build fails 3 times in a row after engineer fixes — mark `in-progress (build broken)` and continue

Each halt files a comment on the linked issue with everything tried.

## After auto-fix

- PR opened against `Haglerd/homekit-ratgdo32` main with full description (log evidence + plan summary + heap impact)
- **Agent waits for CI then merges** (`gh pr checks --watch` then `gh pr merge --squash --delete-branch`). If CI red, leaves open + comments.
- QUEUE.md item marked `done <pr-url>`, moved to "Recently completed"
- Checkpoint state file updated

## Don't
- Don't run more than 5 fixes per scheduled invocation (current cap; adjust here if it proves too aggressive or too slow).
- Don't auto-pick force-close / auto-close state-machine items. The fork's own rule says these need state diagrams and planner review. The planner can produce them, but an autonomous chain can drift; better to wait for a human-in-loop session.
- Don't run /audit (the broad code audit) automatically. That's a separate manual decision.
