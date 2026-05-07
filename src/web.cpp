/****************************************************************************
 * RATGDO HomeKit
 * https://ratcloud.llc
 * https://github.com/PaulWieland/ratgdo
 *
 * Copyright (c) 2023-25 David A Kerr... https://github.com/dkerr64/
 * All Rights Reserved.
 * Licensed under terms of the GPL-3.0 License.
 *
 * Contributions acknowledged from
 * Brandon Matthews... https://github.com/thenewwazoo
 * Jonathan Stroud...  https://github.com/jgstroud
 * Mitchell Solomon... https://github.com/mitchjs
 * Haglerd...          https://github.com/Haglerd  (2025-26: forceClose /setgdo POST handler, auto-close timer)
 *
 */

// C/C++ language includes
#include <string>
#include <tuple>
#include <unordered_map>
#include <time.h>

// ESP system includes
#include <Ticker.h>
#include <MD5Builder.h>
#include <StreamString.h>
#ifdef ESP8266
#include <arduino_homekit_server.h>
#include <eboot_command.h>
#include <ESP8266mDNS.h>
#else
#include "esp_core_dump.h"
#include <ESPmDNS.h>
#ifndef ESP8266
// v24: setsockopt(SO_SNDTIMEO) on SSE TCP sockets to bound write times.
// v47: setsockopt(TCP_KEEPIDLE/INTVL/CNT) on SSE TCP sockets so kernel
// detects silently-dropped peers within ~60s.
// esp_timer_get_time for clientWrite slow-write instrumentation.
// log-audit-004: direct lwip_send in clientWriteEx (errno + send()).
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/tcp.h>
#include <esp_timer.h>
#include <errno.h>
#endif
#endif

// RATGDO project includes
#ifdef USE_GDOLIB
#include "gdo.h"
#endif
#include "ratgdo.h"
#include "config.h"
#include "comms.h"
#include "web.h"
#include "homekit.h"
#include "softAP.h"
#include "json.h"
#include "led.h"
#include "instrumentation.h"
#ifdef ESP8266
#include "wifi_8266.h"
#endif
#ifdef RATGDO32_DISCO
#include "vehicle.h"
#endif
// built by "build_web_content.py"
#include "webcontent.h"

// Logger tag
static const char *TAG = "ratgdo-http";

// Browser cache control, time in seconds after which browser cache invalid
// This is used for CSS, JS and IMAGE file types.  Set to 30 days !!
#define CACHE_CONTROL (60 * 60 * 24 * 30)

// Forward declare the internal URI handling functions...
void handle_reset();
void handle_reconnect_homekit();
void handle_refresh_mdns();
void handle_dump_homekit_state();
void handle_status();
void handle_everything();
void handle_setgdo();
void handle_logout();
void handle_auth();
void handle_subscribe();
void handle_unsubscribe(); // v27: best-effort beacon cleanup, see handle_unsubscribe()
void handle_showlog();
void handle_showrebootlog();
void handle_crashlog();
void handle_clearcrashlog();
#ifdef CRASH_DEBUG
void handle_forcecrash();
void handle_crash_oom();
void *crashptr;
char *test_str = NULL;
#endif
void handle_update();
void handle_firmware_upload();
void SSEHandler(uint32_t channel);
void add_static_mdns();
void add_dynamic_mdns();
#ifndef ESP8266
void try_register_ratgdo_mdns();
#endif

// Built in URI handlers
const char restEvents[] = "/rest/events/";
const std::unordered_map<std::string, std::pair<const HTTPMethod, void (*)()>> builtInUri = {
    {"/status.json", {HTTP_GET, handle_status}},
    {"/reset", {HTTP_POST, handle_reset}},
    {"/reboot", {HTTP_POST, handle_reboot}},
    {"/reconnectHomeKit", {HTTP_POST, handle_reconnect_homekit}},
    {"/refreshHomeKitMDNS", {HTTP_POST, handle_refresh_mdns}},
    {"/dumpHomeKitState", {HTTP_POST, handle_dump_homekit_state}},
    {"/setgdo", {HTTP_POST, handle_setgdo}},
    {"/logout", {HTTP_GET, handle_logout}},
    {"/auth", {HTTP_GET, handle_auth}},
    {"/showlog", {HTTP_GET, handle_showlog}},
    {"/showrebootlog", {HTTP_GET, handle_showrebootlog}},
    {"/wifiap", {HTTP_POST, handle_wifiap}},
    {"/wifinets", {HTTP_GET, handle_wifinets}},
    {"/setssid", {HTTP_POST, handle_setssid}},
    {"/rescan", {HTTP_POST, handle_rescan}},
    {"/crashlog", {HTTP_GET, handle_crashlog}},
    {"/clearcrashlog", {HTTP_GET, handle_clearcrashlog}},
#ifdef CRASH_DEBUG
    {"/forcecrash", {HTTP_POST, handle_forcecrash}},
    {"/crashoom", {HTTP_POST, handle_crash_oom}},
#endif
    {"/rest/events/subscribe", {HTTP_GET, handle_subscribe}},
    // v27: paired endpoint for navigator.sendBeacon() on beforeunload
    {"/rest/events/unsubscribe", {HTTP_POST, handle_unsubscribe}}};

// Declare web server on HTTP port 80.
#ifdef ESP8266
ESP8266WebServer server(80);
#else
WebServer server(80);
#endif

// Local copy of door status
GarageDoor last_reported_garage_door;
bool last_reported_paired = false;
bool last_reported_assist_laser = false;
_millis_t lastDoorUpdateAt;
_millis_t lastDoorOpenAt;
_millis_t lastDoorCloseAt;
GarageDoorCurrentState lastDoorState = (GarageDoorCurrentState)0xff;
static bool new_ipv4_address = false;
static bool new_ipv6_address = false;

bool web_setup_done = false;

// Implement our own firmware update so can enforce MD5 check.
// Based on ESP8266HTTPUpdateServer
std::string _updaterError;
bool _authenticatedUpdate;
char firmwareMD5[36] = "";
size_t firmwareSize = 0;

// Common HTTP responses
constexpr char response400missing[] = "400: Bad Request, missing argument\n";
constexpr char response400invalid[] = "400: Bad Request, invalid argument\n";
constexpr char response404[] = "404: Not Found\n";
constexpr char response503[] = "503: Service Unavailable.\n";
constexpr char response200[] = "HTTP/1.1 200 OK\nContent-Type: text/plain\nConnection: close\n\n";

const char *http_methods[] = {"HTTP_ANY", "HTTP_GET", "HTTP_HEAD", "HTTP_POST", "HTTP_PUT", "HTTP_PATCH", "HTTP_DELETE", "HTTP_OPTIONS"};

// All this is to support a 303 redirect to js.map files when debugging, so we don't have to embed in our firmware !!!!
#ifndef STRINGIFY
#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)
#endif
// If not building in main github repo, then add -D GITUSER=your_userid to the compile line (no quotes, STRINGIFY adds that here)
#ifndef GITUSER
#define _GITUSER "ratgdo"
#else
#define _GITUSER STRINGIFY(GITUSER)
#endif
#ifndef GITREPO
#ifdef ESP8266
#define _GITREPO "homekit-ratgdo"
#else
#define _GITREPO "homekit-ratgdo32"
#endif
#else
#define _GITREPO STRINGIFY(GITREPO)
#endif
#ifndef GITBRANCH
#define _GITBRANCH "main"
#else
#define _GITBRANCH STRINGIFY(GITBRANCH)
#endif
constexpr char gitUser[] = _GITUSER;
constexpr char gitRepo[] = _GITREPO;
constexpr char gitRawURL[] = "https://raw.githubusercontent.com/" _GITUSER "/" _GITREPO "/refs/heads/" _GITBRANCH;
constexpr char gitTaggedURL[] = "https://raw.githubusercontent.com/" _GITUSER "/" _GITREPO "/refs/tags/v" AUTO_VERSION;

// For Server Sent Events (SSE) support
// Just reloading page causes register on new channel.  So we need a reasonable number
// to accommodate "extra" until old one is detected as disconnected.
#define SSE_MAX_CHANNELS 8
// Orphan-slot sweep timeouts. Run from service_timer_loop independent
// of the per-slot heartbeat Ticker (which doesn't fire when heartbeat=0).
//   PREHANDSHAKE: subscribed but EventSource never came back to /events/N.
//   IDLE        : connected but no broadcast/heartbeat traffic — class 5b
//                 (client.connected() == false) is the primary safety net;
//                 this is belt-and-suspenders for a wedged TCP socket
//                 whose lwIP cache hasn't caught up.
#define SSE_PREHANDSHAKE_TIMEOUT_MS  5000UL
#define SSE_IDLE_TIMEOUT_MS         300000UL
// v47: consecutive BUFFER_FULL count threshold for declaring a subscriber
// wedged. After this many flow-control skips with no successful drain in
// between, the sweep marks the slot pendingRemove. Justification:
//   - 30 increments at the typical 1-event/s broadcast cadence = 30s
//     real-time reap, 10x faster than the 300s SSE_IDLE_TIMEOUT_MS belt-
//     and-suspenders 5c sweep.
//   - At 100ms minimum broadcast cadence (RATGDO_STATUS burst) = 3s,
//     comfortably above typical Tailscale DERP relay handover (2-3s)
//     and below 4s LTE backpressure clearing windows.
//   - A successful clientWriteEx (OK only — NOT BUFFER_FULL) resets the
//     counter, so a chronically-slow-but-occasionally-draining peer
//     stays connected.
//   - Heartbeat-only slots never accumulate: heartbeat payload is small
//     (a few hundred bytes, easily fits in lwIP's send buffer), so
//     availableForWrite never returns < len absent a true wedge.
constexpr uint32_t SSE_MAX_CONSECUTIVE_BUFFER_FULL = 30;
struct SSESubscription
{
    IPAddress clientIP;
    WiFiClient client;
    Ticker heartbeatTimer;
    uint32_t heartbeatInterval;
    bool SSEconnected;
    int SSEfailCount;
    String clientUUID;
    bool logViewer;
    // v22: deferred-cleanup flag to break the heartbeatTimer self-detach
    // crash. SSEheartbeat() runs IN the Ticker callback context — calling
    // heartbeatTimer.detach() from there ends up in vTaskDelete on the
    // Ticker's own task, which corrupts the FreeRTOS task list and
    // panics in uxListRemove. Instead we set this flag and let
    // service_timer_loop() (main loop context) do the actual remove.
    volatile bool pendingRemove;
    // v27: timestamps for the orphan-slot sweep that runs independent
    // of the heartbeat Ticker. Pre-v27 the only liveness driver was
    // SSEheartbeat which only ran if heartbeatInterval > 0 — clients
    // that subscribed with heartbeat=0 (like logs.html does) leaked
    // slots forever once the page navigated away. Now the orphan sweep
    // in service_timer_loop catches three classes of leaks: (1)
    // subscribed-but-never-connected to /events/N within 15s, (2)
    // SSEconnected with client.connected()==false, (3) idle for over
    // 120s with no broadcast traffic (heartbeat=0 belt+suspenders).
    // v28: int64_t was an int64_t-tearing risk — read by sweep
    // (main loop) racing writers in Ticker / SSEBroadcastState.
    // Truncated uint32_t millis is safe: 32-bit aligned writes are
    // atomic on ESP32, subtraction is wrap-safe modulo 2^32, and
    // the intervals we compare against (15000/120000 ms) fit
    // comfortably. Even the ~49.7-day rollover causes at most one
    // bad delta on the wrap tick — recoverable next tick.
    volatile uint32_t subscribedAt;
    volatile uint32_t lastActivity;
    // v47: consecutive BUFFER_FULL count for sweep class 5d (wedged on
    // flow control). Reset to 0 on every SseWriteResult::OK; incremented
    // (atomic) on every SseWriteResult::BUFFER_FULL. Sweep reaps the
    // slot when this exceeds SSE_MAX_CONSECUTIVE_BUFFER_FULL.
    // Multi-writer (loopTask + esp_timer + LOG broadcast tasks),
    // single-reader (sweep on loopTask). 32-bit aligned writes are
    // atomic on Xtensa; uses __atomic_* for fetch-add safety under the
    // multi-writer fanout. Same pattern as sseSlowWrites/sseBufferFullSkips
    // global counters.
    volatile uint32_t consecutiveBufferFull;
    // log-audit-003: timestamp of the first BUFFER_FULL of the current
    // streak (i.e. the BUFFER_FULL that took consecutiveBufferFull from
    // 0 -> 1). Reset to 0 alongside consecutiveBufferFull on OK / slot
    // free / subscribe. Used by the wedged-reap log line to report
    // wedgedFor=Xms — how long the connection sat unable to drain
    // before the reap fired. Same multi-writer fanout as the counter;
    // 32-bit aligned writes are atomic on Xtensa, single-reader (sweep)
    // observes a stale value at most one tick (recoverable).
    volatile uint32_t firstBufferFullAt;
};
SSESubscription subscription[SSE_MAX_CHANNELS];

// log-audit-003: per-UUID rapid-recurrence dampener for sweep-class-5d
// reaps. iOS Safari background-tab SSE silencing produces a tight
// reap-recover loop where the same browser-side UUID re-subscribes
// within seconds of being reaped, immediately re-wedges, and gets reaped
// again. One device session was observed reaping the same UUID 28 times
// over 42h. Each reap functioning correctly; root cause is the client
// re-subscribe behavior we can't fix server-side.
//
// Mitigation: when a slot is reaped via class-5d (wedged on flow
// control), stamp its UUID + reap time into recentReaps[]. handle_subscribe
// scans this table on every incoming subscribe; if the same UUID was
// reaped within the last 60s, return 429 instead of allocating a slot.
// The misbehaving client gets backed off; well-behaved clients with
// transient wedge events (a single reap then a clean reconnect after
// >60s) are unaffected.
//
// Heap budget: SSE_MAX_CHANNELS (8) slots * 40 bytes = 320 bytes BSS.
// Zero heap allocation — uuid is fixed char[40] (UUID = 36 chars + null +
// slack), not String. Acceptable on ESP8266.
//
// Threading: handle_subscribe and the orphan sweep both run on
// loopTask (single-threaded webserver / Ticker-deferred-sweep model
// in use here); no mutex needed.
struct RecentReap
{
    char uuid[40];      // 36-char UUID + null + slack; uuid[0] == '\0' = empty
    uint32_t reapedAt;  // millis() stamp; 0 = empty
};
static RecentReap recentReaps[SSE_MAX_CHANNELS];
constexpr uint32_t SSE_DAMPENER_WINDOW_MS = 60000;
// During firmware update note which subscribed client is updating.
// v38 (audit W5): `volatile` qualifier added. Pointer is written by
// handle_firmware_upload (loopTask) and read by the HK watchdog Ticker
// callback (esp_timer task) via firmware_update_in_progress(). Without
// `volatile` an aggressive optimizer / future re-enabled LTO could
// hoist the load out of the function and miss the toggle. The pointer
// itself is word-aligned-atomic on Xtensa so a stale-load is the only
// real concern, and the watchdog re-enters every 180s so a missed tick
// is self-correcting — but the discipline cost of `volatile` is one
// keyword.
SSESubscription * volatile firmwareUpdateSub = NULL;
uint32_t subscriptionCount = 0;

// Public OTA-in-progress check used by the HK watchdog to inhibit
// auto-recover (audit F5 — a WiFi cycle during an active upload aborts
// the transfer and triggers rollback). Single pointer load = atomic on
// Xtensa; no synchronization needed for a hint-quality signal.
bool firmware_update_in_progress()
{
    return firmwareUpdateSub != NULL;
}

// Performance management - removed redundant connection tracking
#define MIN_REQUEST_INTERVAL_MS 100

// Performance monitoring
static uint32_t request_count = 0;
static uint32_t max_response_time = 0;

#ifdef ESP8266
// ESP8266 is single core / single threaded, no mutex's.
#define TAKE_MUTEX()
#define GIVE_MUTEX()
#else
// ESP32 is multi-core, need to serialize access to JSON buffers
static SemaphoreHandle_t jsonMutex = NULL;
#define TAKE_MUTEX() \
    if (jsonMutex)   \
    xSemaphoreTake(jsonMutex, portMAX_DELAY)
#define GIVE_MUTEX() \
    if (jsonMutex)   \
    xSemaphoreGive(jsonMutex)
#endif

// v29: tri-state return from clientWrite. Pre-v29 was bool, which
// conflated "TCP buffer full this instant" (flow control) with "write
// failed" (real wedge). The 120s idle reap (sweep class 5c) treated
// both as "no activity" → reaped healthy slots on chronically-slow
// tunnels (Tailscale, mobile data with marginal signal).
enum class SseWriteResult : uint8_t { OK, BUFFER_FULL, FAILED };

// mDNS update management... re-announcing every 2 minutes.
#define MDNS_ANNOUNCE_TIMEOUT (2 * 60 * 1000)
// But not more often than every 10 seconds if pending updates.
#define MDNS_UPDATE_INTERVAL (10 * 1000)
static _millis_t lastMDNSupdate = 0;
static bool mdnsUpdatePending = false;

// BOOT-OOM-MDNS: defer the ratgdo mDNS service-registration burst at the
// end of setup_web until ESP32 free heap recovers above the floor below.
// The synchronous burst at boot (4 calls: http, ratgdo, static-TXT,
// dynamic-TXT) momentarily allocates ~10-20 KB transiently and has been
// observed driving free heap below the mDNS lwIP send threshold during
// the post-WiFi-up boot window, triggering a panic. Polled from web_loop
// once heap recovers, with a hard 30 s timeout fallback so registration
// happens even if heap stays pinned. ESP8266 path is unchanged.
#ifndef ESP8266
static bool ratgdo_mdns_register_pending = false;
static constexpr uint32_t RATGDO_MDNS_HEAP_FLOOR_BYTES = 50 * 1024;
static constexpr uint32_t RATGDO_MDNS_MAX_DEFER_MS = 30 * 1000;
#endif

// Connection throttling
#define MAX_CONCURRENT_REQUESTS 8
#define REQUEST_TIMEOUT_MS 2000
struct ActiveRequest
{
    IPAddress clientIP;
    _millis_t startTime;
    bool inUse;
};
ActiveRequest activeRequests[MAX_CONCURRENT_REQUESTS];
int activeRequestCount = 0;

