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
9a. **Auto-unblock dependent items.** Scan QUEUE.md for items whose `**Status:**` line contains `blocked` AND whose `**Notes:**` / `**Pre-req:**` text references the just-completed item's id (e.g. `log-audit-20260507-003`, `BOOT-OOM-MDNS`, `W42`). Flip those items from `blocked` back to `queued` (preserve any other status modifiers like `needs-human-planning`). Commit + push the flip together with the "done" mark from step 9. **This is mandatory** — without it, dependent items linger as `blocked` forever and the agent skips them on every subsequent iteration.
10. **On code-review architectural problem**: re-invoke planner with code-review findings as context. Loop up to 3 planner-revision iterations.
11. **On unit-test failure**: engineer fixes → code-review → retest. Loop up to 3 iterations.
12. **On hook fire**: apply auto-recovery, retry up to 3 times on same hook+item.
13. **On success**: loop back to step 0 unless cap reached or hard stop fires.

## End-of-batch release: manifest bump

After the drain finishes (cap reached, queue empty, or last eligible item shipped), evaluate whether to cut a release:

1. **Compute what merged this drain** — `gh pr list --search "merged:>=<drain-start-time>" --repo Haglerd/homekit-ratgdo32 --state merged --json files,mergedAt`
2. **Check if any merged PR touched firmware code** — files matching `src/*.cpp`, `src/*.h`, `src/*.hpp`, `platformio.ini`, `partitions.csv`, `sdkconfig.defaults`. Web UI changes (`src/www/*`) also count if you ship them with firmware.
3. **If yes**: bump the patch version of `docs/manifest.json`'s `version` field AND every URL inside `builds[].parts[].path` that includes the version string. Pattern: `v3.4.4-forceclose.<N>` → `v3.4.4-forceclose.<N+1>`. Commit on `main` with message: `release: bump manifest to v3.4.4-forceclose.<N+1>`. Push. **`auto-release.yml` will fire on the push** → firmware build + GitHub Release.
4. **If no firmware files merged**: skip the bump. Doc-only / lint / .claude-only PRs don't need a release.

This is the LAST step of the drain — runs once per drain, regardless of how many PRs merged.

Manifest paths to update in lockstep with `version`:
- `version`: top-level
- `builds[0].parts[].path` (ESP32) — every URL contains the version
- `builds[1].parts[].path` (ESP8266 if present) — every URL contains the version
- `manifests` array entries if present

If the manifest has a script (`tools/bump-manifest.sh` or similar) — use it. Otherwise edit the JSON directly with `jq` or sed.

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

This list is **EXHAUSTIVE.** No other reason halts a drain. In particular:

- ❌ "context is getting heavy" / "checkpoint here" → **NOT a halt reason.** The user has stated 3+ times: autonomous execution, no self-imposed pauses. If you feel context-bound, finish the current item, pick the smallest remaining queued item, and keep going. Only the listed hard-stops halt. If a halt happens for any other reason it is treated as an autonomy violation.
- ❌ "want to confirm with the user before continuing" → not a halt; the user pre-authorized the whole drain by invoking `/queue-next`.
- ❌ "this PR introduced something risky, let me pause" → still not a halt; if code-review caught something real, that's the planner-revision-iteration loop (see below). Otherwise file the PR and continue.

**Valid hard-stops (these and only these):**

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
