# /queue-next

Pick the top actionable item from `QUEUE.md`, route it through the agent pipeline, report back when done.

## Steps (per item — looped until queue empty, cap reached, or stop condition)

0. **Defensive branch sync** — before starting work on each item, run `git checkout main && git pull --ff-only origin main`. This preempts the auto-release-workflow-shifted-the-checkout trap that otherwise causes the branch-shift guard to block our next commit. Reset the branch-shift stamp file (`rm .git/.claude_session_branch`) so the hook re-baselines on `main` for this iteration.

1. **Read `QUEUE.md`** — focus on `## Active — fork-internal` and `## Fork-internal investigation`. Ignore the "Upstream filing — DO NOT FILE" section; that's tracking, not work.
2. **Pick the top item** by priority (P0 > P1 > P2 > P3) and `Status: queued`. Skip `in-progress` / `blocked` / `deferred`. If no eligible items, report "queue empty" and stop.
3. **Mark it `in-progress`** in the queue file.
4. **Route based on item type:**
   - **Item has `**Issue:**` field with embedded plan** → fetch issue body via `gh issue view <number> --repo Haglerd/homekit-ratgdo32`, use the embedded plan, go to `software-engineer`.
   - **Item is `needs-human-planning`** (per issue label or Notes) → invoke planner; planner ALWAYS produces a plan (picks default-with-rationale on ambiguity), proceed to engineer. PR is the user's review gate, not a pre-PR halt.
   - **Tooling sweep (W45/W46)**: software-engineer directly — well-spec'd in audit-notes, no planner needed
   - **Hygiene refactor (W41/W43)**: software-engineer directly
   - **Concurrency fix (W42)**: planner first (mutex placement risks), then software-engineer
   - **Verification gate (W44)**: software-engineer reads the gate condition, decides path A vs marks `done: verified non-applicable`
   - **Documentation audit (W48)**: software-engineer (writing task)
   - **Investigation item (R-?-fork)**: software-engineer reads HomeSpan source/docs first; close as non-finding OR file follow-up fork-internal item
5. **Run through the pipeline:** software-engineer → code-review → unit-tester.
6. **Audit-notes update**: when complete, update `audit-notes/2026-05-04-fork-vs-upstream-attribution.md` to move the finding from "Open" to "Done" or `audit-notes/2026-05-04-fork-vs-upstream-attribution - Whats Done.md`.
7. **PR via /pr** (always `--repo Haglerd/homekit-ratgdo32`). If item has `**Issue:**` field, include `Closes #<number>`.
8. **Wait for CI + merge** — agent owns the merge:
   - `gh pr checks <#> --repo Haglerd/homekit-ratgdo32 --watch`
   - If green → `gh pr merge <#> --repo Haglerd/homekit-ratgdo32 --squash --delete-branch`
   - If red → leave open, surface failures, continue to next item
9. **Update queue**: mark `done <pr-url>`, move to "Recently completed".
10. **On code-review architectural problem**: re-invoke planner with code-review findings as context. Loop up to 3 planner-revision iterations.
11. **On unit-test failure**: engineer fixes → code-review → retest. Loop up to 3 iterations.
12. **On hook fire**: apply auto-recovery, retry up to 3 times on same hook+item.
13. **On success**: loop back to step 0 unless cap reached or hard stop fires.

## Drain summary report

After drain (or stop):
```
Queue drain summary (homekit-ratgdo32):
- Items processed: N
- PRs opened: <list of urls>
- Stopped at: <item + reason, if applicable>
- Queue remaining: <count>
```

## Partial drains are normal — re-run picks up where it stopped

If a hook block, branch shift, or item failure stops the drain at item 4 of 9, the remaining 5 items stay `queued` in QUEUE.md. **Just run `/queue-next` again** — it'll pick up the next eligible item and continue. The queue is durable; nothing's lost on partial drains.

## Environmental blockers — auto-pivot, don't halt

When an item can't be processed due to a tool/access constraint (NOT a code bug), **mark it `blocked: <reason>` and continue to the next eligible item.** Don't surface a "which option do you want" question — pick the next item autonomously. This is the autonomy goal: environmental issues never halt the batch.

Examples:
- `pio` not on bash PATH AND not at `~/.platformio/penv/Scripts/pio.exe` → **pio is not installed locally on this machine**; mark item `blocked: pio not installed locally; verify via CI after merge` and pivot. Items needing pio (W45/W42/W44 + any firmware-behavior change) will always be blocked until pio is installed or a CI-build-then-fix workflow exists.
- SSH to Pi times out → mark `blocked: pi unreachable`, pivot.
- Device HTTP endpoint times out → mark `blocked: device unreachable`, pivot.
- A required env var or file not present → mark blocked, pivot.

**Items pivot-able without pio**: W41 (header move), W43 (rename), W47 (comment sweep — defer the comms.cpp:3318 fix), W48 (doc audit), W46 (eslint via npx).

Code-bug failures (build fails on real syntax error, tests fail on logic) are NOT environmental — those go through the hook-recovery retry budget and STOP if exhausted.

## Hard stop conditions (last-resort halts only)

- Queue is empty → report and stop.
- Cap reached → report and stop.
- Top item is `deferred` → don't unilaterally promote.
- 3 planner-revision iterations on same item didn't converge.
- 3 engineer+test iterations didn't pass on same item.
- 3 hook auto-recovery attempts in a row failed on same hook+item.
- About to draft `gh pr create --repo ratgdo/...` (upstream) → STOP — fork doesn't file upstream. The pre-tool-use fork-PR hook will block this anyway.

Each halt files a comment on the linked issue summarizing every attempt (plans considered, code-review feedback per attempt, test failures). Halt is a hand-off with full context, not an early bail.

## Don't

- Don't draft upstream PRs or upstream issues. Ever. No exceptions, no "this would be useful upstream" reasoning.
- Don't bundle multiple W4x items into one commit unless the v45 plan explicitly groups them (see commit-boundaries table).
- Don't skip the heap-budget skill on any cpp/h edit that touches buffer sizing.