#define CLIENT_WRITE_TIMEOUT 500
// v24: maximum time a single SSE clientWrite is allowed to spend in
// client.write before we give up + report the slow channel. Was
// effectively unbounded — Arduino-ESP32 WiFiClient::setTimeout only
// affects READS, not writes, so a wedged subscriber would block the
// caller indefinitely. Combined with the lwIP SO_SNDTIMEO set in
// SSEHandler this caps the write to ~200ms.
#define CLIENT_SLOW_WRITE_MS 200
// W43: 512 B scratch buffer used to assemble formatted strings before
// they're written to the network (303 redirect URLs, status JSON open/
// close history blocks, OTA upload-progress SSE frames, ESP8266
// SSEBroadcastState payloads).
//
// INVARIANT (ESP32): only written from loopTask context — Arduino
// WebServer dispatch, OTA UPLOAD_FILE_WRITE handler, and the loopTask
// web_loop status-broadcast path. SSEBroadcastState on ESP32 uses a
// stack-local 512 B buffer instead of this global (see comment in
// SSEBroadcastState for the v38 race that motivated that split).
//
// INVARIANT (ESP8266): single-task cooperative scheduling means there
// is no concurrent writer; SSEBroadcastState reuses this global on the
// 8266 (the ~4 KB main-task stack is too tight to absorb +512 B of
// stack-local state per call).
//
// Renamed from `writeBuffer` (W43) to make the loopTask-only contract
// explicit at every call site. Per-caller stack buffers were rejected
// during planning for ESP8266 stack pressure.
static char loopTaskScratchBuf512[512];
// v24: bump per-broadcast slow-write counter when a single channel
// exceeds CLIENT_SLOW_WRITE_MS. homekit_health_log reads + zeros this
// every 180s. Climbing values pre-freeze identify a wedged subscriber.
volatile uint32_t sseSlowWrites = 0;
// v29: count of clientWrite calls skipped because the lwIP send buffer
// couldn't accept the full payload (flow control). Distinguishes
// chronically-slow connections (Tailscale, congested links) from
// genuinely-wedged sockets — drives whether v30 needs per-slot
// adaptive idle timeouts.
volatile uint32_t sseBufferFullSkips = 0;
// v27: live count of allocated SSE slots (refreshed by sweep_sse_orphans
// every service tick) and a windowed counter of slots reaped by the
// sweep. homekit_health_log reads both every 180s and zeros sseOrphansReaped
// (sseSlotsAlloc is a snapshot, not a counter). Lets us see at a glance
// whether the slot leak that caused the v26 25s-post-boot wedge is back.
volatile uint32_t sseSlotsAlloc = 0;
volatile uint32_t sseOrphansReaped = 0;
// MH6 instrumentation (v33): peak JSON length seen during a build —
// used to inform a future STATUS_JSON_BUFFER_SIZE retune. Updated via
// CAS-loop max from handle_status; read+zeroed each health-log window.
volatile uint32_t statusJsonPeakLen = 0;
// v29: tri-state version of clientWrite. Distinguishes "lwIP send buffer
// can't accept the full payload right now" (BUFFER_FULL — peer is alive
// but slow / link is congested, e.g. Tailscale tunnel) from "client.write
// returned 0 after lwIP accepted bytes for delivery" (FAILED — real wedge,
// socket already stopped). Callers stamp lastActivity on OK or BUFFER_FULL
// so the orphan-sweep idle check (5c) doesn't misclassify a chronically-
// flow-controlled slot as idle. Only FAILED skips the stamp.
SseWriteResult clientWriteEx(WiFiClient client, const char *data)
{
    size_t len = strlen(data);
#ifdef ESP8266
    client.flush(); // make sure previous data all sent.
    // ESP8266's WiFiClient::availableForWrite() actually queries lwIP's
    // tcp_sndbuf (it's overridden, unlike Arduino-ESP32). The fast-path
    // is meaningful here.
    int avail = client.availableForWrite();
    if (avail >= 0 && (size_t)avail < len)
    {
        sseBufferFullSkips++;
        static uint32_t lastSkipLogMs = 0;
        uint32_t nowMs = (uint32_t)_millis();
        if ((uint32_t)(nowMs - lastSkipLogMs) > 60000UL || lastSkipLogMs == 0)
        {
            ESP_LOGD(TAG, "SSE clientWrite skipped — buffer full (need %u, have %d) [%lu total skips]",
                     (unsigned)len, avail, (unsigned long)sseBufferFullSkips);
            lastSkipLogMs = nowMs;
        }
        return SseWriteResult::BUFFER_FULL;
    }
    uint32_t t0 = millis();
    size_t written = client.write(data, len);
    uint32_t dt = millis() - t0;
    if (dt > CLIENT_SLOW_WRITE_MS)
    {
        sseSlowWrites++;
        ESP_LOGW(TAG, "SSE clientWrite slow: %ums for %u bytes (subscriber may be wedged)", dt, (unsigned)len);
    }
    if (written == 0)
    {
        YIELD();
        client.stop();
        ESP_LOGW(TAG, "Failed writing to WiFi Client (%d of %d), connection closed.", written, len);
        return SseWriteResult::FAILED;
    }
    return SseWriteResult::OK;
#else
    // log-audit-004 (errno 11 recurrence): bypass NetworkClient::write
    // and call lwip_send directly. Two reasons:
    //
    // 1) Arduino-ESP32's NetworkClient does NOT override
    //    Print::availableForWrite() (which returns 0). The previous
    //    fast-path `if (client.availableForWrite() < len) BUFFER_FULL`
    //    therefore matched on every call, returning BUFFER_FULL without
    //    ever calling client.write — normal-size status broadcasts
    //    silently failed and v47's wedge sweep eventually reaped the
    //    slot.
    //
    // 2) NetworkClient::write's send-retry loop logs ESP_LOGE
    //    ("fail on fd %d, errno: %d, ...") UNCONDITIONALLY when
    //    lwip_send returns -1 with EAGAIN (TCP send buffer full).
    //    On a long-lived SSE socket against a slow peer that's normal
    //    flow control, but the framework's log_e fires up to
    //    WIFI_CLIENT_MAX_WRITE_RETRY=10 times per write — the visible
    //    `errno 11 fail on fd 51/52 "No more processes"` syslog noise.
    //
    // Direct lwip_send(MSG_DONTWAIT) lets us treat EAGAIN as a clean
    // BUFFER_FULL signal (no log_e), and still surface other errno
    // values (ECONNRESET / EPIPE / ENOTCONN) as FAILED with a single
    // ESP_LOGW so we can see real socket errors.
    int fd = client.fd();
    if (fd < 0)
    {
        return SseWriteResult::FAILED;
    }
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000ULL);
    size_t totalSent = 0;
    while (totalSent < len)
    {
        ssize_t n = ::send(fd, data + totalSent, len - totalSent, MSG_DONTWAIT);
        if (n > 0)
        {
            totalSent += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            if (totalSent == 0)
            {
                // Clean buffer-full at the start of the payload — peer
                // is alive but slow. v47 wedge sweep will reap if this
                // persists for SSE_MAX_CONSECUTIVE_BUFFER_FULL=30 ticks.
                sseBufferFullSkips++;
                static uint32_t lastSkipLogMs = 0;
                uint32_t nowMs = (uint32_t)_millis();
                if ((uint32_t)(nowMs - lastSkipLogMs) > 60000UL || lastSkipLogMs == 0)
                {
                    ESP_LOGD(TAG, "SSE clientWrite skipped — lwip_send EAGAIN (need %u) [%lu total skips]",
                             (unsigned)len, (unsigned long)sseBufferFullSkips);
                    lastSkipLogMs = nowMs;
                }
                return SseWriteResult::BUFFER_FULL;
            }
            // Mid-payload buffer-full: a partial frame is on the wire.
            // Bail within the slow-write budget; if we exceed it, stop
            // the connection so the peer resubscribes from a clean
            // SSE event boundary instead of mid-frame.
            uint32_t dtNow = (uint32_t)(esp_timer_get_time() / 1000ULL) - t0;
            if (dtNow > CLIENT_SLOW_WRITE_MS)
            {
                sseSlowWrites++;
                ESP_LOGW(TAG, "SSE clientWrite mid-payload wedge (sent %u of %u in %ums) — stopping",
                         (unsigned)totalSent, (unsigned)len, dtNow);
                client.stop();
                return SseWriteResult::FAILED;
            }
            // Brief yield so lwIP's TCPIP task can drain.
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        // Real error: ECONNRESET / EPIPE / ENOTCONN / etc. One ESP_LOGW
        // per occurrence (NOT the framework's per-retry log_e flood).
        ESP_LOGW(TAG, "SSE clientWrite send error fd=%d errno=%d (%s) — stopping",
                 fd, errno, strerror(errno));
        client.stop();
        return SseWriteResult::FAILED;
    }
    uint32_t dt = (uint32_t)(esp_timer_get_time() / 1000ULL) - t0;
    if (dt > CLIENT_SLOW_WRITE_MS)
    {
        sseSlowWrites++;
        ESP_LOGW(TAG, "SSE clientWrite slow: %ums for %u bytes (subscriber may be wedged)", dt, (unsigned)len);
    }
    return SseWriteResult::OK;
#endif
}

// v29: thin bool wrapper for any callers we don't migrate to the tri-state.
// Returns true on OK or BUFFER_FULL (both = "broadcast loop reached this
// slot and tried"; peer is alive enough to keep), false only on FAILED.
bool clientWrite(WiFiClient client, const char *data)
{
    return clientWriteEx(client, data) != SseWriteResult::FAILED;
}

// Helper functions for connection throttling
bool registerRequest()
{
    IPAddress clientIP = server.client().remoteIP();
    _millis_t now = _millis();

    // Clean up timed-out requests
    for (int i = 0; i < MAX_CONCURRENT_REQUESTS; i++)
    {
        if (activeRequests[i].inUse && (now - activeRequests[i].startTime > REQUEST_TIMEOUT_MS))
        {
            ESP_LOGD(TAG, "Request timeout for client %s", activeRequests[i].clientIP.toString().c_str());
            activeRequests[i].inUse = false;
            activeRequestCount--;
        }
    }

    // Check if we're at capacity
    if (activeRequestCount >= MAX_CONCURRENT_REQUESTS)
    {
        ESP_LOGE(TAG, "Max concurrent requests reached, rejecting %s", clientIP.toString().c_str());
        return false;
    }

    // Find a free slot
    for (int i = 0; i < MAX_CONCURRENT_REQUESTS; i++)
    {
        if (!activeRequests[i].inUse)
        {
            activeRequests[i].clientIP = clientIP;
            activeRequests[i].startTime = now;
            activeRequests[i].inUse = true;
            activeRequestCount++;
            return true;
        }
    }

    return false;
}

void unregisterRequest()
{
    IPAddress clientIP = server.client().remoteIP();

    for (int i = 0; i < MAX_CONCURRENT_REQUESTS; i++)
    {
        if (activeRequests[i].inUse && activeRequests[i].clientIP == clientIP)
        {
            activeRequests[i].inUse = false;
            if (activeRequestCount > 0)
                activeRequestCount--; // Prevent negative count
            break;
        }
    }
}

void notify_new_ipv4_address()
{
    TAKE_MUTEX();
    new_ipv4_address = true;
    GIVE_MUTEX();
};

#ifndef ESP8266
void notify_new_ipv6_address()
{
    TAKE_MUTEX();
    new_ipv6_address = true;
    GIVE_MUTEX();
};
#endif

void web_loop()
{
    if (!web_setup_done)
        return;

#ifndef ESP8266
    if (ratgdo_mdns_register_pending) try_register_ratgdo_mdns();
#endif

    static char *json = status_json;
    _millis_t upTime = _millis();
    static _millis_t last_request_time = 0;

    // manage frequency of mDNS updates
#ifndef ESP8266
    if (!ratgdo_mdns_register_pending)
#endif
    {
        if (mdnsUpdatePending)
        {
            if (upTime - lastMDNSupdate > MDNS_UPDATE_INTERVAL)
            {
                // This function also resets mdnsUpdatePending and lastMDNSupdate.
                add_dynamic_mdns();
            }
        }
        else if (upTime - lastMDNSupdate > MDNS_ANNOUNCE_TIMEOUT)
        {
            // if it has been more than MDNS_ANNOUNCE_TIMEOUT since last update, re-announce
            add_dynamic_mdns();
        }
    }

    // W48: SSE delta-broadcast — `_C` vs raw `JSON_ADD_*` variant rule
    // (ESTABLISHED CONVENTION; do not drift):
    //
    //   - `JSON_ADD_*_C` (change-tracked) — used for fields the broadcaster
    //     polls EVERY TICK where a per-field "last reported" cache slot
    //     exists (typically in `last_reported_garage_door`, plus the
    //     `last_reported_paired` / `last_reported_assist_laser` singletons).
    //     The macro compares to the cache and only emits if the value
    //     actually changed — this is the bandwidth-saving variant.
    //   - Raw `JSON_ADD_INT/STR/BOOL` — used for fields gated by a
    //     single-shot event flag (`vehicleStatusChange`, `new_ipv4_address`,
    //     `new_ipv6_address`, the `lastDoorState != current_state`
    //     outer guard for the door-update timestamps). The flag itself
    //     IS the change detector, so a per-field cache is unnecessary.
    //   - `upTime` is always-emit, but only inside the
    //     `if (strlen(json) > 2)` guard below — it serves as the
    //     timestamp when SOMETHING else changed.
    //
    // Sibling path: the polled `/status.json` snapshot in
    // `build_status_json` (around line 1670+) intentionally uses RAW
    // variants for ALL fields — full snapshot every poll, no cache,
    // by design (consumers expect every field every time). Do NOT
    // copy `_C` calls from this SSE path into `build_status_json` or
    // vice versa; they have different consumer contracts.
    TAKE_MUTEX();
    JSON_START(json);
    if (garage_door.active && garage_door.current_state != lastDoorState)
    {
        ESP_LOGD(TAG, "Current Door State changing from %s to %s", DOOR_STATE(lastDoorState), DOOR_STATE(garage_door.current_state));
        if (enableNTP && clockSet)
        {
            time_t timeNow = time(NULL);
            if (lastDoorState == 0xff)
            {
                // initialize with saved time.
                // lastDoorUpdateAt is milliseconds relative to system reboot time.
                lastDoorUpdateAt = (userConfig->getDoorUpdateAt() != 0) ? ((userConfig->getDoorUpdateAt() - timeNow) * 1000) + upTime : 0;
                lastDoorOpenAt = (userConfig->getDoorOpenAt() != 0) ? ((userConfig->getDoorOpenAt() - timeNow) * 1000) + upTime : 0;
                lastDoorCloseAt = (userConfig->getDoorCloseAt() != 0) ? ((userConfig->getDoorCloseAt() - timeNow) * 1000) + upTime : 0;
            }
            else
            {
                // first state change after a reboot, so really is a state change.
                lastDoorUpdateAt = upTime;
                userConfig->set(cfg_doorUpdateAt, (int)timeNow);
                if (garage_door.current_state == GarageDoorCurrentState::CURR_OPEN)
                {
                    lastDoorOpenAt = upTime;
                    userConfig->set(cfg_doorOpenAt, (int)timeNow);
                }
                if (garage_door.current_state == GarageDoorCurrentState::CURR_CLOSED)
                {
                    lastDoorCloseAt = upTime;
                    userConfig->set(cfg_doorCloseAt, (int)timeNow);
                }
                ESP8266_SAVE_CONFIG();
            }
        }
        else
        {
            // No realtime set, use upTime.
            lastDoorUpdateAt = (lastDoorState == 0xff) ? 0 : upTime;
            if (garage_door.current_state == GarageDoorCurrentState::CURR_OPEN)
                lastDoorOpenAt = lastDoorUpdateAt;
            if (garage_door.current_state == GarageDoorCurrentState::CURR_CLOSED)
                lastDoorCloseAt = lastDoorUpdateAt;
        }
        lastDoorState = garage_door.current_state;
        // We send milliseconds relative to current time... ie updated X milliseconds ago
        // First time through, zero offset from upTime, which is when we last rebooted)
        JSON_ADD_INT(cfg_doorUpdateAt, (upTime - lastDoorUpdateAt));
        JSON_ADD_INT(cfg_doorOpenAt, (upTime - lastDoorOpenAt));
        JSON_ADD_INT(cfg_doorCloseAt, (upTime - lastDoorCloseAt));
    }
#ifdef RATGDO32_DISCO
    // Feature not available on ESP8266
    if (garage_door.has_distance_sensor)
    {
        if (vehicleStatusChange)
        {
            vehicleStatusChange = false;
            JSON_ADD_STR("vehicleStatus", vehicleStatus);
        }
        JSON_ADD_BOOL_C("assistLaser", laser.state(), last_reported_assist_laser);
    }
#endif
    // Conditional macros, only add if value has changed
    JSON_ADD_BOOL_C("paired", homekit_is_paired(), last_reported_paired);
    JSON_ADD_STR_C("garageDoorState", DOOR_STATE(garage_door.current_state), garage_door.current_state, last_reported_garage_door.current_state);
    JSON_ADD_STR_C("garageLockState", REMOTES_STATE(garage_door.current_lock), garage_door.current_lock, last_reported_garage_door.current_lock);
    JSON_ADD_BOOL_C("garageLightOn", garage_door.light, last_reported_garage_door.light);
    JSON_ADD_BOOL_C("garageMotion", garage_door.motion, last_reported_garage_door.motion);
    JSON_ADD_BOOL_C("pinBasedObst", garage_door.pinModeObstructionSensor, last_reported_garage_door.pinModeObstructionSensor);
    JSON_ADD_BOOL_C("garageObstructed", garage_door.obstructed, last_reported_garage_door.obstructed);
    JSON_ADD_BOOL_C("garageSec1Emulated", garage_door.wallPanelEmulated, last_reported_garage_door.wallPanelEmulated);
    if (doorControlType == 2)
    {
        JSON_ADD_INT_C("batteryState", garage_door.batteryState, last_reported_garage_door.batteryState);
        JSON_ADD_INT_C("openingsCount", garage_door.openingsCount, last_reported_garage_door.openingsCount);
        JSON_ADD_INT_C(cfg_builtInTTC, garage_door.builtInTTC, last_reported_garage_door.builtInTTC);
        JSON_ADD_INT_C("builtInTTCremaining", garage_door.builtInTTCremaining, last_reported_garage_door.builtInTTCremaining);
        JSON_ADD_BOOL_C("builtInTTChold", garage_door.builtInTTChold, last_reported_garage_door.builtInTTChold);
    }
    JSON_ADD_INT_C("openDuration", garage_door.openDuration, last_reported_garage_door.openDuration);
    JSON_ADD_INT_C("closeDuration", garage_door.closeDuration, last_reported_garage_door.closeDuration);
    JSON_ADD_INT_C("ttcActive", is_ttc_active(), last_reported_garage_door.ttcActive);
    if (new_ipv4_address)
    {
        JSON_ADD_STR(cfg_localIP, userConfig->getLocalIP());
        JSON_ADD_STR(cfg_subnetMask, userConfig->getSubnetMask());
        JSON_ADD_STR(cfg_gatewayIP, userConfig->getGatewayIP());
        JSON_ADD_STR(cfg_nameserverIP, userConfig->getNameserverIP());
        new_ipv4_address = false;
    }
#ifndef ESP8266
    if (new_ipv6_address)
    {
        JSON_ADD_STR("ipv6Addresses", ipv6_addresses);
        new_ipv6_address = false;
    }
#endif
    // got any json?
    if (strlen(json) > 2)
    {
        // Have we added anything to the JSON string?
        JSON_ADD_INT("upTime", upTime);
        JSON_END();
        if (strlen(json) > STATUS_JSON_BUFFER_SIZE * 8 / 10)
        {
            ESP_LOGW(TAG, "WARNING web_loop JSON length: %d is over 80%% of available buffer", strlen(json));
        }
        JSON_REMOVE_NL(json);
        // v24: copy + release-mutex before the broadcast. Same audit
        // pattern as log.cpp / handle_status / SSEheartbeat — the
        // broadcast walks every SSE subscriber writing TCP, which can
        // block on a wedged client. The mutex isn't needed for the
        // write itself, only the buffer construction.
        // v31.2: stack→BSS. 2.5 KB stack alloc on every loop iteration
        // was 31% of ESP32 loopTask but 50% of ESP8266 cont-task
        // (~4 KB). web_loop is only called from one task (loopTask),
        // so a static buffer here is single-writer/single-reader and
        // safe — no race even though it's now shared across iterations.
        bool doFanout = !firmwareUpdateSub;
        static char localJson[STATUS_JSON_BUFFER_SIZE];
        if (doFanout)
        {
            strncpy(localJson, json, sizeof(localJson) - 1);
            localJson[sizeof(localJson) - 1] = '\0';
        }
        mdnsUpdatePending = true;
        GIVE_MUTEX();

        if (doFanout)
            SSEBroadcastState(localJson);
    }
    else
    {
        GIVE_MUTEX();
    }
    // v43 (audit W34): renamed from `static time_t mdnsDoorUpdateAt` to
    // a bool one-shot flag. The variable was never read as a time; only
    // the truthiness was tested (`!mdnsDoorUpdateAt`) and assigned once
    // when `lastDoorUpdateAt` first became non-zero. The time_t type was
    // misleading.
    static bool mdnsDoorUpdateInit = false;
    if (lastDoorUpdateAt && !mdnsDoorUpdateInit)
    {
        // First time setting it... subsequent changes handled above.
        mdnsDoorUpdateInit = true;
        mdnsUpdatePending = true;
    }
    // Rate limiting - minimum interval between requests
    _millis_t current_time = _millis();
    if (current_time - last_request_time < MIN_REQUEST_INTERVAL_MS)
    {
        return; // Skip this cycle to enforce rate limit
    }

    server.handleClient();
    // Update last request time after handling client
    last_request_time = current_time;
}

void setup_web()
{
    if (web_setup_done)
        return;

    ESP_LOGI(TAG, "=== Starting HTTP web server ===");
#ifndef USE_GDOLIB
    if (!garage_door.active)
    {
        // Garage door should be active by now (will have set door state, etc.)
        // If for some reason it is not, send a get status command (Sec+ 2.0 doors only)
        ESP_LOGI(TAG, "Garage door comms not active yet, sending a getStatus to recover");
        send_get_status();
    }
#endif
    IRAM_START(TAG);
    // IRAM heap is used only for allocating globals, to leave as much regular heap
    // available during operations.  We need to carefully monitor useage so as not
    // to exceed available IRAM.  We can adjust the LOG_BUFFER_SIZE (in log.h) if we
    // need to make more space available for initialization.
#ifndef ESP8266
    // We allocated json as a global block.  We are on dual core CPU.  We need to serialize access to the resource.
    jsonMutex = xSemaphoreCreateMutex();
#endif
    last_reported_paired = homekit_is_paired();

    if (!garage_door.has_motion_sensor && (bool)motionTriggers.bit.motion)
    {
        // If we do not have a motion sensor, disable motion sensing setting
        motionTriggers.bit.motion = 0;
        userConfig->set(cfg_motionTriggers, motionTriggers.asInt);
        ESP8266_SAVE_CONFIG();
    }

    ESP_LOGI(TAG, "Has motion sensor %s, Triggers... motion %d, obstruction %d, light key %d, door key %d, lock key %d (asInt: %d)",
             garage_door.has_motion_sensor ? "true" : "false",
             motionTriggers.bit.motion,
             motionTriggers.bit.obstruction,
             motionTriggers.bit.lightKey,
             motionTriggers.bit.doorKey,
             motionTriggers.bit.lockKey,
             motionTriggers.asInt);
    lastDoorUpdateAt = 0;
    lastDoorOpenAt = 0;
    lastDoorCloseAt = 0;
    lastDoorState = (GarageDoorCurrentState)0xff;

    ESP_LOGI(TAG, "Registering URI handlers");
    server.on("/update", HTTP_POST, handle_update, handle_firmware_upload);
    server.onNotFound(handle_everything);
    // here the list of headers to be recorded
    // Origin/Referer/Host are needed for CSRF guard on /setgdo.
    // v38 (audit W4): X-Forwarded-Host is honored by enforce_same_origin
    // when Host doesn't match — necessary for users behind nginx/Caddy/
    // Tailscale Funnel deployments where the proxy presents the public
    // hostname to the browser but forwards Host as the upstream IP.
    const char *headerkeys[] = {"If-None-Match", "Origin", "Referer", "Host", "X-Forwarded-Host"};
    size_t headerkeyssize = sizeof(headerkeys) / sizeof(char *);
    // ask server to track these headers
    server.collectHeaders(headerkeys, headerkeyssize);
    // log-audit-001: zero SSE slots AND subscriptionCount BEFORE server.begin().
    // Boot-time race: opening the listening socket before subscription[] is
    // INADDR_NONE'd and subscriptionCount is reset can let a browser-side
    // EventSource auto-reconnect land on stale slot state. With browsers
    // holding open clients across reboot, /rest/events/subscribe could fire
    // before this loop ran. Symptom: 24/24 boots showing counter=8
    // (=SSE_MAX_CHANNELS) at first orphan-sweep tick.
    subscriptionCount = 0;
    // initialize all the Server-Sent Events (SSE) slots.
    for (uint32_t i = 0; i < SSE_MAX_CHANNELS; i++)
    {
        subscription[i].SSEconnected = false;
        subscription[i].clientIP = INADDR_NONE;
        subscription[i].clientUUID.clear();
        // v27: explicit init for the orphan-sweep + deferred-remove fields.
        // Without this the struct is left with whatever pattern BSS happened
        // to land on (zero in practice, but the sweep is too sensitive to
        // assume — a non-zero subscribedAt + zero clientIP would still get
        // skipped by the FREE check, but leaving it implicit is brittle).
        subscription[i].pendingRemove = false;
        subscription[i].subscribedAt = 0;
        subscription[i].lastActivity = 0;
        subscription[i].consecutiveBufferFull = 0;  // v47
        subscription[i].firstBufferFullAt = 0;      // log-audit-003
    }
    // Now safe to start accepting HTTP connections.
    server.begin();

    // Initialize connection tracking
    for (int i = 0; i < MAX_CONCURRENT_REQUESTS; i++)
    {
        activeRequests[i].inUse = false;
    }
    activeRequestCount = 0;

    IRAM_END(TAG);

#ifndef ESP8266
    // BOOT-OOM-MDNS: defer the 4-step ratgdo mDNS register burst until
    // free heap recovers above RATGDO_MDNS_HEAP_FLOOR_BYTES (with a
    // 30 s timeout fallback). Polled from web_loop via
    // try_register_ratgdo_mdns(). HomeSpan's _hap registration is on a
    // separate code path and is unaffected.
    ratgdo_mdns_register_pending = true;
    web_setup_done = true;
#else
    if (MDNS.addService("http", "tcp", 80))
    {
        ESP_LOGI(TAG, "Added MDNS service for _http._tcp on port 80");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to add MDNS service for _http._tcp on port 80");
    }

    if (MDNS.addService("ratgdo", "tcp", 80))
    {
        ESP_LOGI(TAG, "Added MDNS service for _ratgdo._tcp on port 80");
        add_static_mdns();
        add_dynamic_mdns();
    }
    else
    {
        ESP_LOGE(TAG, "Failed to add MDNS service for _ratgdo._tcp on port 80");
    }

    web_setup_done = true;
#endif
    return;
}

