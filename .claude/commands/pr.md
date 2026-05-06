# /pr

Create a pull request for the current branch, **always against `Haglerd/homekit-ratgdo32`** — never upstream.

## Steps

1. Run `git status --short` and `git log main..HEAD --oneline` to see what's in the branch
2. Push the branch if not already pushed: `git push -u origin HEAD`
3. Build the PR title (under 70 chars) and body from the commits — bullet what changed and why
4. Create the PR with explicit fork target:
   ```
   gh pr create --repo Haglerd/homekit-ratgdo32 --title "..." --body "..."
   ```

## Non-negotiable

- `--repo Haglerd/homekit-ratgdo32` is mandatory. The pre-tool-use hook will block calls without it.
- No AI attribution in the PR title or body.
- For ratgdo32 firmware changes, the body MUST include:
  - **Heap impact** estimate (ESP8266 + ESP32)
  - **Test plan**: which board flashed, Pi syslog observation window, reboot count
  - **State-machine touched?** — if force-close/auto-close, link a state diagram
