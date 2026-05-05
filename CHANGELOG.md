# Change Log

**v3.x.x firmware is for ratgdo32 and ratgdo32-disco boards only**

All notable changes to `homekit-ratgdo32` will be documented in this file. This project tries to adhere to [Semantic Versioning](http://semver.org/) and [Keep a Changelog](https://keepachangelog.com/).

---

## Fork releases (`v3.4.4-forceclose.N`)

This section documents changes specific to the `Haglerd/homekit-ratgdo32` fork. Upstream changes are listed in the `v3.x.x` section below; the fork tracks upstream and adds these on top.

### v3.4.4-forceclose.32 (2026-05-05)

Audit follow-up cleanup of v31 — concurrency hygiene the v31 work surfaced after deployment, plus two memory-headroom items and a sweep of the version-prefix comment debt that accumulated v22-v31. **No new features, no behavior changes for users.** All atomic ordering pairs verified, all call sites swapped, all headers in sync. Code review: GREEN, ship it.

**Fixed (concurrency)**

- **Audit findings A + G — `clear_force_close_state` deferred to loopTask.** v31 deferred the gap-timer ARM but `clear_force_close_state` was still called directly from esp_timer task at four TTC-timer-callback sites in `send_force_close_release_then_maybe_retry` / `send_force_close_press`. That meant `Ticker.detach()` could still race on the gap timer between loopTask's `force_close_drain_pending_arm` and esp_timer's `clear_force_close_state` — same physical-world door-reversal failure mode v31 was meant to close, just relocated. New `request_force_close_clear(reason)` setter for esp_timer-context callers + `force_close_drain_pending_clear()` drained on loopTask via `service_timer_loop`. `clear_force_close_state` now loopTask-only. Reason is a string-literal pointer (single 32-bit pointer write — atomic on Xtensa).
- **Audit finding B — acquire-load on `forceCloseInProgress` in drain.** v31's `__atomic_test_and_set` writer was paired with a plain volatile load in `force_close_drain_pending_arm`, breaking the release/acquire chain. Both drains and `clear_force_close_state` now use `__atomic_load_n(&forceCloseInProgress, __ATOMIC_ACQUIRE)`.
- **Audit finding C — release/acquire on HomeKit deferred-flag setters/drains.** Setters (`homekit_request_reconnect`, `_request_refresh_mdns`, `_request_dump_state`) now write `reason` first, then `__atomic_store_n(&flag, true, __ATOMIC_RELEASE)`. Drains use `__atomic_load_n(&flag, __ATOMIC_ACQUIRE)` then read reason — guarantees the reason is never stale relative to the observed flag.
- **Audit finding D — atomic counter writers.** v31 made the readers atomic via `__atomic_exchange_n` but writers were still plain RMW. Writer for `logMtxMaxWaitMs` is now a CAS loop atomic-max via `__atomic_compare_exchange_n` (RELAXED ordering — diagnostic counter, no ordering needed). All three `sseOrphansReaped` writers in `sweep_sse_orphans` now use `__atomic_fetch_add(..., 1, __ATOMIC_RELAXED)`.

**Memory hygiene**

- **MH3 — Lazy-allocate `WiFiUDP syslog`.** Was 200 B BSS always. Now `static WiFiUDP *syslog = nullptr` + lazy `new (std::nothrow)` on first `logToSyslog` call when `syslogEn=true && WiFi.isConnected()`. Default install (syslogEn=false) saves ~196 B BSS forever. Once enabled, allocates 200 B heap.
- **MH4 — `enforce_same_origin` shared host buffer.** Per-call `originHost[64]` + `refererHost[64]` consolidated to one `extractedHost[64]` — Origin and Referer are checked sequentially and `extractHostFromUrl` zeros the buffer on entry. Saves 64 B stack/POST. Cross-origin rejection log still uniquely identifies the attempt (Origin + Referer + Host headers retained).

**Cleanup (no functional change)**

- **Comment debt sweep**: 151 → 87 fork-versioned comment leaders (`// v22:` … `// v31.2:`). Deleted multi-paragraph evolution blocks targeted by the audit: `enforce_same_origin` 40-line block, `sweep_sse_orphans` 23-line skew-detection block, `handle_subscribe` 7-line v27 ORDER NOTE + 20-line v28 INADDR_NONE block, force-close 17-line v31 deferred-arm block, HomeKit deferred-flag 16-line v24→v31 block, `comms.cpp` v23 deferred-reschedule + v31.2 cache-extension blocks, `ratgdo.cpp` drain-comment narration. Historical context belongs in commit messages and `audit-notes/`, not source.
- **ESP_LOG version-prefix strip**: `"v27: heartbeat=0 coerced to %u..."` → `"heartbeat=0 coerced to %u for %s"` (web.cpp:2285). `"v27: unsubscribe beacon..."` → `"unsubscribe beacon for UUID %s on channel %u"` (web.cpp:2447). Implementation-detail parentheticals trimmed: `(via main-loop drain)`, `(deferred to main-loop arm)`, `expect '(re)connected to AP' shortly`.
- **ESP_LOGI → ESP_LOGD demotes** for routine state (force-close release-sent + press hold; auto-close scheduler narration in 5 sites + boot/refresh/bootstrap; HK watchdog config refresh; controller list change; WiFi reconnected; mDNS refresh complete; HomeSpan state dump; HomeKit reconnect WiFi cycling). KEPT INFO on entry/completion lines: `FORCE CLOSE: starting`, `skipping second press`, `2-attempt sequence complete`, plus the 3 HomeKit health/diag-sse/diag-hk lines and all WARN-level orphan reaps + CSRF rejections.

**Out of scope (intentionally deferred)**: MH1 (ping-pong status_json, risky), MH2 PSTR-wrap (ESP8266-specific, schedule for upstream PR), MH5 build flags (-flto needs build verification), MH6 buffer retune (needs measurement first).

### v3.4.4-forceclose.31 (2026-05-05)

External-audit-driven cleanup of fork-introduced concurrency, security, and portability debt accumulated across v22-v30. All ten audit findings + three follow-up review items addressed. ESP32 build verified clean against the existing test bench; behavior changes are conservative (no new features, only correctness/observability/portability hardening).

**Fixed (concurrency / runtime correctness)**

- **SSE orphan sweep silent-disable regression introduced by v30.** v30's `int32_t` cast on `(now - timestamp)` fixed the TOCTOU race against writers but flipped any slot legitimately aged past 2³¹ ms (~24.85 days) to a negative comparison → `negative > timeout` is false → slot never reaped (sweep silently disables itself). v31 replaces the cast with explicit skew detection: `int32_t skew = (int32_t)(stamp - now); if (skew > 0) continue;` then unsigned `age = now - stamp` for the timeout test. Race fix preserved, long-uptime regression eliminated, mod-2³² wrap still handled correctly.
- **Force-close shared state race (door-reversal risk).** `forceCloseInProgress`/`forceCloseAttempt`/`forceCloseGapTimer` were mutated from both the comms loop (loopTask, `update_door_state(CURR_CLOSING)` → `clear_force_close_state`) and the TTCtimer Ticker callback chain (release → retry/cleanup) without synchronization. Observed race: Ticker decided "schedule attempt 2" → loopTask preempted and cleared force-close state → Ticker resumed and re-armed the gap timer → 1.5 s later attempt 2 fired on a door already physically closing → wall-button-press toggled it back toward Open. Fix: deferred-arm pattern — Ticker sets `forceCloseGapPendingArmMs`, new `force_close_drain_pending_arm` runs on loopTask under `service_timer_loop`, re-checks `forceCloseInProgress` before arming. Plus `__atomic_test_and_set` on the busy flag itself replaces the v22 check-then-set, eliminating the dual-core race where two cores could both pass the check before either set true (doubled-relay-press scenario).
- **`handle_status` torn-buffer race (Homebridge-visible).** v24's mutex-release-before-`server.send_P` mitigated broadcast-stall but introduced a TOCTOU: web_loop on the loopTask was free to take the mutex and overwrite `status_json` mid-send, so Homebridge intermittently saw torn JSON (half-old, half-new) → schema-rejected accessory updates. Fix: snapshot `status_json` into a dedicated `status_json_send` buffer (BSS on ESP32, mutex-held-across-send on ESP8266 to save heap) before releasing the mutex. Both invariants preserved.
- **`handle_status` ESP8266 portability gate.** Above fix is `#ifndef ESP8266` — ESP8266's heap can't afford another 2 KB BSS, and its SSE path can't sustain enough concurrent subscribers to expose the deadlock surface that motivated v24's mutex release. ESP8266 holds the mutex across the send, with a defense-in-depth `json[BUFSZ-1] = '\0'` against `build_status_json` regressions.
- **Auto-close Ticker self-reschedule anti-pattern.** `update_auto_close_schedule`'s outside-window branch armed itself as the one-shot's callback, repeating the FreeRTOS self-detach pattern that crashed `SSEheartbeat` in v22 (uxListRemove panic). Routed via `request_auto_close_reschedule` → main-loop drain.
- **`userSettings::get` data-race mitigation (Ticker-context auto-close reads).** `userSettings::get` is mutex-free upstream while `set` takes the mutex (variant-tear race when read concurrent with write). The fork's new `checkAutoClose` Ticker reader and `autoCloseInWindow` made this reachable in practice. Mitigation: `comms_refresh_auto_close_config` caches all 5 auto-close keys into volatile statics, refreshed at boot + on `/setgdo` save. All Ticker-context reads now hit the cache. Upstream issue draft kept in `audit-notes/` for the underlying class fix.
- **HomeKit watchdog sustained-recovery gate.** `hkRecoverAttempts` reset on a single sub-60 s read in any 180 s window, so a flapping iOS hub delivering one sporadic read could re-arm the watchdog forever (hours of repeated WiFi cycles, no escalation cap). Tightened to require `HK_HEALTHY_TICKS_TO_RESET = 3` consecutive ticks below `hkQuietSecs` before clearing — at 180 s cadence that's ≥9 min sustained healthy reads, well past typical hub flap interval.
- **HomeSpan-from-web-task entry points deferred.** New fork endpoints `/refreshHomeKitMDNS` and `/dumpHomeKitState` called `homeSpan.updateDatabase`/`processSerialCommand` from the WebServer task — outside HomeSpan's documented autoPoll-task safe zone. Routed through new request-flag + drain pattern matching v24's reconnect deferral. Same applied to the watchdog auto-recover's `homekit_refresh_mdns` call (was running from esp_timer task). Upstream's pre-existing `helperFactoryReset` and OTA `vTaskDelete(getAutoPollTask())` deliberately left alone (audit issue C, upstream-side).
- **Per-task log recursion guard.** v24's single-slot `static volatile TaskHandle_t inFnTask` was wiped when a second concurrent task entered, then a re-entry by the first task could recurse through `clientWriteEx`'s slow-write `ESP_LOGW` — exactly the load v24 was deployed to instrument. Replaced with an 8-slot CAS-protected table (`__atomic_compare_exchange_n` for slot acquisition, `__atomic_store_n` release on exit). Per-task isolation; table-exhaustion silently drops broadcast/syslog rather than risk recursion.
- **`reconnectHKReason` torn-write race.** `strncpy` from web handler concurrent with main-loop drain produced occasional garbled log strings. Replaced `char[64]` with `volatile HomekitDeferredReason` enum (single-byte, atomic on Xtensa). Two-writer last-wins semantic preserved; torn writes eliminated. Same enum pattern reused for the new mdns/dump-state defer pairs.
- **`logMtxMaxWaitMs` lost-sample race.** Read-then-zero from `homekit_health_log` was non-atomic; logger task could write the max between the read and the zero. Replaced with `__atomic_exchange_n(&logMtxMaxWaitMs, 0, __ATOMIC_RELAXED)` — single Xtensa instruction. Same pattern applied to `sseOrphansReaped`.

**Fixed (security)**

- **`enforce_same_origin` rewritten — two real bypasses closed.** Pre-v31:
  1. **Substring match.** `origin.indexOf(hostOnly) >= 0` passed any URL whose path/query merely contained the device's hostname as a substring. With `Host: ratgdo`, an attacker page at `http://evil.example/?ratgdo` cleared the check.
  2. **Missing-headers passthrough.** When both `Origin` and `Referer` were absent, the guard returned true. Default-no-password installs reach state-changing endpoints (incl. `/setgdo` `forceClose`) with this guard as the only CSRF defense, so a `<form>` POST with `Referrer-Policy: no-referrer` bypassed it from any same-LAN page.
  
  v31 parses `Origin`/`Referer` URLs with a real host extractor, compares lowercased + port-stripped host fields exactly, and treats absence of both `Origin` AND `Referer` as a hard fail. Bracket-aware IPv6 parsing (`[::1]:8080` → `[::1]`), RFC 6874 zone-ID stripping (`[fe80::1%eth0]` → `[fe80::1]`), degenerate-input fail-close (`[]` → reject). Companion zero-heap rewrite: stack-local `char[]` buffers throughout, no Arduino String allocations — eliminates the 6 `String` allocs per state-changing POST that were the dominant per-request heap-fragmentation surface on ESP8266.

**Added (observability)**

- **HomeKit health log split into 3 lines** (under 256-byte LINE_BUFFER_SIZE — the original combined line was already truncating mid-token at `sseOrphansReaped` pre-v31). New schema:
  - `HomeKit health: wifi=…  rssi=…  heap=…  maxBlock=…  uptime=…  paired=…  controllers=…  last_hap_read_ago=…`
  - `HomeKit diag-sse: logMtxMaxWait=…  sseSlowWrites=…  sseBufferFullSkips=…  sseSlotsAlloc=…  sseOrphansReaped=…`
  - `HomeKit diag-hk:  recoverAttempts=…  hintLevel=…  hkHealthyTicks=…  loopHWM=…B  tmrHWM=…B  apHWM=…B  tickDrift=…ms`
- **Watchdog state visibility:** `recoverAttempts`/`hintLevel`/`hkHealthyTicks` on every health line — confirms tuning is right without grepping for WARN-level auto-recover lines.
- **Per-task stack high-water marks (bytes):** `loopHWM`/`tmrHWM`/`apHWM` for loopTask, FreeRTOS Tmr Svc, and HomeSpan autoPoll — climbing toward zero indicates near-overflow. ESP-IDF wrapper returns bytes natively (no `* sizeof(StackType_t)` multiplier needed).

**Changed (build / portability)**

- **Advisory compiler warnings:** `-Wsign-compare`, `-Wsign-conversion`, `-Wshadow` added to `[env:ratgdo_esp32dev]` build_flags (intentionally NOT errors — library deps emit some that we can't fix in the fork). Would have caught the v30 `int32_t` cast bug at compile time.
- **`web_loop` SSE fanout buffer stack→BSS.** `localJson[2560]` per-loop stack alloc → `static`. ESP32: 31% of loopTask stack reclaimed; ESP8266 cherry-pick critical (50% of cont-task's ~4 KB).
- **`audit-notes/` gitignored** — local fork-attribution scratch space, not for upstream.

**ESP8266 portability notes (for cherry-pick to `Haglerd/homekit-ratgdo`)**

The skew-detection (`#0`), atomic counter ops (`#9`/`#10`/F1), per-task recursion guard (F3), and same-origin URL parsing (`#2b`) are all fully portable — stdint types, GCC built-ins, and standard C string ops only. The HomeKit watchdog/health-log block, the HomeSpan defer pairs, and the new stack-HWM observability are ESP32-only (depend on HomeSpan, FreeRTOS task handles, and esp_heap_caps). The `handle_status` ESP8266 branch (mutex-held-across-send + defense-in-depth terminator) is the recommended port; the ESP32 dedicated-buffer variant is too heap-expensive for ESP8266.

### v3.4.4-forceclose.30 (2026-05-04)

**Fixed (critical)**
- **SSE orphan sweep was reaping healthy slots due to a uint32 underflow.** Symptom in the wild on v29: `SSE orphan (idle) channel=0 ... idle=4294967294ms — reaping`, repeated continuously seconds after boot. Cause: TOCTOU race between `sweep_sse_orphans` capturing `now` once at the top of the loop and writers (Ticker callbacks for `SSEheartbeat`, `BUFFER_FULL` stamps from `SSEBroadcastState`) updating `subscribedAt` / `lastActivity` *after* the snapshot. When a writer's timestamp was slightly newer than our `now`, `(uint32_t)(now - timestamp)` wrapped to ~4.29 billion ms (`UINT32_MAX - delta`) — a value vastly larger than `SSE_IDLE_TIMEOUT_MS` (300000) or `SSE_PREHANDSHAKE_TIMEOUT_MS` (5000), so a healthy slot was reaped one tick after handshake on a busy device. Fix: cast both age computations to `int32_t` so a future timestamp produces a *negative* age, which fails the `> timeout` comparison and leaves the slot alive — the next sweep tick sees the up-to-date state and applies the real check. Also updates the comment on the `now = (uint32_t)_millis()` line to document the new semantic. The 49.7-day uint32 wraparound itself is still handled correctly by mod-2^32 arithmetic — only the sub-second TOCTOU window changes behaviour.

**ESP8266 portability notes (for cherry-picking to `Haglerd/homekit-ratgdo`)**
- The v30 fix is in `web.cpp::sweep_sse_orphans` and is fully portable — no ESP32-specific APIs. Cherry-pick the `int32_t preAge` / `idleAge` cast lines and the matching `> (int32_t)<TIMEOUT>` comparison change. The bug exists wherever a uint32 timestamp is subtracted from `now` without a signed cast.
- Reviewed the v22-v29 fork delta against the ESP8266 codebase: SSE infrastructure (sweep, drain, unsubscribe, `enforce_same_origin`, `clientWriteEx` tri-state return, BUFFER_FULL flow-control diagnostics) is portable. The HomeKit watchdog + 180-s health log block (`homekit.cpp::homekit_health_log` and the `pairedControllersCount` cache + `hap_controller_change_cb` HomeSpan hook) is ESP32-only — it depends on HomeSpan's `setControllerCallback` and `esp_heap_caps`. ESP8266 builds should skip that block; the watchdog config setters and ~30 B BSS for the cache aren't worth the heap on an ESP8266 anyway.

### v3.4.4-forceclose.29 (2026-05-04)

**Fixed**
- **SSE idle-reap was misclassifying TCP-flow-controlled slots as idle.** Real-world: a `logs.html` viewer over a Tailscale (WireGuard) tunnel had its slot reaped every 120 s in a continuous loop. The v28 Nit-5 fix correctly only stamped `lastActivity` on successful writes — but the `availableForWrite() < len` fast-path at `clientWrite` returned `false` without distinguishing "lwIP send buffer full" (flow control, peer is alive but slow) from "write actually failed." Both paths skipped the stamp; on a chronically-congested tunnel the slot's `lastActivity` never advanced and class-5c reaped a healthy slot. Fix: `clientWrite` returns `SseWriteResult` (OK / BUFFER_FULL / FAILED). Callers stamp `lastActivity` on OK or BUFFER_FULL — both mean "broadcast loop reached this slot and tried." Only FAILED (`client.write` returned 0 after lwIP accepted bytes for delivery) skips the stamp. The `printf` fallback paths in `SSEBroadcastState` keep their `pwrote > 0` gate — different layer, that IS the wedge signal we want exposed.
- 5b protection (`client.connected() == false`) is unchanged and is the actual safety net for "TCP socket dead, lwIP cached state stale." 5c was always belt-and-suspenders for that case. A truly wedged slot still gets reaped by 5b once lwIP KeepAlive fires (typically <60s).

**Changed**
- `SSE_IDLE_TIMEOUT_MS` raised 120000 → 300000 (2 min → 5 min). Defense in depth on top of the BUFFER_FULL stamping fix above. 5 min is past most NAT-binding refresh intervals; a genuinely-wedged slot still gets caught well before users notice stale data.

**Added**
- `sseBufferFullSkips` health-log counter. Counts `availableForWrite() < len` skips since boot. Lets us see at a glance whether a subscriber is chronically flow-controlled vs. occasionally — drives whether v30 needs per-slot adaptive timeouts.

### v3.4.4-forceclose.28 (2026-05-04)

**Fixed (critical)**
- **The actual root cause of "no free slots available" wedge.** The SSE-subscribe free-slot scan at `web.cpp:2003` was `if (!subscription[channel].clientIP)`, which (via `IPAddress::operator bool()`) is true ONLY when the address is 0.0.0.0 — NOT when it's `INADDR_NONE` (0xFFFFFFFF). But `setup_web` and `removeSSEsubscription` mark slots free by setting `clientIP = INADDR_NONE`. The orphan sweep at line 1642 correctly compares `clientIP == IPAddress(INADDR_NONE)`; the subscribe scan disagreed. Net effect: the very first 8 subscribes worked (initial slots have dword=0 from struct init), but every slot subsequently freed via `removeSSEsubscription` was permanently invisible to the scan. After all 8 slots had been used and freed once, every new subscribe returned 503 "no free slots" — even though the sweep correctly reported `sseSlotsAlloc=0`. Pre-existing bug all the way back to v22; v22-v26 hit it identically but the v22 SSE deadlock crashed the device before slot 9 was attempted, masking the symptom. v27's deadlock fix exposed it. Scan now compares `== IPAddress(INADDR_NONE)` to match the canonical "free" marker; same fix applied to `handle_unsubscribe`'s slot-lookup.

**Fixed (audit findings on v27)**
- **`SSESubscription.subscribedAt` and `lastActivity` were `int64_t` tearing risk.** Changed to `volatile uint32_t` (truncated `_millis()` cast) — eliminates race between sweep (main loop) and writers in Ticker / SSEBroadcastState. Wrap-safe subtraction handles ~49.7-day rollover; intervals (15s/120s) fit comfortably in 32 bits.
- **`handle_unsubscribe` missing `enforce_same_origin`.** v27 comment claimed sendBeacon can't set custom headers — true for X-* headers, but browsers DO populate Origin/Referer/Host on sendBeacon POSTs (which is what `enforce_same_origin` actually checks). Added the guard; blocks drive-by cross-origin closes without breaking legitimate beacons.
- **`/reset` and `/reboot` missing same-origin guards.** v23 added `enforce_same_origin` to `/setgdo`, `/reconnectHomeKit`, `/refreshHomeKitMDNS`, `/dumpHomeKitState`. `/reset` (un-pair + reboot) and `/reboot` (full reboot) were missed — a cross-origin LAN page could trigger either with a single sendBeacon. Both now consistent. `/reboot` additionally gained `AUTHENTICATE()` (no-op when no www password set, enforces password otherwise — matches `/reset`).
- **Printf-path `lastActivity` stamps now gated on success.** `SSEBroadcastState`'s oversized-message printf fallback was stamping `lastActivity` regardless of write success, masking the orphan-sweep idle check (5c) by ~3min on a wedged subscriber. Captures `printf` return; only stamps when bytes were actually written.
- **Firmware-update SSE write now stamps `lastActivity` on success.** Was the one v27 SSE write site that didn't refresh the timestamp. Negligible in practice (updates run <30s) but matches the rest of the SSE write paths.

**Changed (storm robustness)**
- **`SSE_PREHANDSHAKE_TIMEOUT_MS` reduced from 15000 → 5000.** Real-world reconnect storm observed where browser EventSource auto-retry filled all 8 slots faster than the 15s sweep could drain them. 5s is well above the typical EventSource handshake (<500ms even on cellular) — slow legitimate clients just hit a fresh subscribe retry, but storms can't outpace the sweep.
- **`functions.js` (home page) gets the same UUID-localStorage + sendBeacon-on-unload + `heartbeat=10` treatment v27 applied to logs.js.** Home page reload no longer leaks a fresh slot per load.

**Changed (consistency)**
- **`isPaired` annotated `volatile`** for cross-context consistency with `hapLastReadSec` / `pairedControllersCount`. No behavior change — single-byte writes already atomic on ESP32; volatile just prevents compiler hoisting.



### v3.4.4-forceclose.27 (2026-05-02)

**Fixed**
- **SSE slot leak — device wedges 25s after every boot, `logs.html` unreachable.** Four interacting bugs:
  1. **`subscriptionCount` desync.** `handle_subscribe()` incremented the counter mid-validation. Subsequent rejection paths (heartbeat range, low heap, dead client) returned without decrementing, so the counter drifted up over time and falsely tripped the "no free slots" capacity check while real slots sat free. Fix: all validations moved before the slot/counter mutations; counter only bumps after every rejection path is exhausted.
  2. **No liveness driver for `heartbeat=0` clients.** `logs.html` subscribed with `heartbeat=0`, which produced a slot with no `Ticker` running. The only cleanup path (`SSEheartbeat()` failing 5x → `pendingRemove`) never fired, so `logs.html` slots leaked forever once the page navigated away. Fix: server-side coerce `heartbeat=0 → 30` and add an orphan sweep that runs from `service_timer_loop` independent of the Ticker.
  3. **No pre-handshake timeout.** A slot allocated by `handle_subscribe()` but whose `EventSource` never came back to `/events/N` (browser closed mid-flight, GET hung) had no timeout — it sat as `clientIP=set, SSEconnected=false` until the next reboot. Fix: orphan sweep reaps any pre-handshake slot older than 15s.
  4. **No idle timeout for connected slots.** A slot whose TCP socket dropped without an RST or whose subscriber stopped reading (`client.connected()` still true, no broadcast ever fails) was never cleaned up. Fix: orphan sweep reaps connected slots idle for >120s.
- **`controllers=0` cosmetic in health log.** HomeSpan does not invoke `setControllerCallback` for pairings loaded from NVS at boot — only for live add/remove events. The health log reported `controllers=0` from boot until the next live pairing change (often forever). Fix: explicitly call `hap_controller_change_cb()` once at the end of `setup_homekit` to seed the cached count.

**Added**
- `POST /rest/events/unsubscribe?id=UUID` — best-effort beacon endpoint. `logs.js` calls this via `navigator.sendBeacon()` on `beforeunload`, releasing the SSE slot immediately on page navigation instead of waiting for the orphan sweep timeout. No auth, no CSRF (sendBeacon can't set custom headers; worst case is closing your own session). Browsers don't guarantee delivery — the orphan sweep is still the authoritative cleanup path.
- Low-heap rejection at the top of `handle_subscribe()`. New SSE subscriptions are refused with 503 when free heap is below 16KB. Stops the cascade where heap pressure → write failures → leaked slots → more heap pressure.
- New SSE health-log fields: `sseSlotsAlloc` (live snapshot of allocated slots, refreshed every service tick) and `sseOrphansReaped` (count of slots reaped by the sweep this 180s window). `sseSlotsAlloc=8 + sseOrphansReaped=0 + new clients getting "no free slots"` = sweep is broken.

**Changed**
- `logs.js` now persists its SSE client UUID in `localStorage` (`ratgdo-logs-uuid`). Pre-v27 every page reload generated a fresh UUID, which combined with the orphan sweep's 15s pre-handshake timeout meant ~6 fast reloads could fill all 8 channels. Falls back to a per-session UUID if storage is blocked (iOS private browsing).
- `logs.js` heartbeat changed from `0` → `10`. Keeps `lastActivity` fresh so the connected client never trips the 120s idle reap, and gives `SSEheartbeat()` a Ticker for class-5b cleanup.

**Note: missing v23-v26 CHANGELOG entries.** Versions 23 through 26 shipped without a CHANGELOG entry at the time. Quick recap — v23 added the same-origin/CSRF guard for state-changing endpoints + the auto-close reschedule deferral + watchdog config defensive clamps; v24 added SSE write-side `SO_SNDTIMEO`, the `clientWrite` slow-write counter + `availableForWrite` fast-path, the `pairedControllersCount` cache, and the `homekit_drain_pending_reconnect` deferral; v25 fixed an `INADDR_NONE` overload ambiguity that surfaced after a header-include shuffle; v26 was a release-pipeline-only change. Source-of-truth is the git log on `main` for those tags.

### v3.4.4-forceclose.22 (2026-05-02)

**Fixed**
- **SSE self-detach crash in `loopTask`** — `SSEheartbeat()` called `removeSSEsubscription()` from inside the Ticker callback, which `Ticker.detach()`'d its own running heartbeat → `vTaskDelete` on the Ticker's task → `uxListRemove` panic. Long-running `logs.html` sessions would eventually hit this on any version. Fix: `SSESubscription.pendingRemove` flag set from inside the callback, drained from `service_timer_loop()` in main-loop context.
- Auto-close: ticker now scheduled window-aware. Outside the configured window, no recurring tick — a one-shot timer sleeps until window-start, then the 60s tick attaches. Re-armed on `/setgdo` settings save. Skip-paths are silent (no more `AUTO-CLOSE: tick #N — door not Open, skipping` every 60s).
- HomeKit watchdog config now cached at boot and on settings save instead of taking the `userConfig` mutex inside the Ticker callback every 60s.

**Changed**
- `/status.json` request log demoted `ESP_LOGI → ESP_LOGD`. The Homebridge plugin polls every 3s by default; the previous INFO line was ~20/min of pure noise. The 95%-buffer-full WARN stays at WARN (still actionable).
- HomeKit / WiFi / HomeSpan log lines are now **exclusive to the HomeKit tab** in `logs.html` — they no longer also clutter the System Log tab.
- Health log interval bumped 60s → 180s (purely diagnostic; doesn't need 1-min resolution).

### v3.4.4-forceclose.21 (2026-05-02)

**Added**
- **HomeKit watchdog settings UI** — Settings page now exposes a toggle (`hkAutoRecover`) and four threshold inputs (`hkAutoRecoverSecs`, `hkHintQuietSecs`, `hkHintStaleSecs`, `hkHintLikelyNRSecs`). Replaces the v19/v20 compile-time constants. Defaults preserve v19/v20 behaviour exactly: auto-recover OFF, 5/15/30-minute hint tiers, 30-minute trigger.

### v3.4.4-forceclose.20 (2026-05-02)

**Fixed**
- CI: replaced 5 separate `wow-actions/download-upload` Contents-API commits with a single `git checkout -fB main origin/main` + `git push`. Branch protection ("Block force pushes / Restrict deletions") was timing out the Contents-API rule eval, leaving `docs/firmware/` stale by one release on every build (v18 + v19 both shipped without bins committed). The single direct push works inside a 1s window.

**Changed**
- New `auto-release.yml` — fires on push to `main` that touches `docs/manifest.json`. Reads version, creates tag + release, dispatches `release.yml`. Net effect: bumping `docs/manifest.json` on a feature branch + merging the PR runs the entire release pipeline hands-free.

### v3.4.4-forceclose.19 (2026-05-02)

**Changed**
- HomeKit watchdog ships **disabled by default** with tiered diagnostic hints. Real-world iOS read cadence is highly variable (gaps of 6+ min observed during normal idle), so a low fixed threshold caused false-trigger recoveries on healthy connections. v19 keeps the threshold-checking logic and emits hint logs at 5/15/30 min, but no recovery action runs unless the user opts in.

### v3.4.4-forceclose.18 (2026-05-02)

**Added**
- **HomeKit self-healing watchdog** — periodic health check (every 60s) tracking `last_hap_read_ago`. When iOS goes silent past the trigger threshold AND WiFi is connected AND we have paired controllers, escalates: mDNS refresh first (cheap, no outage), then WiFi reconnect (~5s outage). Stops after 2 attempts; never auto-reboots.

### v3.4.4-forceclose.17 (2026-05-02)

**Fixed**
- `clear_force_close_state()` was firing on every `CURR_OPEN` / `CURR_CLOSED` / `CURR_STOPPED` status poll, wiping `forceCloseAttempt` mid-sequence. Symptom: "attempt 0 release sent" then full sequence ran twice. Removed the over-defensive terminal-state cleanup; kept only the `CURR_CLOSING` transition fix from .16.

**Added**
- HomeKit recovery buttons on home page (Refresh mDNS / Reconnect HomeKit / Dump State) in addition to the logs.html ones.

### v3.4.4-forceclose.16 (2026-05-02)

**Fixed**
- `forceCloseInProgress` flag leak — fork's force-close override could get stuck "in progress" if the firmware took an unexpected path during the 2-press sequence, blocking subsequent close commands until reboot. Added `clear_force_close_state()` triggered on `CURR_CLOSING` transition (the door reached the desired state — clear our in-progress marker).

**Added**
- HomeKit visibility (WiFi connect/disconnect events with reason codes, periodic 60s health log, three HomeSpan callbacks tracking `hapLastReadSec`).
- Recovery endpoints: `POST /reconnectHomeKit` (cycles WiFi, HomeSpan re-attaches), `POST /refreshHomeKitMDNS` (re-broadcast mDNS without dropping WiFi), `POST /dumpHomeKitState` (dump HomeSpan CLI status / accessory DB / diag to log).
- New "HomeKit" tab in `logs.html` with three recovery buttons + filtered HomeKit/WiFi/HomeSpan event view.

### v3.4.4-forceclose.15 (and earlier)

Initial fork releases adding the `forceClose` HTTP primitive (single POST → 2-press hold-to-close override at the Sec+1.0 protocol level), the firmware-side auto-close timer (with optional time-of-day window), security hardening (input validation, CSRF on `/setgdo`, busy-flag guard on force-close), and basic UI polish (widened time inputs, fork README header).

---

## Upstream releases (`v3.x.x`)

The following are upstream `ratgdo/homekit-ratgdo32` releases the fork tracks via daily auto-sync. Each fork release includes everything from upstream `v3.4.4` plus the fork-specific changes above.

## v3.4.4 (2026-02-??)

### What's Changed

* Bugfix: Inform browser whenever IP address is set or changed
* Bugfix: Fix setting keyname mismatch between browser and server (homekitLight / lightHomeKit)
* Feature: Allow user to set NTP server URL
* Other: ESP32 only, update HomeSpan library to version 2.1.7

### Known Issues

* ESP32 (ratgdo32) only... Some users may get an error during OTA upload that firmware is too large for the OTA partition. See [README.md](https://github.com/ratgdo/homekit-ratgdo32/blob/main/README.md#upgrade-failures) for work-around.
* Sec+ 1.0 doors with digital wall panel (e.g. 889LM) sometimes do not close after a time-to-close delay. Please watch your door to make sure it closes after TTC delay.
* Sec+ 1.0 doors with "0x37" digital wall panel (e.g. 398LM) not working.  We now detect but will not support them. Recommend replacing with 889LM panel.
* When creating automations in Apple Home the garage door may show only lock/unlock and not open/close as triggers. This is a bug in Apple Home. Workaround is to use the Eve App to create the automation, it will show both options.

## v3.4.3 (2026-01-11)

### What's Changed

* Bugfix: (Sec+2.0 only) door not closing if ratgdo thinks it is still opening (rightly or wrongly). https://github.com/ratgdo/homekit-ratgdo32/issues/131
* Bugfix: Escape backslash and double quotes inside JSON strings.  https://github.com/ratgdo/homekit-ratgdo32/issues/134
* Bugfix/feature: (Sec+2.0 only) allow user to select sending TOGGLE command instead of CLOSE. https://github.com/ratgdo/homekit-ratgdo32/issues/131
* Feature: Hardwired Sec+ GPIO Controls Mirror Wall Panel, Optional TTC Bypass. https://github.com/ratgdo/homekit-ratgdo32/pull/136
* Feature: Publish ratgdo and door status over mDNS
* Other: Update settings page visuals to disable/enable options rather than hide/show.

### Known Issues

* ESP32 (ratgdo32) only... Some users may get an error during OTA upload that firmware is too large for the OTA partition. See [README.md](https://github.com/ratgdo/homekit-ratgdo32/blob/main/README.md#upgrade-failures) for work-around.
* Sec+ 1.0 doors with digital wall panel (e.g. 889LM) sometimes do not close after a time-to-close delay. Please watch your door to make sure it closes after TTC delay.
* Sec+ 1.0 doors with "0x37" digital wall panel (e.g. 398LM) not working.  We now detect but will not support them. Recommend replacing with 889LM panel.
* When creating automations in Apple Home the garage door may show only lock/unlock and not open/close as triggers. This is a bug in Apple Home. Workaround is to use the Eve App to create the automation, it will show both options.

## v3.4.2 (2025-12-13)

### What's Changed

* Feature: Query the state of emergency back up battery on boot and every 55 minutes (Sec+2.0 only).
* Feature: User can select whether to create HomeKit accessories for motion sensor and light switch (ratgdo32 only). With thanks to https://github.com/DaveLinger
* Bugfix: If firmware upload error detected before update begins, do not require a reboot.
* Other: Average vehicle distance over larger sample size (now 50) to smooth out spurious readings (ratgdo32-disco only).

## v3.4.1 (2025-11-22)

### What's Changed

* Bugfix: Vehicle departing motion sensor may not trigger correctly (ratgdo32-disco only)
* Bugfix: re-Announce ratgdo mDNS every two minutes, so that we remain visible on network (default TTL is 2 minutes)
* Bugfix: The home icon at top/right of the system logs page was not always returning to ratgdo main page https://github.com/ratgdo/homekit-ratgdo/issues/318
* Feature: Add a [webmanifest](https://developer.mozilla.org/en-US/docs/Web/Progressive_web_apps/Manifest) file and update all browser favorite icons for better visuals
* Feature: Add support for Captive Network Assistant (CNA) so that Apple and Android devices will automatically load WiFi provisioning page when connecting to ratgdo Soft Access Point (Soft AP mode)
* Feature: Add a warning and countdown timer to web page when Sec+2.0 doors have automatic door close (TTC) active
* Other: Display "Off" instead of "0" when settings sliders are set to zero seconds/minutes
* Other: Improved web page design for iPhone and iPad devices
* Other: Attempt to recover from out-of-sync Sec+2.0 rolling code https://github.com/ratgdo/homekit-ratgdo/issues/315
* Other: HomeSpan library updated to version 2.1.6 (ratgdo32 only)
* Other: Various log message cleanup to make debugging easier and reduce log clutter at default Info level

## v3.4.0 (2025-11-01)

### What's Changed

* Bugfix: User selected syslog facility not restored on startup. https://github.com/ratgdo/homekit-ratgdo32/issues/116
* Bugfix: Crash when HomeKit tries to open or close a dry contact door. https://github.com/ratgdo/homekit-ratgdo32/issues/117
* Bugfix: Sec+2.0 only, not handling packet transmit errors during initialization
* Feature: Sec+2.0 only, support garage door automatic close after selected delay, [SEE README](https://github.com/ratgdo/homekit-ratgdo32/blob/main/README.md#automatic-close)
* Feature: Add time-to-close countdown timer to web page
* Other: Allow user to disable triggering motion from Sec+2.0 wall panel motion sensors
* Other: Ratgdo-disco only, update vehicle presence algorithm for no vehicle present to match ESPhome
* Other: Adjust some Info-level log messages to Debug- or Error-level... reduces log clutter at default Info level

## v3.3.9 (2025-10-24)

### What's Changed

* Bugfix: Update function that calculates median door open/close duration. https://github.com/ratgdo/homekit-ratgdo/issues/309
* Bugfix: Do not cancel time-to-close if second door close request received. https://github.com/ratgdo/homekit-ratgdo32/issues/112
* Other: Ratgdo-disco only, update vehicle presence algorithm to match ESPhome https://github.com/ratgdo/esphome-ratgdo/pull/496
* Other: Additional Serial CLI commands for development and debugging to e.g, provision WiFi SSID and password.

## v3.3.8 (2025-10-19)

### What's Changed

* Bugfix: dry contact doors not reporting status correctly on web page. https://github.com/ratgdo/homekit-ratgdo32/issues/109
* Bugfix: Sec+1.0 add timeout when waiting for GDO reply to poll commands https://github.com/ratgdo/homekit-ratgdo32/issues/111
* Other: Save door open/close durations so not reset to unknown on a reboot
* Other: Add timers to check that door starts to open/close and reaches fully opened/closed state in expected time
* Other: Sec+2.0 use MotorOn packet to error correct if we miss notification packet of door opening or closing
* Other: Add serial CLI commands to scan WiFi networks and reset door ID & rolling codes
* Other: Remove known issues list from prior versions in CHANGELOG.md... because they are now repeating

## v3.3.7 (2025-10-12)

### What's Changed

* Bugfix: Sec+2.0 doors not opening or closing. Issue https://github.com/ratgdo/homekit-ratgdo/issues/305

## v3.3.6 (2025-10-12)

### What's Changed

* Bugfix: HomeKit pairing failed with out-of-compliance error. https://github.com/ratgdo/homekit-ratgdo/issues/300
* Bugfix: Web wage status occasionally getting out-of-sync with actual light/lock/door state.
* Bugfix: Door open/close duration calculation not handling cases where door reverses before reaching open/close state.
* Bugfix: Setting syslog port not taking effect until after reboot. https://github.com/ratgdo/homekit-ratgdo/issues/304
* Bugfix: do not attempt to act on Sec+2.0 packet that failed to decode. Issue https://github.com/ratgdo/homekit-ratgdo32/issues/106
* Feature: Change WiFi and MDNS hostname when user changes GDO name. https://github.com/ratgdo/homekit-ratgdo32/issues/93
* Feature: Add clipboard copy icon to IP address and mDNS name.
* Other: Detect (but do not support) 0x37 wall panels like LiftMaster 398LM. https://github.com/ratgdo/homekit-ratgdo32/issues/95
* Other: ESP8266 (original ratgdo) only... suspend GDO communications during HomeKit pairing process.
* Other: ESP8266 (original ratgdo) only... move more constants into PROGMEM and optimize use of system stack.

## v3.3.5 (2025-09-28)

### What's Changed

* Bugfix: Add error handling for a blank SSID... force boot into Soft AP mode, https://github.com/ratgdo/homekit-ratgdo/issues/295
* Bugfix: Buffer overrun that caused Improv setup to fail, https://github.com/ratgdo/homekit-ratgdo/issues/298
* Bugfix: Aog messages that are truncated for exceeding buffer size not null terminated
* Other: Add simple serial console CLI (when HomeSpan CLI disabled) to allow setting debug level, displaying saved logs and request reboot.

## v3.3.4 (2025-09-27)

### What's Changed

* Bugfix: Activity LED blink was not obeying user preferences (e.g. always off). https://github.com/ratgdo/homekit-ratgdo/issues/292
* Bugfix: Ignore implausibly long door opening / closing times. https://github.com/ratgdo/homekit-ratgdo32/issues/98
* Bugfix: Room occupancy state not set on motion (ratgdo32 only). https://github.com/ratgdo/homekit-ratgdo32/issues/96
* Feature: Allow user selection of Syslog facility number (Local0 .. Local7). https://github.com/ratgdo/homekit-ratgdo32/issues/94
* Feature: Add MDNS service for _http._tcp to allow local network name discovery. https://github.com/ratgdo/homekit-ratgdo32/issues/93
* Other: Disable HomeKit and garage door communications during OTA firmware update.
* Other: Miscellaneous stability improvements.

## v3.3.3 (2025-09-20)

### What's Changed

* Bugfix... Date and time on web page now displayed in the time zone of the server (NTP server feature must be enabled).
* Bugfix... Log level was not getting set correctly (should take effect immediately but it required a reboot).
* Bugfix... Park assist laser was not activating on vehicle arrival (since v3.3.0).
* Bugfix... Displaying last reboot log was missing time stamps.
* New Feature... last door open and close date and time is displayed under opening/closing status (NTP server feature must be enabled).
* Other... Add a "home" button to system logs page because iOS and iPad OS 26 have removed the "done" button.

## v3.3.2 (2025-09-14)

### What's Changed

* Other... Add release notes to firmware update dialog.
* Other... Reduce unnecessary network traffic during firmware update

## v3.3.1 (2025-09-13)

### What's Changed

* Bugfix... Updating from GitHub was not finding the firmware download file.

## v3.3.0 (2025-09-13)

Version 3.3.0 is a significant upgrade for ESP32-based ratgdo boards. Almost all source files for the ESP8266 and ESP32 versions of ratgdo have been merged which results in minor changes to the underlying features and function for ESP32 versions. The main benefit is for the original ESP8266-based ratgdo boards.

While source files have been merged there remain significant differences between the two board types, most notably in the library used to communicate with HomeKit which are completely different.

* Before an Over-The-Air (OTA) upgrade it is good to first reboot your current version.

### What's Changed

* Other... Significant source code changes to support move towards single code base for ESP8266 and ESP32
* Other... Change garage door communications from using GDOLIB library to same code as used for ESP8266

## v3.2.1 (2025-07-??)

### What's Changed

* Bugfix: IPv6 support should now be working (updated HomeSpan to release 2.1.3).
* Bugfix: Boot safely when no GDO connected (updates GDOLIB to release 1.2.1).

## v3.2.0 (2025-07-04)

### What's Changed

* Bugfix: Doors without obstruction sensor may not respond to door action CLOSE commands, use TOGGLE instead (Issue #81).

## v3.1.12 (2025-06-15)

### What's Changed

* Bugfix: Sec+ 1.0 doors may not close at end of time-to-close delay (Issue #77).

## v3.1.11 (2025-06-13)

### What's Changed

* Feature: Make IPv6 optional, disabled by default.

## v3.1.10 (2025-06-08)

### What's Changed

* Feature: Enable IPv6 (PR #74) with thanks to https://github.com/apearson

## v3.1.9 (2025-05-11)

### What's Changed

* Bugfix: Hostname not set correctly to comply with RFC952 (Issue #72)
* Bugfix: Door close duration not reported correctly on web page
* Other: Add console CLI command to allow setting debug message log level
* Other: Add console CLI command to facilitate testing moving door to position between 0% and 100%
* Other: Update to use version 2.1.2 of upstream HomeSpan library
  
## v3.1.8 (2025-04-27)

### What's Changed

* Bugfix: show motion status and controls when any motion trigger type is set
* Bugfix: room occupancy and motion reset time not working as expected

### Known Issues

* Still testing... Future updates MAY include breaking changes requiring a flash erase and re-upload.

## v3.1.7 (2025-04-27)

### What's Changed

* Feature: Allow user setting to disable light flashing during time-to-close
* Feature: Add a HomeKit occupancy sensor triggered by motion that is active for user specified duration between 0 and 120 minutes, Requested in issue #45
* Bugfix: Do not run setup function(s) if already run.  Hope that this fixes issue #51
* Bugfix: Obstruction sensor not triggering HomeKit motion, fix issue #65
* Other: Additional error checking and changes to log verbosity for some messages.

### Known Issues

* Still testing... Future updates MAY include breaking changes requiring a flash erase and re-upload.

## v3.1.6 (2025-04-01)

Version 3.1.x include major change to garage door opener (GDO) communications.  This has major benefit in separating out the details of communicating with the garage door from HomeKit and the ratgdo user interface, greatly simplifying our code.

### What's Changed

* Feature: Allow user setting for dry contact debounce timer from 50ms to 1000ms
* Bugfix: HomeKit door status not always correct, reported in issue #57
* Bugfix: Fis time-to-close light flashing to work as intended (flash every 1/2 second)

## v3.1.5 (2025-03-31)

Version 3.1.x include major change to garage door opener (GDO) communications.  This has major benefit in separating out the details of communicating with the garage door from HomeKit and the ratgdo user interface, greatly simplifying our code.

### What's Changed

* Bugfix: Dry contact wall panel button not working, fixes issue #57

### Known Issues

* Still testing... Future updates MAY include breaking changes requiring a flash erase and re-upload.

## v3.1.4 (2025-03-30)

Version 3.1.x include major change to garage door opener (GDO) communications.  This has major benefit in separating out the details of communicating with the garage door from HomeKit and the ratgdo user interface, greatly simplifying our code.

### What's Changed

* Feature: Allow selection between software serial port emulation and hardware UART.  May help with issue #48
* Feature: Add debug terminal command to print out buffered message log
* Bugfix: Default to always use ratgdo's own timer for door time-to-close (TTC) and not the TTC built-in to garage door opener.  Partially fixes issue #50
* Bugfix: Remove user interface setting for built-in time-to-close, also addresses issue #50
* Bugfix: Garage door opening/closing status not reported correctly to HomeKit, fixes issue #53
* Buffix: Sec+ 1.0 garage door was unexpectedly opening on reboot when switching between hardware UART and s/w serial
* Other: Add additional debug and error checking to assist with fixing issue #51

### Known Issues

* Still testing... Future updates MAY include breaking changes requiring a flash erase and re-upload.

## v3.1.0 - v3.1.3 (2025-03-07)

Version 3.1.x include major change to garage door opener (GDO) communications.  This has major benefit in separating out the details of communicating with the garage door from HomeKit and the ratgdo user interface, greatly simplifying our code.

As this is a major change, thorough testing is required.

### What's Changed

* Feature: Updated [HomeSpan](https://github.com/HomeSpan/HomeSpan) to version 2.1.1
* Feature: Use new [library](https://github.com/dkerr64/gdolib) for garage door communications
* Feature: For Sec+2.0 use GDO's built-in Time-to-Close (TTC).  Option on user interface to disable and use ratgdo timer.
* Feature: Add user option to disable HomeKit motion and occupancy accessories for vehicle presence.
* Feature: Add door opening and closing duration to web page (calculated first time door operated after reboot).
* Feature: Added message log level to user interface, defaults to INFO.
* Bugfix: Dry contact should be working now, fixes issue #41
* Other: Replace all our RINFO()/RERROR() message log macros with standard ESP_LOGx() macros.

Note: The built-in TTC feature operates slightly differently than ratgdo's timer.  If you set the TTC to e.g. 20 seconds, then on requesting a door close it will wait, in silence, for 20 seconds then sound its internal beeper and flash the lights for 10 seconds before closing the door... thus 30 seconds before the door closes.  The TTC timer in ratgdo will immediately sound its buzzer and flash lights, and close the door after 20 seconds.

### Known Issues

* Still testing... Future updates MAY include breaking changes requiring a flash erase and re-upload.

## v3.0.7 (2025-02-07)

### What's Changed

* Feature: Report door open/close cycle count and emergency backup battery charging/full status to web page (Sec+2.0 only).
* Bugfix: Problem with time-to-close and Sec+1.0 on doors with digital wall panel, issue #35
* Other: Support logging of ESP_LOGx() messages through our logger facility.

### Known Issues

* Still testing... Future updates MAY include breaking changes requiring a flash erase and re-upload.

## v3.0.6 (2025-01-17)

### What's Changed

* Bugfix: Crash in esp_timer.c possibly related to multi-thread use of Ticker for LED timer (Issue #30).
* Bugfix: Motion sensor status should not show if there is no motion sensor.
* Bugfix: Crash reported at end of time-to-close delay (Issue #32).
* Bugfix: Could not set vehicle threshold above 200cm
* Feature: Set default time-to-close delay to 5 seconds and add warning if user selects lower value (Issue #33).
* Feature: Dynamically create HomeKit QR setup ID, and QR code graphic for pairing.

### Known Issues

* Still testing... Future updates MAY include breaking changes requiring a flash erase and re-upload.

## v3.0.5 (2025-01-05)

### What's Changed

* Feature: Add user setting to enable/disable parking assist laser, and set duration of assist laser.
* Feature: Added support to save and view message log on crash (Issue #2).
* Bugfix: Remove multiple copies of web page content from the firmware binary.
* Bugfix: Use 64-bit integer to handle milliseconds since last boot.
* Bugfix: Handle more possible return codes from vehicle distance sensor.
* Bugfix: Door status incorrectly reported to HomeKit if close requested for already closed door (Issue #28).
* Bugfix: Last door change date/time was not always been set correctly after reboot.
* Bugfix: Activity LED options not correctly shown in web page.
* Bugfix: Activity LED constantly on with Sec+ 1.0 protocol.
* Bugfix: Remove wait for incoming serial packet before starting to detect Sec+ 1.0 digital wall panel.
* Updated copyright statement(s) to include year 2025.

### Known Issues

* Still testing... Future updates MAY include breaking changes requiring a flash erase and re-upload.

## v3.0.4 (2024-12-21)

### What's Changed

* Feature: Add HomeKit light switch and web page button for parking assist laser
* Feature: Change web page separated buttons for on/off, open/close, etc. to single buttons

### Known Issues

* Still testing... Future updates MAY include breaking changes requiring a flash erase and re-upload.

## v3.0.3 (2024-12-19)

### What's Changed

* Bugfix: Soft AP list of available WiFi networks not properly terminated
* Bugfix: Setting static IP address did not set correct subnet mask
* Bugfix: Crash in soft AP because memory buffer was not allocated
* Feature: Add support Improv-based WiFi provisioning
* Feature: Add the 10 minute timeout on soft AP mode
* Feature: Add the check for WiFi connectivity 30 seconds after change to static IP

### Known Issues

* Still testing... Future updates MAY include breaking changes requiring a flash erase and re-upload.

## v3.0.2 (2024-12-16)

### What's Changed

* Feature: Dry contact support, with thanks to @tlhagan
* Feature: Add support for HomeKit Identify characteristic
* Feature: Support for ratgdo32 as well as ratgdo32-disco
* Bugfix: Vehicle distance sensor logic

### Known Issues

* Still testing... Future updates MAY include breaking changes requiring a flash erase and re-upload.

## v3.0.1 (2024-12-11)

### What's Changed

* Feature: Beeps during time-to-close delay
* Feature: Vehicle presence, arriving, departing sensing, and parking assist laser
* Bugfix: Web-based flash installer now working
* Bugfix: OTA update from GitHub now working
* Bugfix: Blue LED was not blinking
* Bugfix: Device name not initialized to default on startup

### Known Issues

* Still testing... Future updates MAY include breaking changes requiring a flash erase and re-upload.

## v3.0.0 (2024-11-30)

### What's Changed

* New release for Ratgdo32 - DISCO

### Known Issues

* THIS IS PRE-RELEASE FIRMWARE for testing purposes. Future updates MAY include breaking changes requiring a flash erase and re-upload.