void handle_notfound()
{
    ESP_LOGD(TAG, "Sending 404 Not Found for: %s with method: %s to client: %s", server.uri().c_str(), http_methods[server.method()], server.client().remoteIP().toString().c_str());
    server.send_P(404, type_txt, response404);
    return;
}

// v39: per-IP recent-auth allowlist. EventSource (browser SSE API) cannot
// participate in Digest challenge/response — it has no API for retrying
// with Authorization headers and the browser's auth-cache replay is
// inconsistent across implementations. So an `AUTHENTICATE()` gate on
// the SSE log subscribe path (added in v37) hard-broke the web-UI live
// log viewer for users with a password set: every EventSource attempt
// got a 401 with no recovery path.
//
// Fix: when AUTHENTICATE() succeeds for ANY handler, record the client
// IP with a 15-minute TTL (v52: bumped from 5 minutes to reduce re-auth
// pressure on the home page's SSE subscribe — users idle on the home
// page for 10+ minutes won't have to re-Digest). The SSE log subscribe
// path then checks the allowlist instead of running Digest itself.
// Browser flow:
//   1. User navigates to a log/admin page  → Digest challenge → enters
//      password → AUTHENTICATE() succeeds → IP recorded.
//   2. Web UI's JS opens EventSource to /rest/events/subscribe?log
//      → server checks IP is in allowlist → allow.
//   3. An attacker on a DIFFERENT LAN IP cannot read the live SSE log
//      stream without first AUTHENTICATE()-ing from that IP.
//
// 4 slots × (IPAddress + uint32_t ttl) is ~80 B BSS — supports up to
// four concurrent auth'd clients. Oldest-evict on overflow. ESP8266
// + ESP32 share the same code; AUTHENTICATE() macro is the only
// arch-specific part.
#define AUTH_ALLOWLIST_SLOTS    4
#define AUTH_ALLOWLIST_TTL_MS   (15UL * 60UL * 1000UL)
struct AuthAllowEntry {
    IPAddress ip;
    uint32_t  expiresMs;
};
static AuthAllowEntry authAllowlist[AUTH_ALLOWLIST_SLOTS] = {};

static void recordAuthSuccess(const IPAddress &ip)
{
    uint32_t now = (uint32_t)_millis();
    uint32_t expires = now + AUTH_ALLOWLIST_TTL_MS;
    int      oldestIdx       = 0;
    uint32_t oldestExpiresMs = authAllowlist[0].expiresMs;
    for (int i = 0; i < AUTH_ALLOWLIST_SLOTS; i++) {
        if (authAllowlist[i].ip == ip) {
            authAllowlist[i].expiresMs = expires;
            return;
        }
        if (authAllowlist[i].expiresMs < oldestExpiresMs) {
            oldestIdx       = i;
            oldestExpiresMs = authAllowlist[i].expiresMs;
        }
    }
    authAllowlist[oldestIdx].ip        = ip;
    authAllowlist[oldestIdx].expiresMs = expires;
}

static bool isAuthAllowedForIP(const IPAddress &ip)
{
    uint32_t now = (uint32_t)_millis();
    for (int i = 0; i < AUTH_ALLOWLIST_SLOTS; i++) {
        if (authAllowlist[i].ip == ip && authAllowlist[i].expiresMs > now) {
            return true;
        }
    }
    return false;
}

#ifdef ESP8266
#define AUTHENTICATE()                                                                                                                      \
    do {                                                                                                                                    \
        if (userConfig->getPasswordRequired()) {                                                                                            \
            if (!server.authenticateDigest(userConfig->getwwwUsername(), userConfig->getwwwCredentials()))                                  \
                return server.requestAuthentication(DIGEST_AUTH, www_realm);                                                                \
            recordAuthSuccess(server.client().remoteIP());                                                                                  \
        }                                                                                                                                   \
    } while (0)
#else
String *ratgdoAuthenticate(HTTPAuthMethod mode, String enteredUsernameOrReq, String extraParams[])
{
    // ESP_LOGI(TAG, "Auth method: %d", mode);                // DIGEST_AUTH
    // ESP_LOGI(TAG, "User: %s", enteredUsernameOrReq);       // Username
    // ESP_LOGI(TAG, "Param 0: %s", extraParams[0].c_str());  // Realm
    // ESP_LOGI(TAG, "Param 1: %s", extraParams[1].c_str());  // URI
    String *pw = new String(read_door_str(nvram_ratgdo_pw, "password").c_str());
    return pw;
}

#define AUTHENTICATE()                                                                          \
    do {                                                                                        \
        if (userConfig->getPasswordRequired()) {                                                \
            if (!server.authenticate(ratgdoAuthenticate))                                       \
                return server.requestAuthentication(DIGEST_AUTH, www_realm);                    \
            recordAuthSuccess(server.client().remoteIP());                                      \
        }                                                                                       \
    } while (0)
#endif

// v54: AUTHENTICATE_OR_ALLOWLIST — fast path for read-only polling
// endpoints (/showlog, /showrebootlog, /crashlog) that the logs.html
// page hits every 3 seconds. Pre-v54 every poll ran full Digest auth
// via AUTHENTICATE(), but arduino-esp32's Digest issues a fresh nonce
// per response and doesn't set `stale=true` on stale-nonce 401, so
// browsers see the cached-nonce response as "rejected" and re-prompt.
// User-visible symptom: /logs.html prompts FAR more often than
// /settings (which only auths once per click) — every 3s polling
// fetch could trigger a prompt.
//
// v54 path: if the client's IP is already in the v37/v39 per-IP
// recent-auth allowlist (stamped by /auth or any other AUTHENTICATE'd
// endpoint within the last AUTH_ALLOWLIST_TTL_MS = 15 min per v52),
// allow the request without running Digest. Cached creds in the
// allowlist replace cached creds in the browser for these specific
// read-only endpoints. Falls back to AUTHENTICATE() if the IP isn't
// allowlisted (first visit, post-reboot, after 15 min idle).
//
// Security model: same as the SSE allowlist gate at handle_subscribe.
// Per-IP. Same-IP attacker (NAT, shared LAN) gets access — but they
// already could via cached browser creds + Digest replay anyway.
// The allowlist is no weaker than what's already exposed; just less
// chatty about it.
//
// Apply ONLY to read-only endpoints. State-changing endpoints
// (/setgdo, /reboot, /reconnectHomeKit, /reset, /refreshHomeKitMDNS,
// /dumpHomeKitState, /setssid, /wifiap, /rescan, /update,
// /clearcrashlog) keep full AUTHENTICATE() for max security.
// /auth itself stays AUTHENTICATE() — that's what stamps the allowlist
// in the first place.
#ifdef ESP8266
#define AUTHENTICATE_OR_ALLOWLIST()                                                             \
    do {                                                                                        \
        if (userConfig->getPasswordRequired()) {                                                \
            if (!isAuthAllowedForIP(server.client().remoteIP())) {                              \
                if (!server.authenticateDigest(userConfig->getwwwUsername(), userConfig->getwwwCredentials())) \
                    return server.requestAuthentication(DIGEST_AUTH, www_realm);                \
                recordAuthSuccess(server.client().remoteIP());                                  \
            }                                                                                   \
        }                                                                                       \
    } while (0)
#else
#define AUTHENTICATE_OR_ALLOWLIST()                                                             \
    do {                                                                                        \
        if (userConfig->getPasswordRequired()) {                                                \
            if (!isAuthAllowedForIP(server.client().remoteIP())) {                              \
                if (!server.authenticate(ratgdoAuthenticate))                                   \
                    return server.requestAuthentication(DIGEST_AUTH, www_realm);                \
                recordAuthSuccess(server.client().remoteIP());                                  \
            }                                                                                   \
        }                                                                                       \
    } while (0)
#endif

// Same-origin / CSRF guard for state-changing endpoints. Parses the
// host out of Origin/Referer (scheme://HOST[:port][/path…]) and
// compares byte-for-byte against the lowercased, port-stripped Host
// header. Bracket-aware IPv6 with RFC 6874 zone-ID strip; rejects
// degenerate `[]`/`[%foo]`. Treats absence of BOTH Origin AND Referer
// as a hard fail. Returns true to proceed, false (with 403 sent) to
// reject. Stack-local char buffers — no heap allocations per POST.

