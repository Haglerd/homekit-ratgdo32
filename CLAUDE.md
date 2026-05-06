# homekit-ratgdo32

ESP32 firmware fork that exposes a ratgdo garage-door controller as a HomeKit accessory. Fork at `Haglerd/homekit-ratgdo32`. **Not DaqsPickEm-related — no PHP, no DB, no `version.php`.**

> **🚫 NEVER push changes upstream unless the user explicitly says so.** All commits, branches, and PRs go to `Haglerd/homekit-ratgdo32` only. Bugs that also exist in `ratgdo/homekit-ratgdo32` (or the ESP8266 sibling `ratgdo/homekit-ratgdo`) get fixed in our fork only. Do not propose upstream PRs, cherry-picks, or "should we contribute this back?" — even if the fix looks broadly useful. The user has been burned by this multiple times. Default `--repo Haglerd/homekit-ratgdo32` on every `gh pr create`.

## Stack

- **Firmware**: C/C++ on **ESP32 + ESP8266** (PlatformIO + Arduino + ESP-IDF, HomeSpan, async web server, mDNS, NVS/Preferences, FreeRTOS)
- **Web UI**: gzipped + CRC'd into firmware flash, files under `src/www/`
- **CI/CD**: GitHub Actions builds firmware and webcontent

## Hard constraints (real pain — not theoretical)

- **ESP8266 heap is *very* tight.** Every buffer-size bump is a budget question. Use `#ifdef ESP32` to gate heavy features off ESP8266.
- **`esp_timer` task ≠ `loopTask`** — functions reachable from TTC timer callbacks must not assume single-threaded loopTask state. The `clear_force_close_state` invariant was broken by exactly this.
- **Time math uses unsigned types.** `(int32_t)(now - past)` regresses after 25 days of uptime.
- **Force-close / auto-close state machine** is the #1 bug source. Touching it requires a state diagram and reasoned transition coverage.
- **Fork-only PRs**: `gh pr create --repo Haglerd/homekit-ratgdo32`. The CLI has misfired against upstream multiple times — explicit `--repo` every time.

## Build tools — pio is CI-only on this machine

**PlatformIO is NOT installed locally.** No `~/.platformio/`, no `pio.exe` in PATH, no VSCode extension. Builds happen on GitHub Actions, not locally.

**For autonomous fix loops:** any QUEUE.md item whose `Acceptance` requires running `pio run` locally must be marked `blocked: pio not installed locally; verify via CI after merge`. The agent does NOT halt the drain — it pivots to the next pio-independent item.

Pio-INDEPENDENT items (can run autonomously):
- Hygiene refactors that don't change behavior (W41 header move, W43 buffer rename — verified by `git grep`)
- Documentation audits (W48)
- Ticker.detach() comment sweep (W47 — comments only; the comms.cpp:3318 fix needs build verify, gate that piece)
- npm-based tooling (W46 eslint — `npx works` per local check)
- ssh-based Pi log work

Pio-DEPENDENT items (mark blocked, defer to CI):
- W45 `-Wshadow=local` build flag + warning triage
- W42 mutex on userSettings::get (build verify needed)
- W44 DST cap (build verify needed)
- Any change that produces firmware behavior changes

If you want these auto-fixable locally, install PlatformIO Core (`pip install platformio` or use the VSCode extension); the path will then live at `~/.platformio/penv/Scripts/pio.exe`. **Never invoke `pwsh.exe` / `powershell.exe`** to wrap pio — they're in the deny list and will pause the session.

## Pi log access

ratgdo syslog forwards to `dakot@100.121.96.114` (UDP 5140), files at `/var/log/ratgdo.log` + `.1` + `.2.gz`. World-readable. `ssh -i ~/.ssh/pi_key dakot@100.121.96.114 'cat /var/log/ratgdo.log'` for live tail.
