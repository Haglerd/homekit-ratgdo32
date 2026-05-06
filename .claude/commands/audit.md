# /audit

Run the auditor agent against the firmware codebase. Append new findings to `QUEUE.md` and `audit-notes/`.

## Steps

1. Invoke the `auditor` agent
2. Auditor reads `QUEUE.md` + existing `audit-notes/*.md` to build dedup set
3. Auditor sweeps codebase across the 8 categories (heap, context safety, time math, state machines, SSE/web, concurrency, ESP8266 portability, build hygiene)
4. Auditor appends NEW findings (not duplicates) to:
   - `QUEUE.md` under `## Active — fork-internal`, sorted by severity
   - `audit-notes/YYYY-MM-DD-auto-audit.md` for detailed mechanism/fix analysis
5. Auditor reports counts: "Added N findings (P0: x, P1: y, P2: z, P3: w). Skipped M duplicates."

## After /audit

Run `/queue-next` to start consuming the new findings through the pipeline.

## Don't

- Don't run `/audit` and `/queue-next` in the same turn — separate them so you can review what got added before agents start fixing.
- Don't bypass the auditor agent. The dedup logic + heuristics are baked there, not in this slash command.
