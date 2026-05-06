# /queue-next

Pick the top actionable item from `QUEUE.md`, route it through the agent pipeline, report back when done.

## Steps

1. **Read `QUEUE.md`** — focus on `## Active — fork-internal` and `## Upstream filing`.
2. **Pick the top item** by priority (P0 > P1 > P2 > P3) and `Status: queued`. Skip `in-progress` / `blocked` / `deferred`.
3. **Mark it `in-progress`** in the queue file.
4. **Route based on item type:**
   - **Tooling sweep (W45/W46)**: software-engineer directly — these are well-spec'd in audit-notes, no planner needed
   - **Hygiene refactor (W41/W43)**: software-engineer directly
   - **Concurrency fix (W42)**: planner first (mutex placement risks), then software-engineer
   - **Verification gate (W44)**: software-engineer reads the gate condition, decides path A vs drop
   - **Documentation audit (W48)**: software-engineer (writing task)
   - **Upstream filing (R2-R4)**: STOP — do not proceed without user saying "file R<n>". Filing upstream is gated; this command only handles fork-internal items autonomously.
5. **Run through the pipeline:** software-engineer → code-review → unit-tester. The audit-notes files have the spec; the agents have the rules.
6. **Audit-notes update**: when complete, update `audit-notes/2026-05-04-fork-vs-upstream-attribution.md` to move the finding from "Open" to "Done" or `audit-notes/2026-05-04-fork-vs-upstream-attribution - Whats Done.md`.
7. **On success**: open a PR via `/pr` (always `--repo Haglerd/homekit-ratgdo32`). Mark item `done <pr-url>` in QUEUE.md. Move to "Recently completed".
8. **On failure**: leave `in-progress` with one-line blocker, surface to user.

## Stop conditions

- Queue is empty → report and stop.
- Top item is from the **Upstream filing** section → STOP. Upstream PRs require explicit user authorization per round.
- Top item is `deferred` → don't unilaterally promote.
- W42-class concurrency edits without a state diagram in the plan → STOP and call planner.
- Force-close / auto-close state machine touched without explicit AC → STOP and call planner.
- Pre-tool-use hook fires (fork-PR, AI-attribution, branch-shift, heap-warn) → resolve before continuing.

## Don't

- Don't autopilot upstream PRs. Ever.
- Don't bundle multiple W4x items into one commit unless the v45 plan explicitly groups them (see commit-boundaries table).
- Don't skip the heap-budget skill on any cpp/h edit that touches buffer sizing.