// Writes lowercased host (brackets retained for IPv6) to `out`,
// null-terminated. Returns true on success, false on malformed input
// (`out[0]` set to '\0'). Bracket-aware: IPv6 hosts include the `[…]`,
// RFC 6874 `%eth0`/`%25eth0` zone IDs are stripped. IPv4/hostname
// parsing stops at the first '/', '?', '#', ':'. Empty bracket pairs
// or anything < 3 chars after canonicalization is rejected.
static bool extractHostFromUrl(const char *url, char *out, size_t outSize)
{
    if (!url || !out || outSize < 4) {
        if (out && outSize > 0) out[0] = '\0';
        return false;
    }
    out[0] = '\0';

    const char *p = strstr(url, "://");
    if (!p) return false;
    p += 3;
    if (*p == '\0') return false;

    const char *hostStart = p;
    const char *hostEnd;

    if (*p == '[') {
        // IPv6 literal — find closing bracket, include it.
        const char *closeBracket = strchr(p, ']');
        if (!closeBracket) return false;
        hostEnd = closeBracket + 1;
    } else {
        // IPv4 / hostname — stop at first '/', '?', '#', ':'.
        hostEnd = p;
        while (*hostEnd && *hostEnd != '/' && *hostEnd != '?' &&
               *hostEnd != '#' && *hostEnd != ':') {
            hostEnd++;
        }
    }

    size_t hostLen = (size_t)(hostEnd - hostStart);
    if (hostLen == 0 || hostLen >= outSize) return false;

    // Copy + lowercase in one pass.
    for (size_t i = 0; i < hostLen; i++) {
        char c = hostStart[i];
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    out[hostLen] = '\0';

    // IPv6 canonicalization: strip RFC 6874 zone-ID (%eth0,
    // %25eth0) and fail-close on degenerate '[]', '[%foo]'.
    if (out[0] == '[') {
        char *zone = strchr(out, '%');
        char *closeBracket = strchr(out, ']');
        if (zone && closeBracket && zone < closeBracket) {
            // Replace '%' with ']' and terminate — drops zone-ID
            // and any trailing port portion past the original ']'.
            *zone = ']';
            *(zone + 1) = '\0';
        }
        // Smallest valid IPv6 bracket form is '[::]' (4 chars).
        // Anything < 3 has no usable host inside.
        if (strlen(out) < 3) {
            out[0] = '\0';
            return false;
        }
    }
    return true;
}

static bool enforce_same_origin(const char *uriForLog)
{
    // v31.2 zero-heap path: copy header values into stack
    // buffers and operate on char* throughout. Same behavior
    // as v31.1 (exact-host match, fail-closed on missing
    // headers, IPv6 canonicalization). Eliminates the 6
    // Arduino String allocations per state-changing POST that
    // were the dominant per-request heap churn surface.
    char origin[128]  = {0};
    char referer[128] = {0};
    char myHost[64]   = {0};

    if (server.hasHeader("Origin"))  {
        strncpy(origin, server.header("Origin").c_str(), sizeof(origin) - 1);
    }
    if (server.hasHeader("Referer")) {
        strncpy(referer, server.header("Referer").c_str(), sizeof(referer) - 1);
    }
    if (server.hasHeader("Host"))    {
        strncpy(myHost, server.header("Host").c_str(), sizeof(myHost) - 1);
    }

    // No Host header — nothing to compare against. Hard fail.
    if (myHost[0] == '\0') {
        ESP_LOGW(TAG, "CSRF: rejecting %s — no Host header", uriForLog);
        server.send_P(403, type_txt, PSTR("Forbidden: missing Host"));
        return false;
    }
    // Both Origin and Referer absent: pre-v31 this passed; v31
    // fails it. Browsers always send at least one for state-
    // changing methods.
    if (origin[0] == '\0' && referer[0] == '\0') {
        ESP_LOGW(TAG, "CSRF: rejecting %s — both Origin and Referer absent (Host=%s)",
                 uriForLog, myHost);
        server.send_P(403, type_txt, PSTR("Forbidden: missing Origin/Referer"));
        return false;
    }

    // Lowercase + bracket-aware port-strip the Host header.
    char hostOnly[64] = {0};
    for (size_t i = 0; myHost[i] != '\0' && i < sizeof(hostOnly) - 1; i++) {
        char c = myHost[i];
        hostOnly[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    if (hostOnly[0] == '[') {
        // IPv6 Host: keep [...], drop port after the ']'.
        char *closeBracket = strchr(hostOnly, ']');
        if (closeBracket) *(closeBracket + 1) = '\0';
    } else {
        // IPv4 / hostname: drop port after the ':'.
        char *colon = strchr(hostOnly, ':');
        if (colon) *colon = '\0';
    }

    // Apply the same IPv6 canonicalization to hostOnly so it
    // matches the form extractHostFromUrl produces on the
    // Origin/Referer side (zone-ID strip + degenerate reject).
    if (hostOnly[0] == '[') {
        char *zone = strchr(hostOnly, '%');
        char *closeBracket = strchr(hostOnly, ']');
        if (zone && closeBracket && zone < closeBracket) {
            *zone = ']';
            *(zone + 1) = '\0';
        }
        if (strlen(hostOnly) < 3) {
            ESP_LOGW(TAG, "CSRF: rejecting %s — degenerate Host=%s",
                     uriForLog, myHost);
            server.send_P(403, type_txt, PSTR("Forbidden: degenerate Host"));
            return false;
        }
    }

    // Extract Origin/Referer host and compare exact-match. One shared
    // 64-byte buffer — checks are sequential and extractHostFromUrl
    // zeros out[0] on entry, so reusing the buffer across the two calls
    // is safe (saves 64 B stack/POST vs separate originHost+refererHost).
    char extractedHost[64] = {0};
    bool sameOrigin = false;

    if (origin[0] != '\0' &&
        extractHostFromUrl(origin, extractedHost, sizeof(extractedHost)) &&
        strcmp(extractedHost, hostOnly) == 0) {
        sameOrigin = true;
    }
    if (!sameOrigin && referer[0] != '\0' &&
        extractHostFromUrl(referer, extractedHost, sizeof(extractedHost)) &&
        strcmp(extractedHost, hostOnly) == 0) {
        sameOrigin = true;
    }

    // v38 (audit W4): reverse-proxy fallback. When the device sits behind
    // nginx / Caddy / Traefik / Tailscale Funnel, the browser sees one
    // hostname (the public FQDN) and sends `Origin: https://gdo.example.com`,
    // but the proxy may forward `Host:` as the upstream IP (`192.168.1.10`)
    // depending on its `proxy_set_header` config. Result pre-v38: 403 on
    // every state-changing POST with no diagnostic except the syslog line.
    // Modern proxies set X-Forwarded-Host with the original host the
    // browser requested. If Origin/Referer match THAT, treat it as
    // same-origin. The same-LAN attacker model isn't strengthened —
    // an attacker on the LAN can hit the device directly via Host=device-ip
    // and bypass the proxy entirely; honoring X-Forwarded-Host doesn't
    // change that. Only the first comma-separated value is used (RFC 7239
    // / nginx convention for proxy chains); same lowercase + port-strip
    // pipeline as Host. Diagnostic: the rejection log includes the
    // X-Forwarded-Host value when present, so post-deploy issues are
    // diagnosable.
    char xFwdHost[128] = {0};
    if (!sameOrigin && server.hasHeader("X-Forwarded-Host"))
    {
        strncpy(xFwdHost, server.header("X-Forwarded-Host").c_str(), sizeof(xFwdHost) - 1);
        // Trim to first comma (proxy chains: "first,second,third"
        // — first is the original host the browser used).
        char *comma = strchr(xFwdHost, ',');
        if (comma) *comma = '\0';
        // Lowercase + bracket-aware port-strip, mirroring the Host pipeline above.
        char xfwdOnly[64] = {0};
        for (size_t i = 0; xFwdHost[i] != '\0' && i < sizeof(xfwdOnly) - 1; i++)
        {
            char c = xFwdHost[i];
            xfwdOnly[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
        if (xfwdOnly[0] == '[')
        {
            char *closeBracket = strchr(xfwdOnly, ']');
            if (closeBracket) *(closeBracket + 1) = '\0';
            char *zone = strchr(xfwdOnly, '%');
            if (zone && closeBracket && zone < closeBracket)
            {
                *zone = ']';
                *(zone + 1) = '\0';
            }
        }
        else
        {
            char *colon = strchr(xfwdOnly, ':');
            if (colon) *colon = '\0';
        }
        // Now check Origin/Referer against the proxy-asserted host.
        if (xfwdOnly[0] != '\0')
        {
            if (origin[0] != '\0' &&
                extractHostFromUrl(origin, extractedHost, sizeof(extractedHost)) &&
                strcmp(extractedHost, xfwdOnly) == 0) {
                sameOrigin = true;
            }
            if (!sameOrigin && referer[0] != '\0' &&
                extractHostFromUrl(referer, extractedHost, sizeof(extractedHost)) &&
                strcmp(extractedHost, xfwdOnly) == 0) {
                sameOrigin = true;
            }
            if (sameOrigin) {
                ESP_LOGD(TAG, "CSRF: %s passed via X-Forwarded-Host=%s (Host=%s differed)",
                         uriForLog, xfwdOnly, hostOnly);
            }
        }
    }

    if (!sameOrigin) {
        ESP_LOGW(TAG, "CSRF: rejecting %s — Origin=%s Referer=%s Host=%s X-Forwarded-Host=%s",
                 uriForLog, origin, referer, myHost,
                 xFwdHost[0] ? xFwdHost : "(absent)");
        server.send_P(403, type_txt, PSTR("Forbidden: cross-origin"));
        return false;
    }
    return true;
}

void handle_auth()
{
    AUTHENTICATE();
    server.send_P(200, type_txt, PSTR("Authenticated"));
    return;
}

void handle_reset()
{
    AUTHENTICATE();
    // v28: same-origin guard added — consistent with /setgdo and the
    // other state-changing v23+ POSTs. Pre-v28 a cross-origin page on
    // the same LAN could un-pair the device with a single beacon.
    if (!enforce_same_origin("/reset")) return;
    ESP_LOGI(TAG, "... reset requested");
#ifdef ESP8266
    homekit_storage_reset();
#else
    // v43 (audit W29): homekit_unpair calls homeSpan.processSerialCommand
    // synchronously from the WebServer task, breaking the v31 deferred-flag
    // discipline used by homekit_dump_state / homekit_refresh_mdns /
    // homekit_force_reconnect. The synchronous call is intentional here:
    // sync_and_restart() at the bottom of this handler reboots within
    // ~500 ms (delay below + send completion), so the autoPoll-task race
    // window is bounded by the imminent reboot. Wrapping in a request flag
    // would race the reboot path.
    homekit_unpair();
#endif
    server.client().setNoDelay(true);
    server.send_P(200, type_txt, PSTR("Device has been un-paired from HomeKit. Rebooting...\n"));
    // Allow time to process send() before terminating web server...
    delay(500);
    server.stop();
    sync_and_restart();
    return;
}

void handle_reboot()
{
    // v28: AUTHENTICATE + same-origin guard added. Pre-v28 /reboot was
    // entirely unguarded — a cross-origin page on the same LAN could
    // reboot the device. AUTHENTICATE is a no-op when the user hasn't
    // set a www password, preserving the legacy default-no-password UX;
    // for password-protected installs it now enforces the password,
    // matching /reset and /setgdo behaviour.
    AUTHENTICATE();
    if (!enforce_same_origin("/reboot")) return;
    const char *resp = "Rebooting...\n";
    server.client().setNoDelay(true);
    server.send(200, type_txt, resp);
    // Allow time to process send() before terminating web server...
    delay(500);
    server.stop();
    sync_and_restart();
    return;
}

// User-triggered HomeKit recovery — much less disruptive than /reboot
// when the device is otherwise healthy but the HomeKit hub thinks it's
// "No Response" (stale HAP TCP, mDNS gone stale, controller cache).
// Just cycles WiFi; HomeSpan auto-reattaches on link-up.
//
// Returns 200 immediately with a short text response. The actual cycle
// happens after the response is flushed so the client sees the OK and
// the user gets feedback in the UI before HTTP becomes briefly
// unreachable. Logged via ESP_LOGW with "via web UI" tag for syslog
// visibility.
void handle_reconnect_homekit()
{
    AUTHENTICATE();
    if (!enforce_same_origin("/reconnectHomeKit")) return;
    const char *resp = "HomeKit reconnect triggered. WiFi will cycle in ~1s; expect a brief HTTP outage.\n";
    server.client().setNoDelay(true);
    server.send(200, type_txt, resp);
    // v24: route through the deferred-flag drain in main loop instead
    // of a one-shot Ticker. v23's deferred Ticker still ran in
    // esp_timer task context where the ~750ms WiFi cycle would stall
    // every other Ticker callback (SSE heartbeats, health log).
    homekit_request_reconnect(DEFERRED_REASON_WEB_UI);
    return;
}

// Lighter-touch HomeKit recovery — re-broadcast mDNS without dropping
// WiFi. First thing to try when iOS says "No Response" but the syslog
// shows the device is otherwise healthy. No HTTP outage — just a quick
// HomeSpan database update + mDNS re-advert.
void handle_refresh_mdns()
{
    AUTHENTICATE();
    if (!enforce_same_origin("/refreshHomeKitMDNS")) return;
    const char *resp = "HomeKit mDNS refresh triggered.\n";
    server.client().setNoDelay(true);
    server.send(200, type_txt, resp);
    // v31: defer through main-loop drain. Pre-v31 the WebServer task
    // called homeSpan.updateDatabase(true) directly, which is the
    // audit #7b widening — HomeSpan's autoPoll task is the documented
    // owner of those internals, and re-entrancy from the WebServer
    // task isn't guaranteed.
    homekit_request_refresh_mdns(DEFERRED_REASON_WEB_UI);
    return;
}

// Dumps HomeSpan's full diagnostic CLI output (status + accessory DB +
// operational diagnostics) to the system log / syslog. Read-only —
// useful for debugging "No Response" against device-side state without
// needing a USB serial cable.
//
// v23: gated on the homespanCLI setting. The HomeSpan dump exposes
// pairing controller count and HAP IIDs to syslog (and to anyone who
// can read it), which is information disclosure that the user never
// opted into. Tying it to the existing homespanCLI toggle means this
// only works when the user has explicitly enabled HomeSpan diagnostic
// access for the device.
void handle_dump_homekit_state()
{
    AUTHENTICATE();
    if (!enforce_same_origin("/dumpHomeKitState")) return;
    if (!userConfig->getEnableHomeSpanCLI())
    {
        server.send_P(403, type_txt, PSTR("Forbidden: enable 'HomeSpan CLI' in Settings to use the state dump."));
        return;
    }
    const char *resp = "HomeSpan state dump triggered. Check the System Log / HomeKit tab for output.\n";
    server.client().setNoDelay(true);
    server.send(200, type_txt, resp);
    // v31: defer through main-loop drain. processSerialCommand from the
    // WebServer task is the audit #7b widening — same anti-pattern as
    // updateDatabase. Three CLI commands ('s','i','d') run from the
    // drain in single-threaded loopTask context.
    homekit_request_dump_state(DEFERRED_REASON_WEB_UI);
    return;
}

void load_page(const char *page)
{
    IPAddress clientIP = server.client().remoteIP();

    if ((strlen(page) > 6) && !strcmp(&page[strlen(page) - 6], "js.map"))
    {
        // js.map files, also known as JavaScript source maps, are files that provide a mapping between a minified, transpiled,
        // or bundled JavaScript file and its original, uncompressed source code. The browser only requests this if console/debugger
        // is opened. We do not store these locally (as large) and will redirect the browser to load from our GitHub repo.
        if (!strcmp(gitUser, "ratgdo"))
        {
            // If we are building on ratgdo (for published release) then use tagged URL to make sure map file matches the one embedded in the firmware
            strlcpy(loopTaskScratchBuf512, gitTaggedURL, sizeof(loopTaskScratchBuf512));
        }
        else
        {
            // else we are building for our test purposes, point to the raw URL
            strlcpy(loopTaskScratchBuf512, gitRawURL, sizeof(loopTaskScratchBuf512));
        }
        strlcat(loopTaskScratchBuf512, "/src/www", sizeof(loopTaskScratchBuf512));
        strlcat(loopTaskScratchBuf512, page, sizeof(loopTaskScratchBuf512));
        ESP_LOGD(TAG, "Sending 303 redirect to client %s for: %s", clientIP.toString().c_str(), loopTaskScratchBuf512);
        server.sendHeader(F("Location"), loopTaskScratchBuf512);
        server.send_P(303, type_txt, "", 0);
        return;
    }
    else if (webcontent.count(page) == 0)
        return handle_notfound();

    const unsigned char *data = webcontent.at(page).data;
    int length = webcontent.at(page).length;
    const char *typeP = webcontent.at(page).type;
    const char *crc32 = webcontent.at(page).crc32.c_str();
    // need local copy as strcmp_P cannot take two PSTR()'s
    char type[MAX_MIME_TYPE_LEN];
    strncpy_P(type, typeP, MAX_MIME_TYPE_LEN);

    bool cache = false;
    char cacheHdr[24] = "no-cache, no-store";
    // v43 (audit W37): defensive bump from [8] to [16]. Today's CRC32 ETag
    // is 6 chars (urlsafe-b64 of 4 bytes, `=` padding stripped) so [8]
    // fits, but is one byte from silent truncation if the encoding ever
    // changes (full-base64 with `=` = 9 chars, MD5 hex = 32 chars). [16]
    // comfortably accommodates any reasonable hash format. Zero runtime
    // cost; one-line defensive sizing.
    char matchHdr[16] = "";
    if ((CACHE_CONTROL > 0) &&
        (!strcmp_P(type, type_css) || !strcmp_P(type, type_html) || !strcmp_P(type, type_js) || strstr_P(type, PSTR("image"))))
    {
        snprintf_P(cacheHdr, sizeof(cacheHdr), PSTR("max-age=%d"), CACHE_CONTROL);
        cache = true;
    }
    if (server.hasHeader(F("If-None-Match")))
        strlcpy(matchHdr, server.header(F("If-None-Match")).c_str(), sizeof(matchHdr));

    HTTPMethod method = server.method();
    if (strcmp(crc32, matchHdr))
    {
        server.sendHeader(F("Content-Encoding"), F("gzip"));
        server.sendHeader(F("Cache-Control"), cacheHdr);
        if (cache)
            server.sendHeader(F("ETag"), crc32);
        if (method == HTTP_HEAD)
        {
            ESP_LOGD(TAG, "Client %s requesting: %s (HTTP_HEAD, type: %s)", clientIP.toString().c_str(), page, type);
            server.send_P(200, type, "", 0);
        }
        else
        {
            ESP_LOGD(TAG, "Client %s requesting: %s (HTTP_GET, type: %s, length: %d)", clientIP.toString().c_str(), page, type, length);
            server.send_P(200, type, reinterpret_cast<const char *>(data), length);
        }
    }
    else
    {
        ESP_LOGD(TAG, "Sending 304 not modified to client %s requesting: %s (method: %s, type: %s)", clientIP.toString().c_str(), page, http_methods[method], type);
        server.send_P(304, type, "", 0);
    }
    return;
}

void handle_everything()
{
    // Connection throttling
    if (!registerRequest())
    {
        server.send(503, type_txt, response503);
        ESP_LOGW(TAG, "Reject request, server too busy (handle_everything)");
        return;
    }

    HTTPMethod method = server.method();
    String page = server.uri();
    const char *uri = page.c_str();

    // too verbose... ESP_LOGI(TAG, "Handle everything for %s", uri);
    if (builtInUri.count(uri) > 0)
    {
        // requested page matches one of our built-in handlers
        // v49: demote the polling-noise endpoints (/status.json, SSE
        // subscribe/unsubscribe) from ESP_LOGD to ESP_LOGV so DEBUG output
        // doesn't flood the 16KB on-device ring buffer and wrap user-action
        // lines (force-close ESP_LOGI, auto-close fire ESP_LOGW) before
        // /showlog can capture them. Homebridge polls /status.json at 1Hz
        // steady-state and 1.5Hz during force-close — that alone produced
        // ~120 lines/min at DEBUG, half the buffer per minute. Other
        // endpoints (/setgdo, /reboot, /reconnectHomeKit, /showlog, etc.)
        // stay at LOGD because they fire on user actions and are exactly
        // what you want to see when debugging.
        bool quietPath = (strcmp(uri, "/status.json") == 0
                       || strcmp(uri, "/rest/events/subscribe") == 0
                       || strcmp(uri, "/rest/events/unsubscribe") == 0);
        if (quietPath)
            ESP_LOGV(TAG, "Client %s requesting: %s (method: %s)", server.client().remoteIP().toString().c_str(), uri, http_methods[method]);
        else
            ESP_LOGD(TAG, "Client %s requesting: %s (method: %s)", server.client().remoteIP().toString().c_str(), uri, http_methods[method]);
        if (method == builtInUri.at(uri).first)
        {
            builtInUri.at(uri).second();
        }
        else
        {
            handle_notfound();
        }
        unregisterRequest();
        return;
    }
    else if ((method == HTTP_GET) && (!strncmp_P(uri, restEvents, strlen(restEvents))))
    {
        // Request for "/rest/events/" with a channel number appended
        uri += strlen(restEvents);
        uint32_t channel = atoi(uri);
        if (channel < SSE_MAX_CHANNELS)
        {
            SSEHandler(channel);
        }
        else
        {
            handle_notfound();
        }
        unregisterRequest();
        return;
    }
    else if (method == HTTP_GET || method == HTTP_HEAD)
    {
        // HTTP_GET that does not match a built-in handler
        if (page.equals("/"))
        {
            load_page("/index.html");
        }
        else
        {
            load_page(uri);
        }
        unregisterRequest();
        return;
    }
    // it is a HTTP_POST for unknown URI
    handle_notfound();
    unregisterRequest();
    return;
}

void build_status_json(char *json)
{
    // Build the JSON string
    _millis_t upTime = _millis();
    JSON_START(json);
    JSON_ADD_STR("gitUser", gitUser);
    JSON_ADD_STR("gitRepo", gitRepo);
    // Auto-close (fork addition) — exposes the four config values so the
    // plugin and web UI can show current state. autoClose toggles the
    // feature; the rest define when/how it fires.
    JSON_ADD_BOOL("autoClose", userConfig->getAutoClose());
    JSON_ADD_INT("autoCloseMinutes", userConfig->getAutoCloseMinutes());
    JSON_ADD_INT("autoCloseStartMinutes", userConfig->getAutoCloseStartMinutes());
    JSON_ADD_INT("autoCloseEndMinutes", userConfig->getAutoCloseEndMinutes());
    JSON_ADD_BOOL("autoCloseIgnoreWindow", userConfig->getAutoCloseIgnoreWindow());
    // HomeKit watchdog (fork addition) — toggle + thresholds drive the
    // periodic homekit_health_log diagnostics and (if enabled) recovery.
    JSON_ADD_BOOL("hkAutoRecover", userConfig->getHKAutoRecover());
    JSON_ADD_INT("hkAutoRecoverSecs", userConfig->getHKAutoRecoverSecs());
    JSON_ADD_INT("hkHintQuietSecs", userConfig->getHKHintQuietSecs());
    JSON_ADD_INT("hkHintStaleSecs", userConfig->getHKHintStaleSecs());
    JSON_ADD_INT("hkHintLikelyNRSecs", userConfig->getHKHintLikelyNRSecs());
    JSON_ADD_BOOL("hkVerboseLogs", userConfig->getHKVerboseLogs());
    JSON_ADD_INT("upTime", upTime);
    JSON_ADD_STR(cfg_deviceName, userConfig->getDeviceName());
    JSON_ADD_STR("userName", userConfig->getwwwUsername());
    JSON_ADD_BOOL("paired", homekit_is_paired());
    JSON_ADD_STR("firmwareVersion", std::string(AUTO_VERSION).c_str());
    JSON_ADD_STR(cfg_localIP, userConfig->getLocalIP());
    JSON_ADD_STR(cfg_subnetMask, userConfig->getSubnetMask());
    JSON_ADD_STR(cfg_gatewayIP, userConfig->getGatewayIP());
    JSON_ADD_STR(cfg_nameserverIP, userConfig->getNameserverIP());
    new_ipv4_address = false;
    // v43 (audit W35): copy each Arduino String into a stack buffer before
    // calling .c_str(). The pre-v43 pattern `WiFi.macAddress().c_str()`
    // returned a borrowed pointer into a String temporary that was
    // destroyed at the semicolon — undefined behavior, even though
    // JSON_ADD_STR happens to copy the bytes immediately. The wifiRSSI
    // line additionally allocated 3+ heap chunks per call via `std::to_string
    // + std::to_string + +`. Total heap saved per status.json poll: ~120 B.
    char macStr[18];
    char ssidStr[33];
    char rssiBuf[40];
    char bssidStr[18];
    {
        String s_mac   = WiFi.macAddress();
        String s_ssid  = WiFi.SSID();
        String s_bssid = WiFi.BSSIDstr();
        strncpy(macStr,   s_mac.c_str(),   sizeof(macStr)   - 1); macStr[sizeof(macStr)     - 1] = '\0';
        strncpy(ssidStr,  s_ssid.c_str(),  sizeof(ssidStr)  - 1); ssidStr[sizeof(ssidStr)   - 1] = '\0';
        strncpy(bssidStr, s_bssid.c_str(), sizeof(bssidStr) - 1); bssidStr[sizeof(bssidStr) - 1] = '\0';
    }
    snprintf(rssiBuf, sizeof(rssiBuf), "%d dBm, Channel %d", (int)WiFi.RSSI(), (int)WiFi.channel());
    JSON_ADD_STR("macAddress", macStr);
    JSON_ADD_STR("wifiSSID", ssidStr);
    JSON_ADD_STR("wifiRSSI", rssiBuf);
    JSON_ADD_STR("wifiBSSID", bssidStr);
#ifdef ESP8266
    JSON_ADD_BOOL("lockedAP", wifiConf.bssid_set);
#else
    JSON_ADD_BOOL("lockedAP", false);
#endif
    JSON_ADD_INT("wifiPower", userConfig->getWifiPower());
    JSON_ADD_INT(cfg_GDOSecurityType, (uint32_t)userConfig->getGDOSecurityType());
    JSON_ADD_BOOL("garageSec1Emulated", garage_door.wallPanelEmulated);
    JSON_ADD_STR("garageDoorState", garage_door.active ? DOOR_STATE(garage_door.current_state) : DOOR_STATE(255));
    JSON_ADD_STR("garageLockState", REMOTES_STATE(garage_door.current_lock));
    JSON_ADD_BOOL("garageLightOn", garage_door.light);
    JSON_ADD_BOOL("garageMotion", garage_door.motion);
    JSON_ADD_BOOL("garageObstructed", garage_door.obstructed);
    JSON_ADD_BOOL("pinBasedObst", garage_door.pinModeObstructionSensor);
    JSON_ADD_BOOL(cfg_passwordRequired, userConfig->getPasswordRequired());
    JSON_ADD_INT(cfg_rebootSeconds, (uint32_t)userConfig->getRebootSeconds());
    JSON_ADD_INT("freeHeap", free_heap);
    JSON_ADD_INT("minHeap", min_heap);
    JSON_ADD_INT("crashCount", abs(crashCount));
    JSON_ADD_BOOL(cfg_staticIP, userConfig->getStaticIP());
    JSON_ADD_BOOL(cfg_syslogEn, userConfig->getSyslogEn());
    JSON_ADD_STR(cfg_syslogIP, userConfig->getSyslogIP());
    JSON_ADD_INT(cfg_syslogPort, userConfig->getSyslogPort());
    JSON_ADD_INT(cfg_syslogFacility, userConfig->getSyslogFacility());
    JSON_ADD_INT(cfg_logLevel, userConfig->getLogLevel());
    JSON_ADD_INT(cfg_TTCseconds, userConfig->getTTCseconds());
    JSON_ADD_BOOL(cfg_TTClight, userConfig->getTTClight());
    JSON_ADD_INT(cfg_motionTriggers, (uint32_t)motionTriggers.asInt);
    JSON_ADD_INT(cfg_LEDidle, userConfig->getLEDidle());
    // We send milliseconds relative to current time... ie updated X milliseconds ago
    JSON_ADD_INT(cfg_doorUpdateAt, (upTime - lastDoorUpdateAt));
    JSON_ADD_INT(cfg_doorOpenAt, (upTime - lastDoorOpenAt));
    JSON_ADD_INT(cfg_doorCloseAt, (upTime - lastDoorCloseAt));
    JSON_ADD_BOOL("enableNTP", enableNTP);
    if (enableNTP && (bool)clockSet)
    {
        JSON_ADD_INT("serverTime", time(NULL));
    }
    JSON_ADD_STR(cfg_ntpServer, userConfig->getNTPServer());
    JSON_ADD_STR(cfg_timeZone, userConfig->getTimeZone());
    JSON_ADD_BOOL(cfg_dcOpenClose, userConfig->getDCOpenClose());
    JSON_ADD_BOOL(cfg_dcBypassTTC, userConfig->getDCBypassTTC());
    JSON_ADD_BOOL(cfg_obstFromStatus, userConfig->getObstFromStatus());
    JSON_ADD_INT(cfg_dcDebounceDuration, userConfig->getDCDebounceDuration());
    JSON_ADD_STR("qrPayload", qrPayload);
    if (doorControlType == 2)
    {
        JSON_ADD_INT("batteryState", garage_door.batteryState);
        JSON_ADD_INT("openingsCount", garage_door.openingsCount);
        JSON_ADD_INT(cfg_builtInTTC, userConfig->getBuiltInTTC());
        JSON_ADD_INT("builtInTTCremaining", garage_door.builtInTTCremaining);
        JSON_ADD_BOOL("builtInTTChold", garage_door.builtInTTChold);
        JSON_ADD_BOOL(cfg_useToggle, userConfig->getUseToggle());
    }
    if (garage_door.openDuration)
    {
        JSON_ADD_INT("openDuration", garage_door.openDuration);
        snprintf_P(loopTaskScratchBuf512, sizeof(loopTaskScratchBuf512), PSTR("{ \"max\": %d, \"count\": %d, \"duration\": [ %d, %d, %d, %d, %d, %d ] }"),
                   openHistory.max, openHistory.count,
                   openHistory(1), openHistory(2), openHistory(3), openHistory(4), openHistory(5), openHistory(6));
        JSON_ADD_RAW("openHistory", loopTaskScratchBuf512);
    }
    if (garage_door.closeDuration)
    {
        JSON_ADD_INT("closeDuration", garage_door.closeDuration);
        snprintf_P(loopTaskScratchBuf512, sizeof(loopTaskScratchBuf512), PSTR("{ \"max\": %d, \"count\": %d, \"duration\": [ %d, %d, %d, %d, %d, %d ] }"),
                   closeHistory.max, closeHistory.count,
                   closeHistory(1), closeHistory(2), closeHistory(3), closeHistory(4), closeHistory(5), closeHistory(6));
        JSON_ADD_RAW("closeHistory", loopTaskScratchBuf512);
    }
#ifdef ESP8266
#define accessoryID arduino_homekit_get_running_server() ? arduino_homekit_get_running_server()->accessory_id : "Inactive"
#define clientCount arduino_homekit_get_running_server() ? arduino_homekit_get_running_server()->nfds : 0
    JSON_ADD_STR("accessoryID", accessoryID);
    JSON_ADD_INT("clients", clientCount);
    JSON_ADD_BOOL("lockedAP", wifiConf.bssid_set);
    JSON_ADD_INT("wifiPhyMode", userConfig->getWifiPhyMode());
    JSON_ADD_INT("minStack", ESP.getFreeContStack());
#else
    JSON_ADD_INT(cfg_occupancyDuration, userConfig->getOccupancyDuration());
    JSON_ADD_BOOL(cfg_enableIPv6, userConfig->getEnableIPv6());
    JSON_ADD_STR("ipv6Addresses", ipv6_addresses);
    new_ipv6_address = false;
#ifdef USE_GDOLIB
    JSON_ADD_BOOL(cfg_useSWserial, userConfig->getUseSWserial());
#endif
#ifdef RATGDO32_DISCO
    JSON_ADD_BOOL("distanceSensor", garage_door.has_distance_sensor);
    if (garage_door.has_distance_sensor)
    {
        JSON_ADD_STR("vehicleStatus", vehicleStatus);
        JSON_ADD_INT("vehicleDist", (uint32_t)vehicleDistance);
        last_reported_assist_laser = laser.state();
        JSON_ADD_BOOL("assistLaser", last_reported_assist_laser);
    }
    JSON_ADD_BOOL(cfg_vehicleHomeKit, userConfig->getVehicleHomeKit());
    JSON_ADD_BOOL(cfg_vehicleOccupancyHomeKit, userConfig->getVehicleOccupancyHomeKit());
    JSON_ADD_BOOL(cfg_vehicleArrivingHomeKit, userConfig->getVehicleArrivingHomeKit());
    JSON_ADD_BOOL(cfg_vehicleDepartingHomeKit, userConfig->getVehicleDepartingHomeKit());
    JSON_ADD_INT(cfg_vehicleThreshold, userConfig->getVehicleThreshold());
    JSON_ADD_BOOL(cfg_laserEnabled, userConfig->getLaserEnabled());
    JSON_ADD_BOOL(cfg_laserHomeKit, userConfig->getLaserHomeKit());
    JSON_ADD_INT(cfg_assistDuration, userConfig->getAssistDuration());
#endif
    JSON_ADD_BOOL(cfg_homespanCLI, userConfig->getEnableHomeSpanCLI());
    JSON_ADD_BOOL(cfg_lightHomeKit, userConfig->getLightHomeKit());
    JSON_ADD_BOOL(cfg_motionHomeKit, userConfig->getMotionHomeKit());
    // HK-FC: surface the new toggle + hold-ms so the web UI can render
    // the row state and current value.
    // HK-FC tri-state: 0=off, 1=companion tile, 2=replace primary close.
    // Explicit (int32_t) cast disambiguates the JSON add_int overloads
    // (int matches both int32_t and uint32_t signatures otherwise).
    JSON_ADD_INT(cfg_forceCloseHomeKit,       (int32_t)userConfig->getForceCloseHomeKit());
    JSON_ADD_INT(cfg_forceCloseHoldMs,        userConfig->getForceCloseHoldMs());
    JSON_ADD_BOOL(cfg_forceCloseSingleHold,   userConfig->getForceCloseSingleHold());
    JSON_ADD_INT(cfg_forceCloseHoldMsSingle,  userConfig->getForceCloseHoldMsSingle());
#endif
    JSON_ADD_INT("webRequests", request_count);
    JSON_ADD_INT("webMaxResponseTime", max_response_time);
    JSON_ADD_INT("ttcActive", is_ttc_active());
    JSON_END();
}

#ifndef ESP8266
// BOOT-OOM-MDNS: polled from web_loop while ratgdo_mdns_register_pending.
// Runs the original 4-step ratgdo mDNS registration burst once free heap
// recovers above RATGDO_MDNS_HEAP_FLOOR_BYTES, or unconditionally after
// RATGDO_MDNS_MAX_DEFER_MS as a safety fallback. Idempotent guard
// (cleared on success or timeout) plus the rate-limited deferral log
// keep this from spamming the syslog while we wait.
void try_register_ratgdo_mdns()
{
    // Use _millis_t (int64_t on ESP32) to match the codebase pattern and
    // avoid the 49-day uint32_t wrap concern even though this only runs in
    // the first ~30 s of boot. A bool sentinel for "first call" sidesteps
    // any theoretical zero-value ambiguity.
    static bool firstAttemptStamped = false;
    static _millis_t firstAttemptMs = 0;
    static _millis_t lastDeferLogMs = 0;
    _millis_t nowMs = _millis();
    if (!firstAttemptStamped)
    {
        firstAttemptMs = nowMs;
        firstAttemptStamped = true;
    }
    uint32_t freeHeap = ESP.getFreeHeap();
    _millis_t waitedMs = nowMs - firstAttemptMs;
    bool floorCleared = (freeHeap >= RATGDO_MDNS_HEAP_FLOOR_BYTES);
    bool timedOut = (waitedMs >= (_millis_t)RATGDO_MDNS_MAX_DEFER_MS);
    if (!floorCleared && !timedOut)
    {
        if (lastDeferLogMs == 0 || (nowMs - lastDeferLogMs) >= 5000)
        {
            lastDeferLogMs = nowMs;
            ESP_LOGI(TAG, "ratgdo mDNS deferred: free=%u floor=%u waited_ms=%u",
                     (unsigned)freeHeap,
                     (unsigned)RATGDO_MDNS_HEAP_FLOOR_BYTES,
                     (unsigned)waitedMs);
        }
        return;
    }
    if (floorCleared)
    {
        ESP_LOGI(TAG, "ratgdo mDNS register: floor cleared free=%u after_ms=%u",
                 (unsigned)freeHeap, (unsigned)waitedMs);
    }
    else
    {
        ESP_LOGW(TAG, "ratgdo mDNS register: deferral TIMEOUT free=%u floor=%u -- registering anyway",
                 (unsigned)freeHeap,
                 (unsigned)RATGDO_MDNS_HEAP_FLOOR_BYTES);
    }
    if (MDNS.addService("http", "tcp", 80))
    {
        ESP_LOGI(TAG, "Added MDNS service for _http._tcp on port 80");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to add MDNS service for _http._tcp on port 80");
    }
    if (MDNS.addService("ratgdo", "tcp", 80))
    {
        ESP_LOGI(TAG, "Added MDNS service for _ratgdo._tcp on port 80");
        add_static_mdns();
        add_dynamic_mdns();
    }
    else
    {
        ESP_LOGE(TAG, "Failed to add MDNS service for _ratgdo._tcp on port 80");
    }
    ratgdo_mdns_register_pending = false;
}
#endif

void add_static_mdns()
{
    // Values that do not change during runtime
    ESP_LOGD(TAG, "Adding static mDNS TXT records");
    MDNS.addServiceTxt("ratgdo", "tcp", "model", MODEL_NAME);
    MDNS.addServiceTxt("ratgdo", "tcp", "firmwareVersion", AUTO_VERSION);
    MDNS.addServiceTxt("ratgdo", "tcp", "firmwareDate", __DATE__ " " __TIME__);
    MDNS.addServiceTxt("ratgdo", "tcp", cfg_deviceName, userConfig->getDeviceName());
    MDNS.addServiceTxt("ratgdo", "tcp", "gitRepo", gitRepo);
    MDNS.addServiceTxt("ratgdo", "tcp", "macAddress", WiFi.macAddress().c_str());
    MDNS.addServiceTxt("ratgdo", "tcp", "wifiSSID", WiFi.SSID().c_str());
#ifdef RATGDO32_DISCO
    MDNS.addServiceTxt("ratgdo", "tcp", "distanceSensor", garage_door.has_distance_sensor ? "true" : "false");
#endif
}

void add_dynamic_mdns()
{
    // Values that may change during runtime
    ESP_LOGD(TAG, "Updating dynamic mDNS TXT records");
    _millis_t upTime = _millis();
    MDNS.addServiceTxt("ratgdo", "tcp", "upTime", std::to_string(upTime).c_str());
    MDNS.addServiceTxt("ratgdo", "tcp", "wifiRSSI", std::to_string(WiFi.RSSI()).c_str());
    MDNS.addServiceTxt("ratgdo", "tcp", "wifiChannel", std::to_string(WiFi.channel()).c_str());
    MDNS.addServiceTxt("ratgdo", "tcp", "wifiBSSID", WiFi.BSSIDstr().c_str());
    MDNS.addServiceTxt("ratgdo", "tcp", "paired", homekit_is_paired() ? "true" : "false");
    MDNS.addServiceTxt("ratgdo", "tcp", "garageDoorState", DOOR_STATE(garage_door.current_state));
    MDNS.addServiceTxt("ratgdo", "tcp", "garageLockState", REMOTES_STATE(garage_door.current_lock));
    MDNS.addServiceTxt("ratgdo", "tcp", "garageLightOn", garage_door.light ? "true" : "false");
    MDNS.addServiceTxt("ratgdo", "tcp", "garageMotion", garage_door.motion ? "true" : "false");
    MDNS.addServiceTxt("ratgdo", "tcp", "garageObstructed", garage_door.obstructed ? "true" : "false");
    if (doorControlType == 2)
    {
        MDNS.addServiceTxt("ratgdo", "tcp", "batteryState", std::to_string(garage_door.batteryState).c_str());
        MDNS.addServiceTxt("ratgdo", "tcp", "openingsCount", std::to_string(garage_door.openingsCount).c_str());
        MDNS.addServiceTxt("ratgdo", "tcp", cfg_builtInTTC, std::to_string(userConfig->getBuiltInTTC()).c_str());
    }
    MDNS.addServiceTxt("ratgdo", "tcp", cfg_TTCseconds, std::to_string(userConfig->getTTCseconds()).c_str());
    MDNS.addServiceTxt("ratgdo", "tcp", "openDuration", std::to_string(garage_door.openDuration).c_str());
    MDNS.addServiceTxt("ratgdo", "tcp", "closeDuration", std::to_string(garage_door.closeDuration).c_str());
    MDNS.addServiceTxt("ratgdo", "tcp", cfg_passwordRequired, userConfig->getPasswordRequired() ? "true" : "false");
    // We send milliseconds relative to current time... ie updated X milliseconds ago
    MDNS.addServiceTxt("ratgdo", "tcp", cfg_doorUpdateAt, std::to_string(upTime - lastDoorUpdateAt).c_str());
    MDNS.addServiceTxt("ratgdo", "tcp", cfg_doorOpenAt, std::to_string(upTime - lastDoorOpenAt).c_str());
    MDNS.addServiceTxt("ratgdo", "tcp", cfg_doorCloseAt, std::to_string(upTime - lastDoorCloseAt).c_str());
#ifdef RATGDO32_DISCO
    if (garage_door.has_distance_sensor)
    {
        MDNS.addServiceTxt("ratgdo", "tcp", "vehicleStatus", (const char *)vehicleStatus);
        MDNS.addServiceTxt("ratgdo", "tcp", "vehicleDist", std::to_string((uint32_t)vehicleDistance).c_str());
    }
#endif
    if (enableNTP && (bool)clockSet)
    {
        MDNS.addServiceTxt("ratgdo", "tcp", "serverTime", std::to_string(time(NULL)).c_str());
        MDNS.addServiceTxt("ratgdo", "tcp", "serverTimeStr", (const char *)timeString());
        MDNS.addServiceTxt("ratgdo", "tcp", cfg_timeZone, userConfig->getTimeZone());
    }
#ifdef ESP8266
    MDNS.announce();
#else
    MDNS.setInstanceName(device_name);
#endif
    mdnsUpdatePending = false;
    lastMDNSupdate = _millis();
}

void handle_status()
{
    _millis_t startTime = _millis();
    uint32_t response_time;
    uint32_t build_time;
    static char *json = status_json;
    // v31.2: ESP32 keeps the dedicated static send buffer (avoids
    // the torn-buffer race that v24's mutex-release introduced;
    // BSS cost is invisible vs ~140 KB free heap). ESP8266 holds
    // the mutex across server.send_P instead — its ~30-40 KB free
    // heap can't afford another 2 KB BSS, and the v24 broadcast-
    // stall mitigation isn't load-bearing on ESP8266 (SSE path
    // can't sustain enough concurrent subscribers to expose the
    // deadlock surface anyway).
#ifndef ESP8266
    static char status_json_send[STATUS_JSON_BUFFER_SIZE];
#endif
    size_t jsonLen;

    TAKE_MUTEX();
    request_count++;
    build_status_json(json);
    build_time = (uint32_t)(_millis() - startTime);
    last_reported_garage_door = garage_door;
#ifdef ESP8266
    // Defense-in-depth: belt-and-suspenders if build_status_json ever
    // regresses (buffer overflow truncating before the close brace, or
    // a refactor that loses the terminator). Symmetry with the ESP32
    // branch's explicit `status_json_send[sizeof - 1] = '\0'`. Safe
    // here because we hold the mutex (only writer at a time) and we're
    // writing the last byte of a buffer meant to be a C-string.
    json[STATUS_JSON_BUFFER_SIZE - 1] = '\0';
    jsonLen = strlen(json);
    server.sendHeader(F("Cache-Control"), F("no-cache, no-store"));
    server.send_P(200, type_json, json);
    GIVE_MUTEX();
#else
    strncpy(status_json_send, json, sizeof(status_json_send) - 1);
    status_json_send[sizeof(status_json_send) - 1] = '\0';
    GIVE_MUTEX();
    jsonLen = strlen(status_json_send);
    // MH6 instrumentation: track peak across the health-log window.
    {
        uint32_t prev = __atomic_load_n(&statusJsonPeakLen, __ATOMIC_RELAXED);
        while ((uint32_t)jsonLen > prev &&
               !__atomic_compare_exchange_n(&statusJsonPeakLen, &prev, (uint32_t)jsonLen,
                                            false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    }

    server.sendHeader(F("Cache-Control"), F("no-cache, no-store"));
    server.send_P(200, type_json, status_json_send);
#endif
    response_time = _millis() - startTime;
    max_response_time = std::max(max_response_time, response_time);
    if (jsonLen > STATUS_JSON_BUFFER_SIZE * 95 / 100)
    {
        ESP_LOGW(TAG, "WARNING JSON status: %d is over 95%% of available buffer (%d), build time %lums, response time: %lums", (int)jsonLen, STATUS_JSON_BUFFER_SIZE, build_time, response_time);
    }
    else
    {
        // Demoted from INFO → DEBUG in v22. The Homebridge plugin polls
        // /status.json every 3s by default to drive the GarageDoorOpener
        // tile + Obstruction/Motion sensors, so this line was firing ~20
        // times/min and burying every other log message. The 95%-buffer
        // WARNING above still fires at WARN level — that's the actionable
        // signal. To see these again, set log level to DEBUG in settings.
        // v49: demoted to ESP_LOGV (was ESP_LOGD). Pairs with the /status.json
        // request-line demotion above — homebridge's 1-1.5Hz status polls
        // produced ~120 lines/min of low-value output that wrapped user-
        // action lines out of the 16KB on-device ring buffer.
        ESP_LOGV(TAG, "JSON status: %d (%d%%), build time %lums, response time: %lums", (int)jsonLen, (int)(jsonLen * 100 / STATUS_JSON_BUFFER_SIZE), build_time, response_time);
    }
    return;
}

void handle_logout()
{
    ESP_LOGI(TAG, "Handle logout");
    return server.requestAuthentication(DIGEST_AUTH, www_realm);
}

bool helperResetDoor(const std::string &key, const char *value, configSetting *action)
{
    reset_door();
    return true;
}

bool helperGarageLightOn(const std::string &key, const char *value, configSetting *action)
{
    set_light((atoi(value) == 1) ? true : false);
    return true;
}

bool helperGarageDoorState(const std::string &key, const char *value, configSetting *action)
{
    if (atoi(value) == 1)
        open_door();
    else
        close_door();
    return true;
}

// forceClose=<hold_ms>: simulate a wall-button hold-to-close override.
// Useful when the photo-eye is false-tripping (sun glare) or otherwise
// preventing a normal close — the wall-button hold pattern is the only
// way to override the GDO motor's safety beam check.
//
// SAFETY: only intended for cases where the user has visually verified
// nothing is blocking the door. The plugin/UI calling this is expected
// to surface that warning to the user.
//
// Sec+1.0 only. On Sec+2.0 doorControlType, this falls back to a normal
// close (no protocol-level hold pattern exists).
bool helperForceClose(const std::string &key, const char *value, configSetting *action)
{
    // v43 (audit W23): drop the helper-side clamp. door_command_force_close
    // (comms.cpp:2832-2833) is the single source of truth for hold_ms bounds
    // (`<1000 → 3500`, `>10000 → 10000`). Pre-v43 the helper also clamped
    // `<= 0 → 3500` but ignored the upper bound, so dual-validation invited
    // future drift between the two clamps.
    door_command_force_close((uint32_t)atoi(value));
    return true;
}

bool helperGarageLockState(const std::string &key, const char *value, configSetting *action)
{
    set_lock((atoi(value) == 1) ? 1 : 0);
    return true;
}

bool helperCredentials(const std::string &key, const char *value, configSetting *action)
{
    const char *newUsername = strstr(value, "username");
    const char *newCredentials = strstr(value, "credentials");
    const char *newPassword = strstr(value, "password");
    if (!(newUsername && newCredentials && newPassword))
        return false;

    // JSON string passed in.
    // Very basic parsing, not using library functions to save memory
    // find the colon after the key string
    newUsername = strchr(newUsername, ':') + 1;
    newCredentials = strchr(newCredentials, ':') + 1;
    newPassword = strchr(newPassword, ':') + 1;
    // for strings find the double quote
    newUsername = strchr(newUsername, '"') + 1;
    newCredentials = strchr(newCredentials, '"') + 1;
    newPassword = strchr(newPassword, '"') + 1;
    // null terminate the strings (at closing quote).
    *strchr(newUsername, '"') = (char)0;
    *strchr(newCredentials, '"') = (char)0;
    *strchr(newPassword, '"') = (char)0;
    // save values...
    ESP_LOGI(TAG, "Set credentials for user: %s", newUsername);
    userConfig->set(cfg_wwwUsername, newUsername);
    userConfig->set(cfg_wwwCredentials, newCredentials);
#ifndef ESP8266
    // We only need to save password (distinct from credentials) on ESP32
    write_door_str(nvram_ratgdo_pw, newPassword);
#endif
    ESP8266_SAVE_CONFIG();
    return true;
}

bool helperUpdateUnderway(const std::string &key, const char *value, configSetting *action)
{
    firmwareSize = 0;
    firmwareUpdateSub = NULL;
    const char *md5 = strstr(value, "md5");
    const char *size = strstr(value, "size");
    const char *uuid = strstr(value, "uuid");

    if (!(md5 && size && uuid))
        return false;

    // JSON string of passed in.
    // Very basic parsing, not using library functions to save memory
    // find the colon after the key string
    md5 = strchr(md5, ':') + 1;
    size = strchr(size, ':') + 1;
    uuid = strchr(uuid, ':') + 1;
    // for strings find the double quote
    md5 = strchr(md5, '"') + 1;
    uuid = strchr(uuid, '"') + 1;
    // null terminate the strings (at closing quote).
    *strchr(md5, '"') = (char)0;
    *strchr(uuid, '"') = (char)0;
    // ESP_LOGI(TAG,"MD5: %s, UUID: %s, Size: %d", md5, uuid, atoi(size));
    // save values...
    strlcpy(firmwareMD5, md5, sizeof(firmwareMD5));
    firmwareSize = atoi(size);
    for (uint32_t channel = 0; channel < SSE_MAX_CHANNELS; channel++)
    {
        if (subscription[channel].SSEconnected && subscription[channel].clientUUID == uuid && subscription[channel].client.connected())
        {
            firmwareUpdateSub = &subscription[channel];
            break;
        }
    }
    return true;
}

bool helperFactoryReset(const std::string &key, const char *value, configSetting *action)
{
#ifdef ESP8266
    userConfig->erase();
    reset_door();
    WiFi.disconnect();
    ESP.eraseConfig();
    sync_and_restart();
#else
    ESP_LOGI(TAG, "System boot time: %s", timeString(lastRebootAt));
    ESP_LOGI(TAG, "Factory reset at: %s", timeString());
    erase_door_data();
    reset_door();
    // R-?-fork: HomeSpan's `processSerialCommand("F")` erases all NVS
    // partitions and calls reboot() internally — the autoPoll-task race
    // window (pollMutex held by pollTask) collapses to the few ms before
    // reboot() resets the chip. Same imminent-reboot justification as
    // homekit_unpair (see v43/W29 comment at handle_reset call site).
    // No `homeSpan.getMutex()` lock taken here for the same reason as
    // homekit_dump_state: pollTask iterations can take seconds, waiting
    // on the mutex from loopTask could trip the watchdog.
    homeSpan.processSerialCommand("F");
#endif
    return true;
}

void factoryReset()
{
    helperFactoryReset("", "", nullptr);
}

#ifdef RATGDO32_DISCO
bool helperAssistLaser(const std::string &key, const char *value, configSetting *action)
{
    if (atoi(value) == 1)
        laser.on();
    else
        laser.off();
    notify_homekit_laser(atoi(value) == 1);
    return true;
}
#endif

void handle_setgdo()
{
    // Build-in handlers that do not set a configuration value, or if they do they set multiple values.
    // key, {reboot, wifiChanged, value, fn to call}
    static const std::unordered_map<std::string, configSetting> setGDOhandlers = {
        {PSTR("resetDoor"), {true, false, 0, helperResetDoor}},
        {PSTR("garageLightOn"), {false, false, 0, helperGarageLightOn}},
        {PSTR("garageDoorState"), {false, false, 0, helperGarageDoorState}},
        {PSTR("forceClose"), {false, false, 0, helperForceClose}},
        {PSTR("garageLockState"), {false, false, 0, helperGarageLockState}},
        {PSTR("credentials"), {false, false, 0, helperCredentials}}, // parse out wwwUsername and credentials
        {PSTR("updateUnderway"), {false, false, 0, helperUpdateUnderway}},
        {PSTR("factoryReset"), {true, false, 0, helperFactoryReset}},
#ifdef RATGDO32_DISCO
        {PSTR("assistLaser"), {false, false, 0, helperAssistLaser}},
#endif
    };
    bool reboot = false;
    bool error = false;
    bool wifiChanged = false;
    bool saveSettings = false;
    configSetting actions;

    if (!((server.args() == 1) && (server.argName(0) == cfg_timeZone)))
    {
        // We will allow setting of time zone without authentication
        AUTHENTICATE();
    }

    // v23: CSRF guard pulled into enforce_same_origin() (above) so the
    // 4 state-changing endpoints share one implementation.
    if (!enforce_same_origin("/setgdo")) return;

    // Loop over all the GDO settings passed in...
    for (int i = 0; i < server.args(); i++)
    {
        std::string key(server.argName(i).c_str());
        std::string value(server.arg(i).c_str());

        // Clamp known integer-ranged auto-close keys server-side. The web UI
        // already enforces these bounds, but a hand-crafted POST can bypass
        // them; defending here keeps NVRAM from accepting nonsense like
        // autoCloseMinutes=99999999 or autoCloseStartMinutes=-1.
        // v43 (audit W27): bounds pulled from config.h constants —
        // AUTO_CLOSE_MAX_MINUTES, AUTO_CLOSE_MAX_TOD_MIN.
        if (key == "autoCloseMinutes" ||
            key == "autoCloseStartMinutes" ||
            key == "autoCloseEndMinutes")
        {
            long n = strtol(value.c_str(), nullptr, 10);
            long lo = (key == "autoCloseMinutes") ? 1 : 0;
            long hi = (key == "autoCloseMinutes") ? (long)AUTO_CLOSE_MAX_MINUTES : (long)AUTO_CLOSE_MAX_TOD_MIN;
            if (n < lo) n = lo;
            if (n > hi) n = hi;
            value = std::to_string(n);
        }
        // v23: same defensive clamp for HomeKit watchdog timer keys.
        // Without this, a hand-crafted POST hkAutoRecoverSecs=0 (or any
        // value < HK_WATCHDOG_MIN_SECS) would make the watchdog auto-fire
        // on every health tick → WiFi cycles every 3 minutes forever →
        // device unreachable by HomeKit until manual settings rescue.
        // v43 (audit W27): bounds pulled from config.h constants —
        // HK_WATCHDOG_MIN_SECS, HK_WATCHDOG_MAX_SECS. Range matches the
        // form bounds in src/www/index.html (mirrored — see config.h note).
        if (key == "hkAutoRecoverSecs" ||
            key == "hkHintQuietSecs" ||
            key == "hkHintStaleSecs" ||
            key == "hkHintLikelyNRSecs")
        {
            long n = strtol(value.c_str(), nullptr, 10);
            if (n < (long)HK_WATCHDOG_MIN_SECS) n = (long)HK_WATCHDOG_MIN_SECS;
            if (n > (long)HK_WATCHDOG_MAX_SECS) n = (long)HK_WATCHDOG_MAX_SECS;
            value = std::to_string(n);
        }
        // HK-FC: clamp force-close press-hold to [1000, 10000] to match
        // door_command_force_close's bounds (comms.cpp:2880-2881 single
        // source of truth). Same defensive pattern as the auto-close /
        // watchdog clamps above — a hand-crafted POST forceCloseHoldMs=99999
        // would otherwise hold the relay way longer than the motor's
        // override window expects.
        if (key == "forceCloseHoldMs")
        {
            long n = strtol(value.c_str(), nullptr, 10);
            if (n < 1000)  n = 1000;
            if (n > 10000) n = 10000;
            value = std::to_string(n);
        }

        if (setGDOhandlers.count(key))
        {
            if (key == "credentials")
                ESP_LOGI(TAG, "Call SetGDO handler for Key: %s", key.c_str());
            else
                ESP_LOGI(TAG, "Call SetGDO handler for Key: %s, Value: %s", key.c_str(), value.c_str());
            actions = setGDOhandlers.at(key);
            if (actions.fn)
            {
                error = error || !actions.fn(key, value.c_str(), &actions);
            }
            reboot = reboot || actions.reboot;
            wifiChanged = wifiChanged || actions.wifiChanged;
        }
        else if (userConfig->contains(key))
        {
            ESP_LOGI(TAG, "Configuration set for Key: %s, Value: %s", key.c_str(), value.c_str());
            actions = userConfig->getDetail(key);
            if (actions.fn)
            {
                // Value will be set within called function
                error = error || !actions.fn(key, value.c_str(), &actions);
            }
            else
            {
                // No function to call, set value directly.
                userConfig->set(key, value.c_str());
            }
            reboot = reboot || actions.reboot;
            wifiChanged = wifiChanged || actions.wifiChanged;
            saveSettings = true;
        }
        else
        {
            ESP_LOGW(TAG, "Invalid Key: %s, Value: %s (F)", key.c_str(), value.c_str());
            error = true;
        }
        YIELD(); // Yield while looping over all received settings, just-in-case!
        if (error)
            break;
    }

    ESP_LOGV(TAG, "SetGDO Complete");

    if (error)
    {
        // Simple error handling...
        ESP_LOGE(TAG, "Sending %s, for: %s", response400invalid, server.uri().c_str());
        server.send_P(400, type_txt, response400invalid);
        return;
    }

    if (saveSettings)
    {
        userConfig->set(cfg_wifiChanged, wifiChanged);
        ESP8266_SAVE_CONFIG();
        // v40 (audit W26): enforce ordering Quiet < Stale < LikelyNR on the
        // HomeKit hint thresholds before refreshing the watchdog config.
        // The cascade in homekit_health_log evaluates LikelyNR > Stale >
        // Quiet to assign hint level 3 / 2 / 1; out-of-order values let
        // the user input cross-fire (e.g. Quiet=1000, Stale=500, LikelyNR=300
        // makes every idle tick jump straight to level-3 Silent and the
        // intermediate hints never fire). The per-key clamp pass above
        // enforces [60, 7200] bounds; this enforces ORDER. Auto-fix to
        // the user's intent: re-sort the three values ascending so the
        // smallest becomes Quiet, middle becomes Stale, largest becomes
        // LikelyNR. Logged at WARN if a fix was needed so the user can
        // see their input got reordered.
        {
            uint32_t q  = userConfig->getHKHintQuietSecs();
            uint32_t s  = userConfig->getHKHintStaleSecs();
            uint32_t nr = userConfig->getHKHintLikelyNRSecs();
            if (!(q < s && s < nr))
            {
                uint32_t sorted[3] = {q, s, nr};
                // Tiny 3-element insertion sort, no <algorithm>/<utility>
                // header dependency. uint32_t swap via a temp is fine here.
                #define WEB_SWAP_U32(a, b) do { uint32_t tmp__ = (a); (a) = (b); (b) = tmp__; } while (0)
                if (sorted[0] > sorted[1]) WEB_SWAP_U32(sorted[0], sorted[1]);
                if (sorted[1] > sorted[2]) WEB_SWAP_U32(sorted[1], sorted[2]);
                if (sorted[0] > sorted[1]) WEB_SWAP_U32(sorted[0], sorted[1]);
                #undef WEB_SWAP_U32
                ESP_LOGW(TAG, "HomeKit hint thresholds out of order (Quiet=%u Stale=%u LikelyNR=%u); auto-resorting to %u/%u/%u",
                         (unsigned)q, (unsigned)s, (unsigned)nr,
                         (unsigned)sorted[0], (unsigned)sorted[1], (unsigned)sorted[2]);
                userConfig->set(cfg_hkHintQuietSecs,    (int)sorted[0]);
                userConfig->set(cfg_hkHintStaleSecs,    (int)sorted[1]);
                userConfig->set(cfg_hkHintLikelyNRSecs, (int)sorted[2]);
            }
        }
        // v22 hooks — after settings save, re-arm anything that caches
        // a config value at boot. Cheap (refreshing a handful of static
        // vars / setting a flag) and only runs when the user actually
        // changes settings. v23 routes the auto-close reschedule via
        // the deferred-flag drain so it happens on the main loop instead
        // of racing the Ticker callback's own self-reschedule.
        homekit_refresh_watchdog_config();
        comms_refresh_auto_close_config(); // v31: cache for Ticker-safe checkAutoClose reads
        request_auto_close_reschedule();
    }
    if (reboot)
    {
        // Some settings require reboot to take effect
        server.send_P(200, type_html, PSTR("<p>Success. Reboot.</p>"));
        // Allow time to process send() before terminating web server...
        delay(500);
        server.stop();
        sync_and_restart();
    }
    else
    {
        server.send_P(200, type_html, PSTR("<p>Success.</p>"));
    }
    return;
}

void removeSSEsubscription(SSESubscription *s)
{
    if (subscriptionCount > 0)
        subscriptionCount--; // Prevent negative count
    // detach heartbeatTimer: SSE subscription is being torn down — kill the
    // per-subscription heartbeat ticker. Caller must NOT be running inside
    // the heartbeatTimer callback itself (use pendingRemove flag for that
    // path; see v22 self-detach crash note at line ~238). Distinct from
    // TTCtimer; no force-close interaction.
    s->heartbeatTimer.detach();
    ESP_LOGD(TAG, "Remove SSE subscription. Total subscribed: %d", subscriptionCount);
    s->client.stop();
    s->clientIP = INADDR_NONE;
    s->clientUUID.clear();
    s->SSEconnected = false;
    s->pendingRemove = false;
    // v27: reset orphan-sweep timestamps so a freshly freed slot doesn't
    // get reaped on its next allocation if the new client subscribes
    // faster than _millis ticks. Both are cheap writes.
    s->subscribedAt = 0;
    s->lastActivity = 0;
    s->consecutiveBufferFull = 0;  // v47
    s->firstBufferFullAt = 0;      // log-audit-003
}

// v22: drain SSESubscription entries flagged pendingRemove during a
// Ticker callback (where calling Ticker.detach() on the still-running
// Ticker would crash in vTaskDelete → uxListRemove). Called every main
// loop tick from service_timer_loop in ratgdo.cpp. Cheap when nothing
// is pending — just an array scan.
void process_sse_pending_removes()
{
    for (uint32_t i = 0; i < SSE_MAX_CHANNELS; i++)
    {
        if (subscription[i].pendingRemove)
        {
            removeSSEsubscription(&subscription[i]);
        }
    }
}

// v27: orphan-slot sweep. Runs from service_timer_loop BEFORE
// process_sse_pending_removes so any slot we flag here is reaped
// the same tick. Three classes of orphans:
//   5a) Subscribed but EventSource never opened (pre-handshake leak).
//       Pre-v27 this was the dominant leak — logs.html with heartbeat=0
//       had no Ticker to fire SSEheartbeat, so the SSEfailCount
//       fast-path never ran. After SSE_PREHANDSHAKE_TIMEOUT_MS the
//       browser is presumed gone.
//   5b) Connected but TCP socket has dropped without us noticing.
//       client.connected() now returns false but no broadcast/heartbeat
//       has noticed yet (e.g. client subscribed but never sees broadcast
//       traffic during a quiet period).
//   5c) Connected and TCP still up, but truly idle for SSE_IDLE_TIMEOUT_MS.
//       Belt-and-suspenders for a wedged-but-not-yet-RST socket. A
//       healthy heartbeat>=30s client never trips this; logs.html with
//       v27 heartbeat=10 also won't trip (broadcast+heartbeat keep
//       lastActivity fresh).
// Also reconciles subscriptionCount: if it desyncs from the actual slot
// count (which it has been doing because handle_subscribe used to
// pre-increment before all rejection paths exhausted), log + correct.
void sweep_sse_orphans()
{
    // Skew-detection guard: writers (Ticker SSEheartbeat, BUFFER_FULL
    // stamps from SSEBroadcastState) update subscribedAt/lastActivity
    // mid-loop. `int32_t skew = (int32_t)(stamp - now)` is positive iff
    // the writer's stamp is newer than our snapshot — skip and let the
    // next tick handle it. Otherwise compute unsigned `age = now - stamp`
    // (mod-2^32 wrap-safe at any age).
    uint32_t now = (uint32_t)_millis();
    uint32_t currentlyAlloc = 0;
    uint32_t reapedThisTick = 0;
    for (uint32_t i = 0; i < SSE_MAX_CHANNELS; i++)
    {
        SSESubscription &s = subscription[i];
        // FREE — nothing to do.
        if (s.clientIP == IPAddress(INADDR_NONE))
            continue;
        // Already flagged — process_sse_pending_removes will pick it up.
        if (s.pendingRemove)
        {
            currentlyAlloc++;
            continue;
        }
        currentlyAlloc++;
        // 5a) pre-handshake abandoned
        if (!s.SSEconnected && s.subscribedAt != 0)
        {
            uint32_t stamp = s.subscribedAt;          // local snapshot (volatile)
            int32_t skew = (int32_t)(stamp - now);    // > 0 iff writer raced our `now`
            if (skew <= 0)
            {
                uint32_t age = now - stamp;           // unsigned, wrap-safe
                if (age > SSE_PREHANDSHAKE_TIMEOUT_MS)
                {
                    ESP_LOGW(TAG, "SSE orphan (pre-handshake) channel=%u uuid=%s ip=%s age=%ums — reaping",
                             (unsigned)i, s.clientUUID.c_str(),
                             s.clientIP.toString().c_str(),
                             (unsigned)age);
                    s.pendingRemove = true;
                    __atomic_fetch_add(&sseOrphansReaped, 1, __ATOMIC_RELAXED);
                    reapedThisTick++;
                    continue;
                }
            }
        }
        // 5b) connected but TCP gone
        if (s.SSEconnected && !s.client.connected())
        {
            ESP_LOGW(TAG, "SSE orphan (TCP dropped) channel=%u uuid=%s ip=%s — reaping",
                     (unsigned)i, s.clientUUID.c_str(),
                     s.clientIP.toString().c_str());
            s.pendingRemove = true;
            __atomic_fetch_add(&sseOrphansReaped, 1, __ATOMIC_RELAXED);
            reapedThisTick++;
            continue;
        }
        // 5d) wedged on flow control — v47. After SSE_MAX_CONSECUTIVE_BUFFER_FULL
        // successive availableForWrite-rejects with no successful OK in between,
        // the peer is alive at the kernel level (otherwise 5b or keepalive would
        // have caught it) but not draining at the application level. Industry
        // pattern for Mercure / nginx SSE proxies / sse-pubsub.
        //
        // EXCEPTION: skip the OTA slot. helperUpdateUnderway points
        // firmwareUpdateSub at one of these slots; OTA progress chunks are large
        // enough to legitimately hit BUFFER_FULL during a slow upload tail. The
        // existing v22 deferred-cleanup discipline + the OTA's own UPLOAD_FILE_END
        // / UPLOAD_FILE_ABORTED handlers clear firmwareUpdateSub — by the time
        // this exception lapses, the OTA has finished.
        if (s.SSEconnected && &s != firmwareUpdateSub)
        {
            uint32_t cbf = __atomic_load_n(&s.consecutiveBufferFull, __ATOMIC_RELAXED);
            if (cbf >= SSE_MAX_CONSECUTIVE_BUFFER_FULL)
            {
                // log-audit-003: report how long the connection sat wedged
                // (delta since the first BUFFER_FULL of the streak). Lets
                // future regressions in the v47 wedged-detection — e.g.
                // BUFFER_FULL streaks completing in <1s indicating a
                // misclassified flow-control event — be observable from
                // logs alone.
                uint32_t fbfa = __atomic_load_n(&s.firstBufferFullAt, __ATOMIC_RELAXED);
                uint32_t wedgedFor = (fbfa != 0) ? (now - fbfa) : 0;  // unsigned-safe across rollover
                ESP_LOGW(TAG, "SSE orphan (wedged on flow-control) channel=%u uuid=%s ip=%s consecutive=%u wedgedFor=%ums — reaping",
                         (unsigned)i, s.clientUUID.c_str(),
                         s.clientIP.toString().c_str(),
                         (unsigned)cbf,
                         (unsigned)wedgedFor);

                // log-audit-003: stamp this UUID into the recent-reap
                // dampener. handle_subscribe will reject re-subscribes of
                // the same UUID for SSE_DAMPENER_WINDOW_MS. Find an empty
                // slot first; if all 8 are full, overwrite the oldest
                // (which by construction will already be near the 60s
                // boundary — the dampener-window aging in handle_subscribe
                // doesn't run from here).
                if (s.clientUUID.length() > 0)
                {
                    int target = -1;
                    uint32_t oldestAge = 0;
                    for (int r = 0; r < SSE_MAX_CHANNELS; r++)
                    {
                        if (recentReaps[r].reapedAt == 0 || recentReaps[r].uuid[0] == '\0')
                        {
                            target = r;
                            break;
                        }
                        uint32_t age = now - recentReaps[r].reapedAt;  // unsigned-safe
                        if (age >= oldestAge)
                        {
                            oldestAge = age;
                            target = r;
                        }
                    }
                    if (target >= 0)
                    {
                        strlcpy(recentReaps[target].uuid, s.clientUUID.c_str(), sizeof(recentReaps[target].uuid));
                        recentReaps[target].reapedAt = now;
                    }
                }

                s.pendingRemove = true;
                __atomic_fetch_add(&sseOrphansReaped, 1, __ATOMIC_RELAXED);
                reapedThisTick++;
                continue;
            }
        }
        // 5c) idle past the watchdog
        if (s.SSEconnected && s.lastActivity != 0)
        {
            uint32_t stamp = s.lastActivity;          // local snapshot (volatile)
            int32_t skew = (int32_t)(stamp - now);    // > 0 iff writer raced our `now`
            if (skew <= 0)
            {
                uint32_t age = now - stamp;           // unsigned, wrap-safe
                if (age > SSE_IDLE_TIMEOUT_MS)
                {
                    ESP_LOGI(TAG, "SSE orphan (idle) channel=%u uuid=%s ip=%s idle=%ums — reaping",
                             (unsigned)i, s.clientUUID.c_str(),
                             s.clientIP.toString().c_str(),
                             (unsigned)age);
                    s.pendingRemove = true;
                    __atomic_fetch_add(&sseOrphansReaped, 1, __ATOMIC_RELAXED);
                    reapedThisTick++;
                    continue;
                }
            }
        }
    }
    sseSlotsAlloc = currentlyAlloc;
    // F3 reconciliation: subscriptionCount used to drift on the rejection
    // paths in handle_subscribe (counter incremented before all rejection
    // checks ran). If we see a mismatch here, the counter is wrong, the
    // slots are right — assign + warn.
    if (currentlyAlloc != subscriptionCount)
    {
        ESP_LOGW(TAG, "SSE subscriptionCount desync: counter=%u actual=%u — reconciling",
                 (unsigned)subscriptionCount, (unsigned)currentlyAlloc);
        subscriptionCount = currentlyAlloc;
    }
    (void)reapedThisTick;
}

void SSEheartbeat(SSESubscription *s)
{
    if (!s)
        return;

    // Skip slots already marked free (clientIP == INADDR_NONE).
    if (s->clientIP == IPAddress(INADDR_NONE))
        return;

    if (!(s->SSEconnected))
    {
        if (s->SSEfailCount++ >= 5)
        {
            // v22: defer the remove to main-loop context — calling
            // removeSSEsubscription here would Ticker.detach() the very
            // Ticker that's running this callback, crashing in uxListRemove.
            ESP_LOGD(TAG, "Client %s (%s) >5 heartbeat fails, marking for deferred remove", s->clientIP.toString().c_str(), s->clientUUID.c_str());
            s->pendingRemove = true;
        }
        else
        {
            ESP_LOGE(TAG, "Client %s (%s) not yet listening for SSE", s->clientIP.toString().c_str(), s->clientUUID.c_str());
        }
        return;
    }

    if (s->client.connected())
    {
        static int8_t lastRSSI = 0;
        static char *json = status_json;
        TAKE_MUTEX();
        JSON_START(json);
        JSON_ADD_INT("upTime", _millis());
        JSON_ADD_INT("freeHeap", free_heap);
        JSON_ADD_INT("minHeap", min_heap);
        // TODO monitor stack... JSON_ADD_INT("minStack", ESP.getFreeContStack());
#ifdef RATGDO32_DISCO
        static int32_t lastVehicleDistance = 0;
        if (garage_door.has_distance_sensor && (lastVehicleDistance != vehicleDistance))
        {
            lastVehicleDistance = vehicleDistance;
            JSON_ADD_INT("vehicleDist", (uint32_t)vehicleDistance);
        }
#endif
        if (lastRSSI != WiFi.RSSI())
        {
            lastRSSI = WiFi.RSSI();
            // v43 (audit W35): replace `(std::to_string + std::to_string +
            // " dBm, ...").c_str()` 3-allocation chain with one snprintf
            // into a stack buffer. SSEheartbeat fires per-subscriber per
            // ~10s; eliminating heap from this hot path cuts long-term
            // fragmentation pressure.
            char rssiBuf[40];
            snprintf(rssiBuf, sizeof(rssiBuf), "%d dBm, Channel %u", (int)lastRSSI, WiFi.channel());
            JSON_ADD_STR("wifiRSSI", rssiBuf);
        }
#ifdef ESP8266
        static int lastClientCount = 0;
        if (arduino_homekit_get_running_server() && arduino_homekit_get_running_server()->nfds != lastClientCount)
        {
            lastClientCount = arduino_homekit_get_running_server()->nfds;
            JSON_ADD_INT("clients", lastClientCount);
        }
#endif
        JSON_END();
        JSON_REMOVE_NL(json);
        // v24: copy the formatted SSE event to a local stack buffer
        // BEFORE releasing the mutex (because loopTaskScratchBuf512 is shared
        // between heartbeat callers and could be overwritten by another
        // SSE channel firing simultaneously). Then release the mutex
        // and write — clientWrite is now bounded by SO_SNDTIMEO and
        // skips full TCP buffers via availableForWrite.
        char localBuf[sizeof(loopTaskScratchBuf512)];
        snprintf_P(localBuf, sizeof(localBuf), PSTR("event: message\ndata: %s\n\n"), json);
        GIVE_MUTEX();
        // v27: capture clientWrite return so we can stamp lastActivity
        // only on a successful write. Failed writes already mark the
        // socket via clientWrite's internal stop()/skip path; bumping
        // lastActivity on a failure would mask the orphan sweep's
        // idle-detection (5c) for up to SSE_IDLE_TIMEOUT_MS.
        // v29: use clientWriteEx tri-state. Stamp on OK or BUFFER_FULL
        // (both = "broadcast loop reached this slot and tried"; peer is
        // alive enough to keep). Only FAILED skips the stamp — that's
        // the real wedge signal where lwIP rejected bytes for delivery.
        // v47: also reset/increment consecutiveBufferFull for sweep 5d.
        SseWriteResult r = clientWriteEx(s->client, localBuf);
        if (r == SseWriteResult::OK)
        {
            s->lastActivity = (uint32_t)_millis();
            __atomic_store_n(&s->consecutiveBufferFull, 0, __ATOMIC_RELAXED);  // v47: reset on real drain
            __atomic_store_n(&s->firstBufferFullAt, 0, __ATOMIC_RELAXED);      // log-audit-003: clear streak start
        }
        else if (r == SseWriteResult::BUFFER_FULL)
        {
            s->lastActivity = (uint32_t)_millis();  // unchanged: BUFFER_FULL still stamps activity (v29)
            // log-audit-003: stamp streak-start on the 0->1 transition.
            // fetch_add returns the previous value; if 0 we just started a
            // streak. Multi-writer: a racing writer may overwrite our stamp
            // with a slightly later one — acceptable, the log line is
            // diagnostic-quality and the delta is at most one tick.
            uint32_t prev = __atomic_fetch_add(&s->consecutiveBufferFull, 1, __ATOMIC_RELAXED);  // v47
            if (prev == 0)
            {
                __atomic_store_n(&s->firstBufferFullAt, (uint32_t)_millis(), __ATOMIC_RELAXED);
            }
        }
        // FAILED: existing path unchanged (no stamp, no counter; slot is being reaped)
        YIELD();
    }
    else
    {
        // v22: defer to main-loop context (see SSEheartbeat top comment).
        ESP_LOGD(TAG, "Client %s (%s) not listening (heartbeat), marking for deferred remove", s->clientIP.toString().c_str(), s->clientUUID.c_str());
        s->pendingRemove = true;
        YIELD();
    }
}

void SSEHandler(uint32_t channel)
{
    if (server.args() != 1)
    {
        ESP_LOGE(TAG, "Sending %s, for: %s", response400missing, server.uri().c_str());
        server.send_P(400, type_txt, response400missing);
        return;
    }

    SSESubscription &s = subscription[channel];
    s.client = server.client(); // capture SSE server client connection
    if (s.clientUUID != server.arg(0))
    {
        ESP_LOGE(TAG, "Client %s (%s) tries to listen for SSE but not subscribed", s.client.remoteIP().toString().c_str(), server.arg(0).c_str());
        return handle_notfound();
    }
    s.client.setNoDelay(true);
    s.client.setTimeout(CLIENT_WRITE_TIMEOUT);       // default is 5000ms which is way too long (Watchdog will fire)
    // v24: setTimeout() in Arduino-ESP32 only bounds READS, not WRITES
    // — a wedged subscriber would block client.write() indefinitely
    // even with the timeout above. Set SO_SNDTIMEO directly via lwIP
    // setsockopt so writes return after CLIENT_SLOW_WRITE_MS even on
    // a stuck socket. Combined with the availableForWrite fast-path
    // and pendingRemove cleanup, a single dead client can no longer
    // wedge the broadcast loop.
#ifndef ESP8266
    int fd = s.client.fd();
    if (fd >= 0)
    {
        // v24: SO_SNDTIMEO bounds writes to CLIENT_SLOW_WRITE_MS.
        struct timeval sndto;
        sndto.tv_sec = 0;
        sndto.tv_usec = CLIENT_SLOW_WRITE_MS * 1000;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sndto, sizeof(sndto));

        // v47: OS-level TCP keepalive. Detects silently-dropped peers
        // (laptop-lid-close, switch-off, NAT-table-flush) within
        // KEEPIDLE + KEEPCNT*KEEPINTVL = 60s, well inside the 300s
        // SSE_IDLE_TIMEOUT_MS belt-and-suspenders. lwIP flips the socket
        // to !connected() once KEEPCNT probes fail; the next sweep tick
        // reaps via class 5b. SO_KEEPALIVE per-socket is OFF by default
        // even when LWIP_TCP_KEEPALIVE=y in sdkconfig — must enable.
        const int keepAliveOn = 1;
        const int keepIdle    = 30;  // seconds before first probe
        const int keepIntvl   = 10;  // seconds between probes
        const int keepCnt     = 3;   // probes before giving up
        setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE,  &keepAliveOn, sizeof(keepAliveOn));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &keepIdle,    sizeof(keepIdle));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepIntvl,   sizeof(keepIntvl));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &keepCnt,     sizeof(keepCnt));
    }
#endif
    server.setContentLength(CONTENT_LENGTH_UNKNOWN); // the payload can go on forever
    // v50: send `retry: 3000` SSE field in the initial handshake. Tells
    // the browser EventSource to use 3s as the reconnect interval after
    // any disconnect — overrides Chrome's exponential backoff (3s → 6s →
    // 12s → 24s+ after repeated failures) which on a slightly-flaky link
    // can become minutes-long stale-UI windows. 3000 ms matches industry
    // defaults (Mercure, sse-pubsub, nginx push-stream). Format: SSE field
    // must be its own line in the stream after the HTTP-header block ends.
    server.sendContent_P(PSTR(
        "HTTP/1.1 200 OK\n"
        "Content-Type: text/event-stream;\n"
        "Connection: keep-alive\n"
        "Cache-Control: no-cache\n"
        "Access-Control-Allow-Origin: *\n"
        "\n"
        "retry: 3000\n"
        "\n"));
    s.SSEconnected = true;
    s.SSEfailCount = 0;
    // v27: stamp lastActivity on successful EventSource handshake. Without
    // this a slot that subscribes + connects but never receives a broadcast
    // would have lastActivity=0 forever, causing the 5c idle check to
    // skip it (the !=0 guard) but also masking real wedges. Every other
    // success-path keeps it fresh; this seeds it.
    s.lastActivity = (uint32_t)_millis();
    if (s.heartbeatInterval)
    {
        s.heartbeatTimer.attach_ms(s.heartbeatInterval * 1000, [&s]
                                   {
#ifdef ESP8266
                                       schedule_recurrent_function_us([&s]()
                                                                      {
                                                                          SSEheartbeat(&s);
                                                                          return false; // run the fn only once
                                                                      },
                                                                      0); // zero micro seconds (run asap)
#else
                                       SSEheartbeat(&s);
                                       return;
#endif
                                   });
    }
    ESP_LOGD(TAG, "Client %s (%s) listening for SSE events on channel %d", s.client.remoteIP().toString().c_str(), s.clientUUID.c_str(), channel);
}

// v27/v28: heartbeat-interval bounds.
//
//   MIN = 30s  — coerce-target when client EXPLICITLY sends heartbeat=0.
//                Pre-v27 the lower bound was 0 (with 0 == "no heartbeat"),
//                which combined with the slot-leak bug to leave heartbeat=0
//                slots without any Ticker-driven liveness. We now coerce
//                explicit 0 → MIN so every server-acknowledged "I'd rather
//                have no heartbeat" client still gets a Ticker as backup
//                to the orphan sweep.
//
//   MAX = 60s  — one ESP32 Ticker tick cap.
//
//   DEFAULT = 1s — applies ONLY when the client sends NO heartbeat= arg
//                  at all. 1s is more liveness than MIN, not less, so
//                  this is functionally correct — the constant is
//                  named DEFAULT, not MIN, and the F5 coerce
//                  intentionally only fires when the arg is explicitly
//                  provided. Any client that wants the cheaper 30s
//                  cadence should send heartbeat=30 (or heartbeat=0
//                  which gets coerced to 30).
constexpr uint32_t SSE_HEARTBEAT_MIN_SEC = 30;
constexpr uint32_t SSE_HEARTBEAT_MAX_SEC = 60;
constexpr uint32_t SSE_HEARTBEAT_DEFAULT_SEC = 1;
// v27: refuse new SSE subscriptions when free heap is below this
// threshold. WebServer-side allocations (chunked response buffers,
// SSL handshake buffers if HTTPS, lwIP TCP PCBs) total >8KB per
// active connection on ESP32; under heap pressure we'd just OOM
// later in clientWrite anyway. 16KB picked empirically — a healthy
// idle device sits at ~140K, the watchdog trips at <50K, so 16K is
// safely past any normal load.
constexpr uint32_t SSE_MIN_HEAP_FREE = 16384;

void handle_subscribe()
{
    uint32_t channel;
    IPAddress clientIP = server.client().remoteIP(); // get IP address of client
    std::string SSEurl = restEvents;

    if (subscriptionCount == SSE_MAX_CHANNELS)
    {
        ESP_LOGE(TAG, "Client %s SSE Subscription declined, subscription count: %d", clientIP.toString().c_str(), subscriptionCount);
        for (channel = 0; channel < SSE_MAX_CHANNELS; channel++)
        {
            ESP_LOGD(TAG, "Client %d: %s at %s", channel, subscription[channel].clientUUID.c_str(), subscription[channel].clientIP.toString().c_str());
        }
        return handle_notfound(); // We ran out of channels
    }

    // All validations run BEFORE touching slot state or subscriptionCount,
    // so any rejection can `return` without unwinding partial state.

    // 1. clientIP
    if (clientIP == IPAddress(INADDR_NONE))  // explicit cast: sys/socket.h's u32_t INADDR_NONE is ambiguous with the IPAddress overload
    {
        ESP_LOGE(TAG, "Sending %s, for: %s as clientIP missing", response400invalid, server.uri().c_str());
        server.send_P(400, type_txt, response400invalid);
        return;
    }

    // 2. at least one argument
    if (server.args() < 1)
    {
        ESP_LOGE(TAG, "Sending %s, for: %s", response400missing, server.uri().c_str());
        server.send_P(400, type_txt, response400missing);
        return;
    }

    // 3. parse argument indices (no slot state mutated)
    int id = -1;
    bool logViewer = false;
    int heartbeatIntervalArgIdx = -1;
    for (int i = 0; i < server.args(); i++)
    {
        if (server.argName(i).equals("id"))
            id = i;
        else if (server.argName(i).equals("log"))
            logViewer = true;
        else if (server.argName(i).equals("heartbeat"))
            heartbeatIntervalArgIdx = i;
    }

    // v39: log-stream subscribe checks the recent-auth IP allowlist
    // instead of running Digest itself. EventSource (browser SSE API)
    // can't participate in Digest challenge/response — the v37 attempt
    // to use AUTHENTICATE() here returned 401 to every EventSource and
    // hard-broke the live log viewer for users with a password set.
    //
    // The flow now: user navigates to any auth'd page (`/showlog`,
    // `/setgdo`, `/reboot`, etc.) → AUTHENTICATE() macro succeeds →
    // recordAuthSuccess() stamps their IP in `authAllowlist` with
    // 15-min TTL (v52). The web UI's JS then opens EventSource to ?log=1
    // from the same origin → this check passes → SSE stream opens.
    // An attacker on a different LAN IP can't read the SSE log feed
    // without first AUTHENTICATE()-ing from their IP. No-password
    // installs short-circuit at the getPasswordRequired() check, same
    // as the AUTHENTICATE() macro. Door-status SSE (logViewer == false)
    // stays open, matching /status.json.
    if (logViewer && userConfig->getPasswordRequired() &&
        !isAuthAllowedForIP(server.client().remoteIP()))
    {
        ESP_LOGW(TAG, "SSE log subscribe rejected: IP %s not in recent-auth allowlist (auth via /showlog or another auth'd page first)",
                 server.client().remoteIP().toString().c_str());
        server.send_P(401, type_txt, PSTR("Unauthorized: open an authenticated page first to authorize this IP for SSE log access"));
        return;
    }

    // 4. heartbeat interval (F5 coerce)
    uint32_t heartbeatInterval = SSE_HEARTBEAT_DEFAULT_SEC;
    if (heartbeatIntervalArgIdx >= 0)
    {
        int hbi = server.arg(heartbeatIntervalArgIdx).toInt();
        if (hbi < 0 || hbi > (int)SSE_HEARTBEAT_MAX_SEC)
        {
            ESP_LOGE(TAG, "Invalid heartbeat interval (0 - %u) for SSE subscription", (unsigned)SSE_HEARTBEAT_MAX_SEC);
            server.send(400, type_txt, "Invalid heartbeat interval");
            return;
        }
        if (hbi == 0)
        {
            // v27: caller asked for "no heartbeat" but the orphan sweep needs a
            // Ticker running on every slot to detect class-5b drops in real-time.
            // Coerce to MIN. logs.html ships v27-and-later with heartbeat=10 so
            // this branch only fires for older logs.html (cache) or third-party clients.
            ESP_LOGD(TAG, "heartbeat=0 coerced to %u for %s",
                     (unsigned)SSE_HEARTBEAT_MIN_SEC, clientIP.toString().c_str());
            heartbeatInterval = SSE_HEARTBEAT_MIN_SEC;
        }
        else
        {
            heartbeatInterval = (uint32_t)hbi;
        }
    }

    // 5. low-heap rejection (F6) — return 503 without touching slot state.
    if (ESP.getFreeHeap() < SSE_MIN_HEAP_FREE)
    {
        ESP_LOGW(TAG, "SSE subscription rejected - low heap (%u < %u)",
                 (unsigned)ESP.getFreeHeap(), (unsigned)SSE_MIN_HEAP_FREE);
        server.send(503, type_txt, "Low memory, try again shortly\n");
        return;
    }

    // 6. client validity — checked BEFORE we capture into the slot.
    WiFiClient client = server.client();
    if (!client || !client.connected())
    {
        ESP_LOGE(TAG, "Invalid client for SSE subscription");
        server.send(400, type_txt, "Invalid client connection");
        return;
    }

    // 6a. log-audit-003: rapid-recurrence dampener. Age out stale entries
    // first (cheap, prevents the table from accumulating expired stamps
    // and silently aging into garbage), then check whether the incoming
    // UUID was reaped within SSE_DAMPENER_WINDOW_MS. If so, return 429
    // and bail without touching slot state. Misbehaving clients (e.g.
    // iOS Safari background-tab SSE that re-subscribes immediately after
    // every reap) get backed off; well-behaved clients with a single
    // transient wedge are unaffected.
    //
    // id == -1 means the request had no `id=` argument; skip the dampener
    // entirely rather than matching against server.arg(-1) (undefined).
    if (id != -1)
    {
        uint32_t nowSub = (uint32_t)_millis();
        for (auto &r : recentReaps)
        {
            if (r.reapedAt != 0 && (nowSub - r.reapedAt) >= SSE_DAMPENER_WINDOW_MS)
            {
                r.uuid[0] = '\0';
                r.reapedAt = 0;
            }
        }
        // Bind the String temp to a local so .c_str() doesn't dangle across
        // the strcmp loop (server.arg returns by value).
        String incomingUUIDStr = server.arg(id);
        const char *incomingUUID = incomingUUIDStr.c_str();
        if (incomingUUID[0] != '\0')
        {
            for (auto &r : recentReaps)
            {
                if (r.reapedAt != 0 &&
                    r.uuid[0] != '\0' &&
                    strcmp(r.uuid, incomingUUID) == 0 &&
                    (nowSub - r.reapedAt) < SSE_DAMPENER_WINDOW_MS)
                {
                    uint32_t ageMs = nowSub - r.reapedAt;
                    ESP_LOGW(TAG, "SSE 429 dampener uuid=%s ip=%s ageMs=%u — recently reaped (wedged on flow-control); backing off client",
                             r.uuid, clientIP.toString().c_str(), (unsigned)ageMs);
                    server.send(429, type_txt, "Recently reaped — try again in 60s\n");
                    return;
                }
            }
        }
    }

    // 7. find existing UUID. removeSSEsubscription decrements
    //    subscriptionCount; we don't pre-increment here so re-subscribing
    //    is a clean wash.
    bool foundExisting = false;
    for (channel = 0; channel < SSE_MAX_CHANNELS; channel++)
    {
        if (subscription[channel].clientUUID == server.arg(id))
        {
            if (subscription[channel].SSEconnected)
            {
                // Already connected.  We need to close it down as client will be reconnecting
                ESP_LOGD(TAG, "Client %s (%s) already connected on channel %d, remove SSE subscription", clientIP.toString().c_str(), server.arg(id).c_str(), channel);
                removeSSEsubscription(&subscription[channel]);
                break; // without setting foundExisting... so we create new instance.
            }
            else
            {
                // Subscribed but not connected yet, so nothing to close down.
                ESP_LOGD(TAG, "Client %s (%s) already subscribed for SSE but not connected on channel %d", clientIP.toString().c_str(), server.arg(id).c_str(), channel);
            }
            foundExisting = true;
            break;
        }
    }

    // 8. allocate a new slot if needed
    if (!foundExisting)
    {
        // v28 BUG FIX: match the canonical "free" marker used by the orphan
        // sweep at line 1642 — `clientIP == IPAddress(INADDR_NONE)`.
        //
        // Pre-v28 this scan was `if (!subscription[channel].clientIP)`,
        // which calls IPAddress::operator bool(). That returns false ONLY
        // when the address is 0.0.0.0 (the default-constructed state) —
        // NOT when it's INADDR_NONE (0xFFFFFFFF, the marker that
        // setup_web + removeSSEsubscription assign to "free" the slot).
        //
        // Effect: the very first 8 subscribes worked (initial slots have
        // dword=0 from struct init, so !clientIP was true). Every slot
        // subsequently freed via removeSSEsubscription was set to
        // INADDR_NONE — at which point the scan never saw it as free
        // again. After all 8 slots had been used + freed once, the device
        // permanently rejected new subscribes with 503 "no free slots
        // available," even though the sweep correctly reported
        // sseSlotsAlloc=0. The sweep and the scan disagreed about what
        // "free" meant, and the scan had it wrong. v22-v26 hit this
        // identically; the v22 SSE deadlock crashed the device before
        // slot 9 was attempted, masking the bug. v27's deadlock fix
        // exposed it.
        for (channel = 0; channel < SSE_MAX_CHANNELS; channel++)
            if (subscription[channel].clientIP == IPAddress(INADDR_NONE))
                break;
    }

    // 9. no free slot — 503 (still no state mutated for this request)
    if (channel >= SSE_MAX_CHANNELS)
    {
        ESP_LOGE(TAG, "SSE subscription failed - no free slots available");
        server.send(503, type_txt, "No free subscription slots available");
        return;
    }

    // 10. commit slot fields (F7: explicit reset of v27 lifecycle fields).
    subscription[channel].clientIP = clientIP;
    subscription[channel].client = client;
    subscription[channel].heartbeatTimer = Ticker();
    subscription[channel].SSEconnected = false;
    subscription[channel].SSEfailCount = 0;
    subscription[channel].clientUUID = server.arg(id);
    subscription[channel].logViewer = logViewer;
    subscription[channel].heartbeatInterval = heartbeatInterval;
    subscription[channel].pendingRemove = false;
    subscription[channel].subscribedAt = (uint32_t)_millis();
    subscription[channel].lastActivity = (uint32_t)_millis();
    subscription[channel].consecutiveBufferFull = 0;  // v47
    subscription[channel].firstBufferFullAt = 0;      // log-audit-003

    // 11. counter bumped only AFTER all rejection paths exhausted.
    if (!foundExisting)
    {
        subscriptionCount++;
    }

    SSEurl += std::to_string(channel);
    ESP_LOGD(TAG, "Client %s (%s) SSE subscription: %s, Total: %d, Heartbeat: %d, Log: %d", clientIP.toString().c_str(), server.arg(id).c_str(), SSEurl.c_str(), subscriptionCount, heartbeatInterval, (int)logViewer);
    server.sendHeader(F("Cache-Control"), F("no-cache, no-store"));
    server.send_P(200, type_txt, SSEurl.c_str());
}

// v27/v28: best-effort cleanup endpoint. Browser calls this via
// navigator.sendBeacon() on beforeunload to release the SSE slot
// without waiting for the orphan sweep timeout. Browsers don't
// guarantee beacon delivery (especially on mobile background tabs),
// so the orphan sweep is still the authoritative cleanup path —
// this just cuts leak rate during normal navigation.
//
// v28: Same-origin guard added. v27's comment claimed "sendBeacon
// cannot set custom headers" — true for X-* headers, but BROWSERS
// DO populate Origin / Referer / Host on sendBeacon POSTs, which is
// exactly what enforce_same_origin checks. Adding the guard blocks
// drive-by cross-origin closes (e.g. malicious LAN page knocking
// the user offline) without breaking the legitimate beacon flow.
//
// AUTH note: this endpoint is intentionally not gated by AUTHENTICATE.
// sendBeacon does not implement Digest, so adding AUTH would either
// break legitimate beacon delivery (the sole UA mechanism for "release
// my SSE slot on page unload") or repeat the v37 EventSource trap of
// spurious 401s on background-tab transitions. The session UUID is the
// authority, but its premise is best-effort: syslog readers and same-
// LAN sniffers can capture active UUIDs (we log channel-allocation at
// handle_subscribe and the device exports syslog UDP unauthenticated).
// An attacker who reads syslog can beacon-disconnect any user — bounded
// to forcing a browser SSE reconnect (DoS, no state damage).
void handle_unsubscribe()
{
    if (!enforce_same_origin("/rest/events/unsubscribe")) return;
    // v43 (audit W24): drop the Arduino `String uuid;` + `uuid =
    // server.arg(i);` heap-double-alloc pattern (1 String for the local
    // + 1 for server.arg's return) on a path that fires per-page-navigate
    // via navigator.sendBeacon. char[40] holds 36-char UUIDs comfortably.
    char uuid[40] = {0};
    for (int i = 0; i < server.args(); i++)
    {
        if (server.argName(i).equals("id"))
        {
            strncpy(uuid, server.arg(i).c_str(), sizeof(uuid) - 1);
            break;
        }
    }
    if (uuid[0] == '\0')
    {
        server.send_P(400, type_txt, response400missing);
        return;
    }
    int matched = 0;
    for (uint32_t i = 0; i < SSE_MAX_CHANNELS; i++)
    {
        if (subscription[i].clientUUID.equals(uuid) &&
            subscription[i].clientIP != IPAddress(INADDR_NONE))
        {
            ESP_LOGD(TAG, "unsubscribe beacon for UUID %s on channel %u",
                     uuid, (unsigned)i);
            subscription[i].pendingRemove = true;
            matched++;
        }
    }
    server.send(204, type_txt, "");
    (void)matched;
}

void handle_crashlog()
{
    // v54: read-only polling endpoint — use allowlist fast-path to
    // avoid arduino-esp32's stale-nonce re-prompt loop. See
    // AUTHENTICATE_OR_ALLOWLIST macro definition for full rationale.
    AUTHENTICATE_OR_ALLOWLIST();
    server.client().print(response200);
    ratgdoLogger->printCrashLog(server.client());
}

void handle_showlog()
{
    // v54: read-only polling endpoint — use allowlist fast-path to
    // avoid arduino-esp32's stale-nonce re-prompt loop. logs.html
    // polls this every 3s; pre-v54 every poll could trigger a
    // browser auth prompt due to Digest nonce rotation.
    AUTHENTICATE_OR_ALLOWLIST();
    server.client().print(response200);
    ratgdoLogger->printMessageLog(server.client());
}

void handle_showrebootlog()
{
    // v54: read-only polling endpoint — use allowlist fast-path to
    // avoid arduino-esp32's stale-nonce re-prompt loop.
    AUTHENTICATE_OR_ALLOWLIST();
    server.client().print(response200);
#ifdef ESP8266
    File file = LittleFS.open(REBOOT_LOG_MSG_FILE, "r");
    ratgdoLogger->printSavedLog(file, server.client());
    file.close();
#else
    ratgdoLogger->printSavedLog(server.client());
#endif
}

void handle_clearcrashlog()
{
    AUTHENTICATE();
    ESP_LOGI(TAG, "Clear saved crash log");
    ratgdoLogger->clearCrashLog();
    server.send_P(200, type_txt, PSTR("Crash log cleared\n"));
}

#ifdef CRASH_DEBUG
void handle_crash_oom()
{
    ESP_LOGI(TAG, "Attempting to use up all memory");
    server.send_P(200, type_txt, PSTR("Attempting to use up all memory\n"));
    delay(1000);
    for (int i = 0; i < 30; i++)
    {
        ESP_LOGI(TAG, "malloc(1024)");
        crashptr = malloc(1024);
    }
}

void handle_forcecrash()
{
    ESP_LOGI(TAG, "Attempting to null ptr deref");
    server.send_P(200, type_txt, PSTR("Attempting to null ptr deref\n"));
    delay(1000);
    ESP_LOGI(TAG, "Result: %s", test_str);
}
#endif // CRASH_DEBUG

void SSEBroadcastState(const char *data, BroadcastType type)
{
    if (!web_setup_done)
        return;

    // Flash LED to signal activity
    // led.flash(FLASH_MS);

    // if nothing subscribed, then return
    if (subscriptionCount == 0)
        return;

    // v38 (audit W7): use a stack-local write buffer instead of the
    // file-scope `loopTaskScratchBuf512` global. Pre-v38 SSEBroadcastState filled
    // `loopTaskScratchBuf512` then called clientWriteEx, while
    // simultaneously web_loop's RATGDO_STATUS path AND any
    // ESP_LOGx-triggered LOG_MESSAGE path could be calling here
    // concurrently from different tasks (loopTask, esp_timer, WiFi
    // event task, comms callbacks). Two concurrent broadcasts
    // overwrote each other's payload mid-write — subscribers saw
    // truncated/garbled SSE events as occasional weird log lines.
    // Stack-local fixes the race at the source.
    //
    // Stack delta on ESP32: +512 B per SSEBroadcastState invocation.
    // loopTask (8 KB) and esp_timer task (4 KB) both have ample
    // headroom; the existing tmrSvcHWM reported in the periodic HK
    // diag log will surface any regression.
    //
    // ESP8266 keeps the static buffer — single-task cooperative
    // scheduling means the race doesn't exist there, AND ESP8266's
    // 4 KB main stack is too tight to absorb +512 B.
#ifdef ESP8266
    char *wb = loopTaskScratchBuf512;
    const size_t wbSize = sizeof(loopTaskScratchBuf512);
#else
    char wb[512];
    const size_t wbSize = sizeof(wb);
#endif

    for (uint32_t i = 0; i < SSE_MAX_CHANNELS; i++)
    {
        YIELD(); // yield between each SSE client
        if (subscription[i].SSEconnected)
        {
            if (subscription[i].client.connected())
            {
                if (type == LOG_MESSAGE)
                {
                    if (subscription[i].logViewer)
                    {
                        if (snprintf_P(wb, wbSize, PSTR("event: logger\ndata: %s\n\n"), data) >= (int)wbSize)
                        {
                            // Will not fit in our write buffer.
#ifdef ESP8266
                            subscription[i].client.flush(); // make sure previous data all sent.
                            // ESP8266 keeps the framework printf fallback — its
                            // WiFiClient::availableForWrite() works (queries
                            // tcp_sndbuf), so the framework's send loop doesn't
                            // hit the log_e-on-EAGAIN noise pattern.
                            size_t pwrote = subscription[i].client.printf("event: logger\ndata: %s\n\n", data);
                            if (pwrote > 0)
                                subscription[i].lastActivity = (uint32_t)_millis();
#else
                            // log-audit-004: route oversized log payloads through
                            // clientWriteEx so they use the same direct lwip_send
                            // path as in-buffer writes — avoids the framework
                            // NetworkClient::write retry loop's `log_e fail on fd
                            // %d, errno: %d` flood on benign EAGAIN flow control.
                            // Heap allocation is fine here: this branch only fires
                            // for log lines > ~490 B (rare), and ~3 KB allocations
                            // are clean against typical post-boot heap (~50 KB+).
                            int needed = snprintf_P(NULL, 0, PSTR("event: logger\ndata: %s\n\n"), data);
                            if (needed > 0)
                            {
                                char *bigBuf = (char *)malloc((size_t)needed + 1);
                                if (bigBuf)
                                {
                                    snprintf_P(bigBuf, (size_t)needed + 1, PSTR("event: logger\ndata: %s\n\n"), data);
                                    SseWriteResult r = clientWriteEx(subscription[i].client, bigBuf);
                                    free(bigBuf);
                                    if (r == SseWriteResult::OK)
                                    {
                                        subscription[i].lastActivity = (uint32_t)_millis();
                                        __atomic_store_n(&subscription[i].consecutiveBufferFull, 0, __ATOMIC_RELAXED);
                                        __atomic_store_n(&subscription[i].firstBufferFullAt, 0, __ATOMIC_RELAXED);
                                    }
                                    else if (r == SseWriteResult::BUFFER_FULL)
                                    {
                                        subscription[i].lastActivity = (uint32_t)_millis();
                                        uint32_t prev = __atomic_fetch_add(&subscription[i].consecutiveBufferFull, 1, __ATOMIC_RELAXED);
                                        if (prev == 0)
                                        {
                                            __atomic_store_n(&subscription[i].firstBufferFullAt, (uint32_t)_millis(), __ATOMIC_RELAXED);
                                        }
                                    }
                                }
                                // malloc failure: drop the oversized log entry
                                // silently — alternative is the noisy framework
                                // path we're trying to escape.
                            }
#endif
                        }
                        else
                        {
                            // v27: only stamp lastActivity on a successful write so
                            // the orphan-sweep idle check (5c) actually sees idle slots.
                            // v29: tri-state — stamp on OK or BUFFER_FULL, only skip
                            // on FAILED (real wedge). Tailscale / congested-link
                            // subscribers no longer get reaped every 120s.
                            // v47: also reset/increment consecutiveBufferFull for sweep 5d.
                            SseWriteResult r = clientWriteEx(subscription[i].client, wb);
                            if (r == SseWriteResult::OK)
                            {
                                subscription[i].lastActivity = (uint32_t)_millis();
                                __atomic_store_n(&subscription[i].consecutiveBufferFull, 0, __ATOMIC_RELAXED);  // v47: reset on real drain
                                __atomic_store_n(&subscription[i].firstBufferFullAt, 0, __ATOMIC_RELAXED);      // log-audit-003: clear streak start
                            }
                            else if (r == SseWriteResult::BUFFER_FULL)
                            {
                                subscription[i].lastActivity = (uint32_t)_millis();  // unchanged: BUFFER_FULL still stamps activity (v29)
                                // log-audit-003: stamp streak-start on 0->1 transition (see SSEheartbeat for rationale).
                                uint32_t prev = __atomic_fetch_add(&subscription[i].consecutiveBufferFull, 1, __ATOMIC_RELAXED);  // v47
                                if (prev == 0)
                                {
                                    __atomic_store_n(&subscription[i].firstBufferFullAt, (uint32_t)_millis(), __ATOMIC_RELAXED);
                                }
                            }
                            // FAILED: existing path unchanged (no stamp, no counter; slot is being reaped)
                        }
                    }
                }
                else if (type == RATGDO_STATUS)
                {
                    ESP_LOGV(TAG, "Client %s (%s) send status SSE on channel %d, data: %s",
                             IPAddress(subscription[i].clientIP).toString().c_str(),
                             subscription[i].clientUUID.c_str(), i, data);
                    if (snprintf_P(wb, wbSize, PSTR("event: message\ndata: %s\n\n"), data) >= (int)wbSize)
                    {
                        // Will not fit in our write buffer.
#ifdef ESP8266
                        subscription[i].client.flush(); // make sure previous data all sent.
                        // ESP8266 keeps the framework printf fallback (see
                        // matching LOG_MESSAGE branch for rationale).
                        size_t pwrote = subscription[i].client.printf("event: message\ndata: %s\n\n", data);
                        if (pwrote > 0)
                            subscription[i].lastActivity = (uint32_t)_millis();
#else
                        // log-audit-004: route oversized status payloads through
                        // clientWriteEx (direct lwip_send) instead of the framework's
                        // log_e-on-EAGAIN-noisy NetworkClient::write retry loop.
                        // jsonPeak observed at 2312 B — this is the dominant
                        // oversize-broadcast path; killing the noise here is the
                        // primary errno-11-recurrence fix.
                        int needed = snprintf_P(NULL, 0, PSTR("event: message\ndata: %s\n\n"), data);
                        if (needed > 0)
                        {
                            char *bigBuf = (char *)malloc((size_t)needed + 1);
                            if (bigBuf)
                            {
                                snprintf_P(bigBuf, (size_t)needed + 1, PSTR("event: message\ndata: %s\n\n"), data);
                                SseWriteResult r = clientWriteEx(subscription[i].client, bigBuf);
                                free(bigBuf);
                                if (r == SseWriteResult::OK)
                                {
                                    subscription[i].lastActivity = (uint32_t)_millis();
                                    __atomic_store_n(&subscription[i].consecutiveBufferFull, 0, __ATOMIC_RELAXED);
                                    __atomic_store_n(&subscription[i].firstBufferFullAt, 0, __ATOMIC_RELAXED);
                                }
                                else if (r == SseWriteResult::BUFFER_FULL)
                                {
                                    subscription[i].lastActivity = (uint32_t)_millis();
                                    uint32_t prev = __atomic_fetch_add(&subscription[i].consecutiveBufferFull, 1, __ATOMIC_RELAXED);
                                    if (prev == 0)
                                    {
                                        __atomic_store_n(&subscription[i].firstBufferFullAt, (uint32_t)_millis(), __ATOMIC_RELAXED);
                                    }
                                }
                            }
                            // malloc failure: skip this broadcast — silent drop
                            // beats the framework noise path.
                        }
#endif
                    }
                    else
                    {
                        // v29: tri-state — stamp on OK or BUFFER_FULL, only skip
                        // on FAILED (real wedge). Same rationale as LOG_MESSAGE.
                        // v47: also reset/increment consecutiveBufferFull for sweep 5d.
                        SseWriteResult r = clientWriteEx(subscription[i].client, wb);
                        if (r == SseWriteResult::OK)
                        {
                            subscription[i].lastActivity = (uint32_t)_millis(); // v27
                            __atomic_store_n(&subscription[i].consecutiveBufferFull, 0, __ATOMIC_RELAXED);  // v47: reset on real drain
                            __atomic_store_n(&subscription[i].firstBufferFullAt, 0, __ATOMIC_RELAXED);      // log-audit-003: clear streak start
                        }
                        else if (r == SseWriteResult::BUFFER_FULL)
                        {
                            subscription[i].lastActivity = (uint32_t)_millis();  // unchanged: BUFFER_FULL still stamps activity (v29)
                            // log-audit-003: stamp streak-start on 0->1 transition (see SSEheartbeat for rationale).
                            uint32_t prev = __atomic_fetch_add(&subscription[i].consecutiveBufferFull, 1, __ATOMIC_RELAXED);  // v47
                            if (prev == 0)
                            {
                                __atomic_store_n(&subscription[i].firstBufferFullAt, (uint32_t)_millis(), __ATOMIC_RELAXED);
                            }
                        }
                        // FAILED: existing path unchanged (no stamp, no counter; slot is being reaped)
                    }
                }
            }
            else
            {
                // Client connection has gone.  Remove from our subscribed client list.
                // v38 (audit W1): defer the actual removal — SSEBroadcastState
                // can be invoked from non-loopTask contexts (any ESP_LOGx
                // caller via LOG::logToBuffer's LOG_MESSAGE fanout: Ticker
                // callbacks, WiFi event task, comms-callback paths). The v22
                // SSEheartbeat fix established the discipline that all SSE
                // slot teardown must happen on loopTask, because
                // removeSSEsubscription descends into client.stop() →
                // lwIP socket close. Calling that from a non-loopTask
                // context risked the race that motivated the v22 deferred
                // sweep, just relocated to this site. pendingRemove is
                // reaped by the next service tick via process_sse_pending_removes.
                ESP_LOGD(TAG, "Client %s (%s) not listening (broadcast), marking for deferred remove",
                         subscription[i].clientIP.toString().c_str(), subscription[i].clientUUID.c_str());
                subscription[i].pendingRemove = true;
            }
        }
    }
    YIELD();
}

// Implement our own firmware update so can enforce MD5 check.
// Based on HTTPUpdateServer
void _setUpdaterError()
{
    StreamString str;
    Update.printError(str);
    _updaterError = str.c_str();
    ESP_LOGE(TAG, "Update error: %s", str.c_str());
}

void handle_update()
{
    bool verify = !strcmp(server.arg("action").c_str(), "verify");

    server.sendHeader(F("Access-Control-Allow-Headers"), "*");
    server.sendHeader(F("Access-Control-Allow-Origin"), "*");
    AUTHENTICATE();

    server.client().setNoDelay(true);
    if (!verify && Update.hasError())
    {
        // Error logged in _setUpdaterError
#ifdef ESP8266
        eboot_command_clear();
#else
        // TODO how to handle firmware upload failure on ESP32?
#endif
        firmwareUpdateSub = NULL;
        ESP_LOGE(TAG, "Firmware upload error. Aborting update, not rebooting");
        server.send(400, type_txt, _updaterError.c_str());
        return;
    }

    if (server.args() > 0)
    {
        firmwareUpdateSub = NULL;
        // Don't reboot, user/client must explicitly request reboot.
        server.send_P(200, type_txt, PSTR("Upload Success.\n"));
    }
    else
    {
        // Legacy... no query string args, so automatically reboot...
        server.send_P(200, type_txt, PSTR("Upload Success. Rebooting...\n"));
        // Allow time to process send() before terminating web server...
        delay(500);
        server.stop();
        sync_and_restart();
    }
}

void handle_firmware_upload()
{
    // handler for the file upload, gets the sketch bytes, and writes
    // them through the Update object
    static size_t uploadProgress;
    static uint32_t nextPrintPercent;
    HTTPUpload &upload = server.upload();
    static bool verify = false;
    static size_t size = 0;
    static const char *md5 = NULL;

    if (upload.status == UPLOAD_FILE_START)
    {
        _updaterError.clear();

#ifdef ESP8266
        _authenticatedUpdate = !userConfig->getPasswordRequired() || server.authenticateDigest(userConfig->getwwwUsername(), userConfig->getwwwCredentials());
#else
        _authenticatedUpdate = !userConfig->getPasswordRequired() || server.authenticate(ratgdoAuthenticate);
#endif
        if (!_authenticatedUpdate)
        {
            ESP_LOGE(TAG, "Unauthenticated Update");
            return;
        }
        ESP_LOGI(TAG, "Update: %s", upload.filename.c_str());
        verify = !strcmp(server.arg("action").c_str(), "verify");
        size = atoi(server.arg("size").c_str());
        md5 = server.arg("md5").c_str();

        // We are updating.  If size and MD5 provided, save them
        firmwareSize = size;
        if (strlen(md5) > 0)
            strlcpy(firmwareMD5, md5, sizeof(firmwareMD5));

        uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
        ESP_LOGI(TAG, "Available space for upload: %lu", maxSketchSpace);
        ESP_LOGI(TAG, "Firmware size: %s", (firmwareSize > 0) ? std::to_string(firmwareSize).c_str() : "Unknown");
        ESP_LOGI(TAG, "Flash chip speed %d MHz", ESP.getFlashChipSpeed() / 1000000);
        // struct eboot_command ebootCmd;
        // eboot_command_read(&ebootCmd);
        // ESP_LOGI(TAG, "eboot_command: 0x%08X 0x%08X [0x%08X 0x%08X 0x%08X (%d)]", ebootCmd.magic, ebootCmd.action, ebootCmd.args[0], ebootCmd.args[1], ebootCmd.args[2], ebootCmd.args[2]);
        if (firmwareSize > maxSketchSpace)
        {
            ESP_LOGE(TAG, "Firmware size is larger than available OTA upload space");
            // If we detect this error then we will not shut down all our services, because upload will fail.
            // Failure is detected on first call to Update.write() where it will set UPDATE_ERROR_SPACE.
            // This is passed back to the client with a http 400 error and the string "Not Enough Space"
        }
        else if (!verify)
        {
            // Close services so we don't have to handle network traffic during update
            // Only if not verifying as either will have been shutdown on immediately prior upload, or we
            // just want to verify without disrupting operation of the HomeKit service.
            ESP_LOGI(TAG, "Shutdown HomeKit and GDO communications");

            // Service loop has things like reboot after X days, homekit notifications, etc. that we don't want during OTA
            suspend_service_loop = true;
#ifdef RATGDO32_DISCO
            // Ignore vehicle distance sensor
            vehicle_setup_done = false;
#endif
            shutdown_comms();
#ifdef ESP8266
            // Shutdown HomeKit
            homekit_setup_done = false;
            arduino_homekit_close();
#else
            // Shutdown HomeSpan server
            vTaskDelete(homeSpan.getAutoPollTask());
#endif
        }

        if (!verify && !Update.begin((firmwareSize > 0) ? firmwareSize : maxSketchSpace, U_FLASH))
        {
            _setUpdaterError();
        }
        else if (strlen(firmwareMD5) > 0)
        {
            // uncomment for testing...
            // char firmwareMD5[] = "675cbfa11d83a792293fdc3beb199cXX";
            ESP_LOGI(TAG, "Expected MD5: %s", firmwareMD5);
            Update.setMD5(firmwareMD5);
            if (firmwareSize > 0)
            {
                uploadProgress = 0;
                nextPrintPercent = 5;
                ESP_LOGI(TAG, "%s progress: 00", verify ? "Verify" : "Update");
            }
        }
    }
    else if (_authenticatedUpdate && upload.status == UPLOAD_FILE_WRITE && !_updaterError.length())
    {
        // v43 (audit W30): dropped Serial.print(".") progress noise.
        // ESP_LOGI percentage line below is the authoritative indicator.
        if (firmwareSize > 0)
        {
            uploadProgress += upload.currentSize;
            uint32_t uploadPercent = (uploadProgress * 100) / firmwareSize;
            if (uploadPercent >= nextPrintPercent)
            {
                ESP_LOGI(TAG, "%s progress: %d", verify ? "Verify" : "Update", uploadPercent);
                nextPrintPercent += 5;
                // Report percentage to browser client if it is listening
                if (firmwareUpdateSub && firmwareUpdateSub->client.connected())
                {
                    static char *json = status_json;
                    TAKE_MUTEX();
                    JSON_START(json);
                    JSON_ADD_INT("uploadPercent", uploadPercent);
                    JSON_END();
                    JSON_REMOVE_NL(json);
                    snprintf_P(loopTaskScratchBuf512, sizeof(loopTaskScratchBuf512), PSTR("event: uploadStatus\ndata: %s\n\n"), json);
                    // v28: stamp lastActivity on success — matches the
                    // SSEBroadcastState pattern. Prevents the orphan
                    // sweep from reaping the slot mid-update if a slow
                    // upload spans >300s (large firmware on slow link).
                    // v29: tri-state — stamp on OK or BUFFER_FULL, only
                    // skip on FAILED (real wedge).
                    // v47: also reset/increment consecutiveBufferFull for sweep 5d.
                    // (OTA slot is exempted from the sweep 5d reap, but the
                    // counter is still tracked for visibility / consistency.)
                    SseWriteResult r = clientWriteEx(firmwareUpdateSub->client, loopTaskScratchBuf512);
                    if (r == SseWriteResult::OK)
                    {
                        firmwareUpdateSub->lastActivity = (uint32_t)_millis();
                        __atomic_store_n(&firmwareUpdateSub->consecutiveBufferFull, 0, __ATOMIC_RELAXED);  // v47: reset on real drain
                        __atomic_store_n(&firmwareUpdateSub->firstBufferFullAt, 0, __ATOMIC_RELAXED);      // log-audit-003: clear streak start
                    }
                    else if (r == SseWriteResult::BUFFER_FULL)
                    {
                        firmwareUpdateSub->lastActivity = (uint32_t)_millis();  // unchanged: BUFFER_FULL still stamps activity (v29)
                        // log-audit-003: stamp streak-start on 0->1 transition (see SSEheartbeat for rationale).
                        uint32_t prev = __atomic_fetch_add(&firmwareUpdateSub->consecutiveBufferFull, 1, __ATOMIC_RELAXED);  // v47
                        if (prev == 0)
                        {
                            __atomic_store_n(&firmwareUpdateSub->firstBufferFullAt, (uint32_t)_millis(), __ATOMIC_RELAXED);
                        }
                    }
                    // FAILED: existing path unchanged (no stamp, no counter; slot is being reaped)
                    GIVE_MUTEX();
                }
            }
        }
        if (!verify)
        {
            // Don't write if verifying... we will just check MD5 of the flash at the end.
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
                _setUpdaterError();
        }
    }
    else if (_authenticatedUpdate && upload.status == UPLOAD_FILE_END && !_updaterError.length())
    {
        // v43 (audit W30): dropped Serial.print("\n") that closed the
        // dropped progress-dots line.
        if (!verify)
        {
            if (Update.end(true))
            {
                ESP_LOGI(TAG, "Upload size: %zu", upload.totalSize);
            }
            else
            {
                _setUpdaterError();
            }
            firmwareUpdateSub = NULL;
        }
    }
    else if (_authenticatedUpdate && upload.status == UPLOAD_FILE_ABORTED)
    {
        if (!verify)
            Update.end();
        ESP_LOGI(TAG, "%s was aborted", verify ? "Verify" : "Update");
        firmwareUpdateSub = NULL;
    }
}
