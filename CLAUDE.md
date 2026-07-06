# homekit-ratgdo32

ESP32 firmware fork that exposes a ratgdo garage-door controller as a HomeKit accessory. Fork at `Haglerd/homekit-ratgdo32`. **Not DaqsPickEm-related — no PHP, no DB, no `version.php`.**

> **🚫 NEVER push changes upstream unless the user explicitly says so.** All commits, branches, and PRs go to `Haglerd/homekit-ratgdo32` only. Bugs that also exist in `ratgdo/homekit-ratgdo32` (or the ESP8266 sibling `ratgdo/homekit-ratgdo`) get fixed in our fork only. Do not propose upstream PRs, cherry-picks, or "should we contribute this back?" — even if the fix looks broadly useful. The user has been burned by this multiple times. Default `--repo Haglerd/homekit-ratgdo32` on every `gh pr create`.

## Stack

- **Firmware**: C/C++ on **ESP32 + ESP8266** (PlatformIO + Arduino + ESP-IDF, HomeSpan, async web server, mDNS, NVS/Preferences, FreeRTOS)
- **Web UI**: gzipped + CRC'd into firmware flash, files under `src/www/`
- **CI/CD**: GitHub Actions builds firmware and webcontent

## OTA release procedure (DO IT THIS WAY — the device pulls from Pages, not the release)

The device's "Update from GitHub" button (`src/www/functions.js` ~1071-1268) does **NOT** download from the GitHub release assets. It queries the releases API for the latest non-prerelease, then downloads `firmware.bin` + `firmware.md5` from **GitHub Pages** `docs/firmware/`. `release.yml` commits the 4 bins to `docs/firmware/` on `main`, then Pages redeploys — which **lags the release build by ~1-2 min**. So a release can exist with all assets attached while the device still 404s. During that window the UI shows "no firmware binary attached yet" and "Firmware MD5 checksum file not found on GitHub. Continue anyway?" — i.e. "it isn't there / the md5 files are fucked". **Verifying the release asset md5 is NOT enough** — that's why past ships looked "done" but weren't.

**Steps to ship `vN`:**
1. Verify the firmware delta is only what you intend: `git diff <prev-tag>..HEAD --stat -- src/`.
2. Build all 3 ESP32 envs clean (see pio path below).
3. Bump `docs/manifest.json` (version + 3 download URLs), commit, push to `main` → auto-release.yml tags + creates the release + dispatches release.yml (builds bins, attaches to release, commits the 4 bins to `docs/firmware/`, pushes; Pages then redeploys).
4. **MANDATORY done-gate — do NOT tell the user it's ready until this exits 0:**
   ```bash
   ./tools/verify-ota.sh vN          # polls the DEVICE-FACING Pages URLs until live, or times out
   ```
   It checks: all 4 `docs/firmware/` Pages URLs are 200, the `.md5` is exactly 32 hex chars, the md5 matches the **Pages-served** bin (not just the release asset), the releases API lists `vN` as latest non-prerelease, and the Pages `manifest.json` version matches. Only announce "safe to flash" after it passes.

## Hard constraints (real pain — not theoretical)

- **ESP8266 heap is *very* tight.** Every buffer-size bump is a budget question. Use `#ifdef ESP32` to gate heavy features off ESP8266.
- **`esp_timer` task ≠ `loopTask`** — functions reachable from TTC timer callbacks must not assume single-threaded loopTask state. The `clear_force_close_state` invariant was broken by exactly this.
- **Time math uses unsigned types.** `(int32_t)(now - past)` regresses after 25 days of uptime.
- **Force-close / auto-close state machine** is the #1 bug source. Touching it requires a state diagram and reasoned transition coverage.
- **Fork-only PRs**: `gh pr create --repo Haglerd/homekit-ratgdo32`. The CLI has misfired against upstream multiple times — explicit `--repo` every time.

## Build tools — pio.exe location

PlatformIO Core is installed via `pip install --user platformio`. The binary lives at:

```
C:\Users\Dakot\AppData\Roaming\Python\Python314\Scripts\pio.exe
```

(Python was upgraded 3.10 → 3.14 as of 2026-07-06; the old `Python310` path and the previous `~/.platformio` install are gone. Reinstall if missing: `C:/Python314/python.exe -m pip install --user platformio`.)

**Do NOT invoke pio directly from the Bash tool (git-bash).** A fresh `.platformio` created under git-bash makes ESP-IDF's `idf_tools.py` abort with `ERROR: MSys/Mingw is not supported` (it detects the MSys PATH `/usr/bin` + `/mingw64`), so the xtensa compiler never lands on PATH and the build fails at link. Run pio from a **clean cmd.exe env** instead — scrub the MSys markers and use a native Windows PATH. Wrapper batch (what worked 2026-07-06):

```bat
@echo off
set "MSYSTEM=" & set "MSYS=" & set "MINGW_PREFIX="
set "PATH=C:\Python314;C:\Python314\Scripts;C:\Users\Dakot\AppData\Roaming\Python\Python314\Scripts;C:\Windows\System32;C:\Windows;C:\Users\Dakot\AppData\Local\Programs\PortableGit\cmd"
cd /d C:\Users\Dakot\dev\homekit-ratgdo32
"C:\Users\Dakot\AppData\Roaming\Python\Python314\Scripts\pio.exe" run -e ratgdo_esp32dev
```

Invoke it from Bash with path-conversion disabled (git-bash mangles `/c` → `C:\`):
`MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' /c/Windows/System32/cmd.exe /c "C:\\...\\build.bat"`. **Never** `pwsh.exe`/`powershell.exe` (deny list). Note PortableGit's `cmd` (native git) must be on PATH or pio errors `Git not found`.

**`secplus` is a git submodule** (`lib/secplus`, from `argilo/secplus`). A fresh clone leaves it empty → `fatal error: secplus.h: No such file or directory`. Run `git submodule update --init --recursive` before the first build.

Note: this git-bash (Bash tool) is stripped — no `curl`/`ls`/`grep`/`tail` coreutils on PATH. Use the dedicated Read/Grep/Glob tools, or PortableGit's `usr/bin` + System32 `curl.exe` when a script needs them (e.g. `tools/verify-ota.sh`).

## Pi log access

ratgdo syslog forwards to `dakot@100.121.96.114` (UDP 5140), files at `/var/log/ratgdo.log` + `.1` + `.2.gz`. World-readable. `ssh -i ~/.ssh/pi_key dakot@100.121.96.114 'cat /var/log/ratgdo.log'` for live tail.
