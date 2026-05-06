# /quick-fix

Single-file fix shortcut — bypass full planner pipeline.

## Use when

- Single file, single function change
- Type fix, typo, log message, missing null check
- Not touching state machines, heap-sensitive buffers, or WiFi/mDNS

## Workflow

1. Locate the file, read it
2. Make the minimal edit
3. Confirm no buffer-size or state-machine touches (if so, abort and route to `planner`)
4. Build locally if possible (`pio run -e <env>`)
5. Commit on a feature branch, push, /pr

If the fix balloons (multi-file or state-touching), STOP and invoke `planner` instead.
