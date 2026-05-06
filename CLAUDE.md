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

## Build tools — pio.exe location

PlatformIO Core is installed via `pip install --user platformio`. The binary lives at:

```
C:\Users\Dakot\AppData\Roaming\Python\Python310\Scripts\pio.exe
```

That directory is NOT on bash PATH, so call by full path:

```bash
/c/Users/Dakot/AppData/Roaming/Python/Python310/Scripts/pio.exe run -e ratgdo_esp32dev
```

Both forward-slash and Windows-style paths are pre-allowed in `.claude/settings.json`. **Never invoke `pwsh.exe` / `powershell.exe`** to wrap pio — they're in the deny list and will pause the session.

All autonomous build verification (W45 -Wshadow flag triage, W42 mutex build-check, W44 DST build-check, etc.) now works locally — no CI-only blockers.

## Pi log access

ratgdo syslog forwards to `dakot@100.121.96.114` (UDP 5140), files at `/var/log/ratgdo.log` + `.1` + `.2.gz`. World-readable. `ssh -i ~/.ssh/pi_key dakot@100.121.96.114 'cat /var/log/ratgdo.log'` for live tail.
