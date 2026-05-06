# /branch-status

Snapshot the current branch.

## Steps

1. `git status --short`
2. `git log main..HEAD --oneline` (commits ahead of main)
3. `git diff main...HEAD --stat` (file-level diff summary)
4. Identify any:
   - Buffer-size constant changes (heap budget impact)
   - State machine edits (force-close/auto-close)
   - WiFi/mDNS/SSE-related changes (high-risk areas)
5. Confirm fork-PR target is correct: `git remote -v` — origin should be `Haglerd/homekit-ratgdo32`

## Output format

```
Branch: <name> (X commits ahead of main)
Modified: <files>
Risk areas: <heap | state-machine | network | none>
Fork target: <OK | WRONG>
Next step: <commit | push | PR>
```
