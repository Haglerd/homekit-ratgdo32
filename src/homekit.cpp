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
 * Thomas Hagan...     https://github.com/tlhagan
 * Brandon Matthews... https://github.com/thenewwazoo
 * Jonathan Stroud...  https://github.com/jgstroud
 *
 */

#ifdef ESP8266
#include <arduino_homekit_server.h>
#include <ESP8266WiFi.h>
#endif // ESP8266

// Ticker for periodic HomeKit health log
#include <Ticker.h>
#ifndef ESP8266
// v24: heap_caps_get_largest_free_block for fragmentation visibility
#include "esp_heap_caps.h"
#include "esp_timer.h"
#endif

// RATGDO project includes
#include "ratgdo.h"
#include "config.h"
#include "comms.h"
#include "homekit.h"
#include "web.h"
#include "softAP.h"
#include "led.h"
#include "provision.h"
#include "instrumentation.h"

#ifdef RATGDO32_DISCO
#include "vehicle.h"
#endif
#ifdef USE_GDOLIB
#include "gdo.h"
#else
#include "drycontact.h"
#endif

// Logger tag
static const char *TAG = "ratgdo-homekit";
char qrPayload[21];
bool homekit_setup_done = false;

#ifdef ESP8266
// Forward-declare setters used by characteristics
homekit_value_t current_door_state_get();
homekit_value_t target_door_state_get();
void target_door_state_set(const homekit_value_t new_value);
homekit_value_t obstruction_detected_get();
homekit_value_t current_lock_state_get();
homekit_value_t target_lock_state_get();
void target_lock_state_set(const homekit_value_t new_value);
homekit_value_t light_state_get();
void light_state_set(const homekit_value_t value);

#else // not ESP8266

// v40 (audit W13): volatile. Written by statusCallback (loopTask via
// HomeSpan poll) on HS_REBOOTING; read by homekit_health_log (Ticker /
// esp_timer task) every 180s and by other gates. Without volatile the
// compiler is allowed to cache the value across calls within the same
// Ticker invocation, missing the reboot window. Single-byte access is
// atomic on Xtensa so volatile is sufficient (no need for atomic ops).
static volatile bool rebooting = false;
// v28: volatile for cross-context consistency (HomeSpan callbacks
// write, homekit_health_log Ticker reads). Single-byte ⇒ atomic on
// ESP32; volatile prevents the compiler from hoisting reads.
static volatile bool isPaired = false;

#endif // ESP8266

#ifdef CRASH_DEBUG
// Default arg comes from comms.h (already included above) — repeating it
// here is ill-formed C++ [dcl.fct.default].
extern void delayFnCall(uint32_t ms, void (*callback)(), bool preempt_force_close);
void testDelayFn(const char *buf)
{
    delayFnCall(5000, (void (*)())NULL);
}
#endif // CRASH_DEBUG

/****************************************************************************
 * Convert a decimal number to base62 (so can use A-Za-z0-9 to represent it.
 */
char *toBase62(char *base62, size_t len, uint32_t base10)
{
    static const char base62Chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    size_t i = 0;
    // Will pad with zeros until base62 buffer filled (to len)
    while ((base10 > 0) || (i < len - 1))
    {
        base62[i++] = base62Chars[base10 % 62];
        base10 /= 62;
    }
    // null terminate
    base62[i] = 0;
    // Now reverse order of the string;
    char *str = base62;
    char *end = str + strlen(str) - 1;
    while (str < end)
    {
        *str ^= *end;
        *end ^= *str;
        *str ^= *end;
        str++;
        end--;
    }
    return base62;
}

#ifdef ESP8266
/****************************************************************************
 * On ESP8266 we use the Arduino HomeKit library.  On ESP32 we use HomeSpan library
 * This requires completely separate initialization and runtime handling.
 *
 * If making a change to any of the common functions, be sure to check whether
 * the change is required in both implementations.
 */

void homekit_event(homekit_event_t event)
{
    static bool reset_comms = false;
    switch (event)
    {
    case homekit_event_t::HOMEKIT_EVENT_SERVER_INITIALIZED:
    {
        ESP_LOGI(TAG, "HomeKit Server Initialized");
        break;
    }
    case homekit_event_t::HOMEKIT_EVENT_CLIENT_CONNECTED:
    {
        if (!homekit_is_paired())
        {
            ESP_LOGI(TAG, "Client connected... not paired yet");
            // During pairing process suspend the GDO comms loop.  This improves reliability of pairing on ESP8266
            if (comms_setup_done)
            {
                ESP_LOGD(TAG, "Disable comms loop while pairing");
                comms_setup_done = false;
                reset_comms = true;
            }
            // comms loop will be enabled again on any other HomeKit event.
            return;
        }
        else
        {
            ESP_LOGI(TAG, "Client connected... paired");
        }
        break;
    }
    case homekit_event_t::HOMEKIT_EVENT_CLIENT_VERIFIED:
    {
        ESP_LOGI(TAG, "Client verified");
        break;
    }
    case homekit_event_t::HOMEKIT_EVENT_CLIENT_DISCONNECTED:
    {
        ESP_LOGI(TAG, "Client disconnected");
        break;
    }
    case homekit_event_t::HOMEKIT_EVENT_PAIRING_ADDED:
    {
        ESP_LOGI(TAG, "Pairing added");
        break;
    }
    case homekit_event_t::HOMEKIT_EVENT_PAIRING_REMOVED:
    {
        ESP_LOGI(TAG, "Pairing removed");
        break;
    }
    default:
    {
        ESP_LOGI(TAG, "Server unknown event: %d", event);
        break;
    }
    } // end switch
    if (reset_comms)
    {
        ESP_LOGD(TAG, "Re-enable comms loop");
        comms_setup_done = true;
        reset_comms = false;
    }
}

/****************************************************************************
 * Setup HomeKit, non-HomeSpan version.
 */
void setup_homekit()
{
    ESP_LOGI(TAG, "=== Starting HomeKit Server");
    String macAddress = WiFi.macAddress();
    snprintf_P(serial_number, SERIAL_NAME_SIZE, PSTR("%s"), macAddress.c_str());

    current_door_state.getter = current_door_state_get;
    target_door_state.getter = target_door_state_get;
    target_door_state.setter = target_door_state_set;
    obstruction_detected.getter = obstruction_detected_get;
    current_lock_state.getter = current_lock_state_get;
    target_lock_state.getter = target_lock_state_get;
    target_lock_state.setter = target_lock_state_set;
    light_state.getter = light_state_get;
    light_state.setter = light_state_set;

    // Generate a QR Code ID from our MAC address, which should create unique pairing QR codes
    // for each of multiple devices on a network... although we do have to clip to 4 characters,
    // so we loose ~2 most significant bits.
    uint8_t mac[WL_MAC_ADDR_LENGTH];
    WiFi.macAddress(mac);
    uint32_t uid = (mac[3] << 16) + (mac[4] << 8) + mac[5];
    static char HKpassword[] = "251-02-023"; // On Oct 25, 2023, Chamberlain announced they were disabling API
                                             // access for "unauthorized" third parties.
    static char setupID[6];
    toBase62(setupID, sizeof(setupID), uid); // always includes leading zeros
    // setupID will be string "0ABCD" plus null terminator.  We throw away the first char.
    ESP_LOGI(TAG, "HomeKit pairing QR Code ID: %s", &setupID[1]);
    // X-HM://0042WZMX3 + setupID... string is constant, precalculated from 25102023
    // and Category::GarageDoorOpeners in the HomeSpan version of setup code.
    strlcpy(qrPayload, "X-HM://0042WZMX3", sizeof(qrPayload));
    sprintf(&qrPayload[16], "%-4.4s", &setupID[1]);
    ESP_LOGI(TAG, "HomeKit QR setup payload: %s", qrPayload);
    config.password = HKpassword;
    config.setupId = &setupID[1];
    config.on_event = homekit_event;

    garage_door.has_motion_sensor = (bool)read_door_int(nvram_has_motion);
    if (!garage_door.has_motion_sensor && (userConfig->getMotionTriggers() == 0))
    {
        ESP_LOGI(TAG, "Motion Sensor not detected.  Disabling Service");
        config.accessories[0]->services[3] = NULL;
    }
    if (userConfig->getGDOSecurityType() == 3)
    {
        ESP_LOGI(TAG, "Dry contact does not support light control.  Disabling Service");
        config.accessories[0]->services[2] = NULL;
    }

    arduino_homekit_setup(&config);
    homekit_setup_done = true;
}

void homekit_loop()
{
    if (!homekit_setup_done && !comms_status_done)
        return;

    arduino_homekit_loop();
}

homekit_value_t current_door_state_get()
{
    // We cannot sent an illegal value to HomeKit, subsititute with value in valid range
    GarageDoorCurrentState state = (garage_door.current_state == 0xFF) ? CURR_CLOSED : garage_door.current_state;
    ESP_LOGD(TAG, "Get current door state: %s", DOOR_STATE(state));
    return HOMEKIT_UINT8_CPP(state);
}

homekit_value_t target_door_state_get()
{
    // We cannot sent an illegal value to HomeKit, subsititute with value in valid range
    GarageDoorTargetState state = (garage_door.target_state == 0xFF) ? TGT_CLOSED : garage_door.target_state;
    ESP_LOGD(TAG, "Get target door state: %s", DOOR_STATE(state));
    return HOMEKIT_UINT8_CPP(state);
}

void target_door_state_set(const homekit_value_t value)
{
    ESP_LOGD(TAG, "Set door state: %s", DOOR_STATE(value.uint8_value));
    switch (value.uint8_value)
    {
    case TGT_OPEN:
        open_door();
        break;
    case TGT_CLOSED:
        close_door();
        break;
    default:
        ERROR("invalid target door state set requested: %d", value.uint8_value);
        break;
    }
}

homekit_value_t obstruction_detected_get()
{
    ESP_LOGD(TAG, "Get obstruction: %s", (garage_door.obstructed) ? "Obstructed" : "Clear");
    return HOMEKIT_BOOL_CPP(garage_door.obstructed);
}

homekit_value_t current_lock_state_get()
{
    // We cannot sent an illegal value to HomeKit, subsititute with value in valid range
    LockCurrentState state = (garage_door.current_lock == 0xFF) ? LockCurrentState::CURR_UNKNOWN : garage_door.current_lock;
    ESP_LOGD(TAG, "Get current lock state: %s", LOCK_STATE(state));
    return HOMEKIT_UINT8_CPP(state);
}

homekit_value_t target_lock_state_get()
{
    // We cannot sent an illegal value to HomeKit, subsititute with value in valid range
    LockTargetState state = (garage_door.target_lock == 0xFF) ? LockTargetState::TGT_UNLOCKED : garage_door.target_lock;
    ESP_LOGD(TAG, "Get target lock state: %s", LOCK_STATE(state));
    return HOMEKIT_UINT8_CPP(state);
}

void target_lock_state_set(const homekit_value_t value)
{
    ESP_LOGD(TAG, "Set lock state: %d", LOCK_STATE(value.uint8_value));
    set_lock(value.uint8_value);
}

homekit_value_t light_state_get()
{
    ESP_LOGD(TAG, "Get light state: %s", garage_door.light ? "On" : "Off");
    return HOMEKIT_BOOL_CPP(garage_door.light);
}

void light_state_set(const homekit_value_t value)
{
    ESP_LOGD(TAG, "Set light: %s", value.bool_value ? "On" : "Off");
    set_light(value.bool_value);
}

#else // not ESP8266, must be ESP32

/****************************************************************************
 * On ESP32 we use HomeSpan library, this requires completely separate
 * initialization and runtime handling.
 *
 * If making a change to any of the common functions, be sure to check whether
 * the change is required in both implementations.
 */

// Declare the HomeKit accessories
static DEV_GarageDoor *door;
// HK-FC: optional second GarageDoorOpener accessory. nullptr when the
// toggle is OFF (default) — costs only this 4-byte BSS pointer plus the
// vtable + member layout overhead in .rodata.
static DEV_GarageDoorForceClose *forceCloseDoor;
static DEV_Light *light;
static DEV_Motion *motion;
static DEV_Motion *arriving;
static DEV_Motion *departing;
static DEV_Occupancy *vehicle;
static DEV_Light *assistLaser;
static DEV_Occupancy *roomOccupancy;

// Buffer to hold all IPv6 addresses as a single string
char ipv6_addresses[LWIP_IPV6_NUM_ADDRESSES * IP6ADDR_STRLEN_MAX] = {0};

/****************************************************************************
 * Callback functions, notify us of significant events
 */
void wifiBegin(const char *ssid, const char *pw)
{
    if (strEmptyOrSpaces(ssid))
    {
        ESP_LOGE(TAG, "ERROR: Invalid SSID value (%s) boot into soft access point mode", ssid);
        userConfig->set(cfg_softAPmode, true);
        ESP8266_SAVE_CONFIG();
        sync_and_restart();
        return;
    }

    ESP_LOGI(TAG, "Wifi begin for SSID: %s", ssid);
    WiFi.setSleep(WIFI_PS_NONE); // Improves performance, at cost of power consumption
    WiFi.hostname(const_cast<char *>(device_name_rfc952));
    if (userConfig->getEnableIPv6())
    {
        // Enable IPv6 support
        WiFi.enableIPv6();
    }
    if (userConfig->getStaticIP())
    {
        IPAddress ip;
        IPAddress gw;
        IPAddress nm;
        IPAddress dns;
        if (ip.fromString(userConfig->getLocalIP()) &&
            gw.fromString(userConfig->getGatewayIP()) &&
            nm.fromString(userConfig->getSubnetMask()) &&
            dns.fromString(userConfig->getNameserverIP()))
        {
            ESP_LOGI(TAG, "Set static IP: %s, Mask: %s, Gateway: %s, DNS: %s",
                     ip.toString().c_str(), nm.toString().c_str(), gw.toString().c_str(), dns.toString().c_str());
            WiFi.config(ip, gw, nm, dns);
        }
        else
        {
            ESP_LOGI(TAG, "Failed to set static IP address, error parsing addresses");
        }
    }
    WiFi.begin(ssid, pw);

    // Setting power has to be after WiFi.begin()
    if (userConfig->getWifiPower() < 20)
    {
        // Only set WiFi power if set to less than the maximum
        // Our range is 1..20, ESP32 range is 8..84
        wifi_power_t wifiPower = (wifi_power_t)std::min(84, std::max(8, (int)(userConfig->getWifiPower() * 4)));
        if (WiFi.setTxPower(wifiPower))
        {
            ESP_LOGI(TAG, "Setting WiFi TX power to %d", wifiPower);
        }
        else
        {
            ESP_LOGW(TAG, "Failed to set user requested WiFi TX power");
        }
    }
    ESP_LOGI(TAG, "WiFi TX power: %d", (int)WiFi.getTxPower());
}

void connectionCallback(int count)
{
    if (rebooting)
        return;

    ESP_LOGI(TAG, "WiFi established, count: %d, IP: %s, Mask: %s, Gateway: %s, DNS: %s",
             count,
             WiFi.localIP().toString().c_str(),
             WiFi.subnetMask().toString().c_str(),
             WiFi.gatewayIP().toString().c_str(),
             WiFi.dnsIP().toString().c_str());
    ESP_LOGI(TAG, "WiFi SSID %s at %ddBm on channel %d to access point %s", WiFi.SSID().c_str(), WiFi.RSSI(), WiFi.channel(), WiFi.BSSIDstr().c_str());

    if (softAPmode)
        return;

    // IPv4 Config
    userConfig->set(cfg_localIP, WiFi.localIP().toString().c_str());
    userConfig->set(cfg_gatewayIP, WiFi.gatewayIP().toString().c_str());
    userConfig->set(cfg_subnetMask, WiFi.subnetMask().toString().c_str());

    // Only update cfg_nameserverIP if it is an IPv4 address. .dnsIP() can return an IPv6 address if we have one from SLAAC
    // but our user interface only allows for IPv4 DNS server configuration.
    if (WiFi.dnsIP().type() == IPv4)
        userConfig->set(cfg_nameserverIP, WiFi.dnsIP().toString().c_str());

    // With WiFi connected, we can now initialize the rest of our app.
#ifdef USE_GDOLIB
    // start communications with garage door opener
    // for some unknown reason we need to start GDOLIB comms from this callback and
    // not in our regular loop.
    setup_comms();
#endif
    wifi_got_ip = true;
    notify_new_ipv4_address();
}

// WiFi disconnect/reconnect visibility — without these the firmware emits
// nothing when WiFi flaps, which is the most common root cause of HomeKit
// "No Response" reports we can't otherwise explain. With remote syslog
// enabled (syslogEn=true), every drop and re-association is now timestamped
// in the Pi-side log so we can correlate user-visible failures with the
// underlying network state instead of guessing.
// Track timestamp (in seconds since boot) of the last time iOS asked us
// for any characteristic value. If this gets stale (no reads for >5min)
// while WiFi is healthy, the failure is hub-side — iOS isn't even
// trying to talk to us. This is THE single best signal for narrowing
// "No Response" to device-side vs hub-side. Updated by the
// setGetCharacteristicsCallback hook.
static volatile uint32_t hapLastReadSec = 0;

static void hap_get_characteristics_cb(const char *paths)
{
    hapLastReadSec = (uint32_t)(_millis() / 1000);
}

// Pair/unpair real-time event. setPairCallback fires on every HomeKit
// pairing transition, including unexpected unpair-from-iOS — the
// startup HS_PAIRED status only fires once at boot, so without this
// we wouldn't notice if a controller dropped the pairing later.
//
// v40 (audit W11): parameter renamed from `isPaired` to `paired` to stop
// shadowing the file-scope `::isPaired` global. Pre-v40 the assignment
// pattern was missing entirely, AND the parameter shadow would have
// blocked any future fix from working — any `isPaired = isPaired;` would
// have just been a self-assignment of the parameter. Renamed param +
// explicit assignment to the global. The 12 notify_homekit_* gates
// (pre-v40: stuck at the last HS_PAIRED-set value until reboot) now
// observe the live state. pairedControllersCount is cleared by the
// controller-list-changed callback that fires shortly after — no need
// to pre-empt it here (and pairedControllersCount is declared later
// in this file, would be a forward-ref).
static void hap_pair_cb(boolean paired)
{
    ESP_LOGW(TAG, "HomeKit pair state changed: now %s", paired ? "paired" : "UNPAIRED");
    isPaired = paired;
}

// v31 final: captured at setup_homekit so homekit_health_log can
// query loopTask's stack high-water mark from Ticker context (where
// xTaskGetCurrentTaskHandle would return the timer service task
// instead of loopTask). setup_homekit is called from setup() on the
// Arduino loopTask, so xTaskGetCurrentTaskHandle() there returns
// the right handle.
TaskHandle_t loopTaskHandleForHWM = NULL;

// Cached paired-controller count (refreshed by hap_controller_change_cb).
// Avoids iterating homeSpan.controllerListBegin/End from Ticker context
// (which would acquire HomeSpan's internal mutex).
static volatile size_t pairedControllersCount = 0;

// Controller list change — fires when a pairing is added/removed or
// admin status changes. Logs the new count + admin count so timeline
// shows exactly when paired devices appear/disappear.
static void hap_controller_change_cb()
{
    size_t count = 0;
    size_t admins = 0;
    for (auto it = homeSpan.controllerListBegin(); it != homeSpan.controllerListEnd(); ++it) {
        ++count;
        if (it->isAdmin()) ++admins;
    }
    pairedControllersCount = count;
    ESP_LOGD(TAG, "HomeKit controller list changed: %u paired (%u admin)", (unsigned)count, (unsigned)admins);
}

// Periodic HomeKit/WiFi state snapshot — gives the Pi syslog a timeline
// to correlate "No Response" reports against signal degradation, heap
// leaks, or disconnects. 180s cadence balances diagnostic value against
// log volume. Watchdog thresholds are in minutes; this cadence is plenty.
constexpr uint32_t HOMEKIT_HEALTH_INTERVAL_MS = 180000; // 180s
static Ticker homekitHealthTicker;

#ifndef ESP8266
// codebase-audit-20260517-002: homekitHealthTicker is re-armed from two
// task contexts (loopTask + esp_timer task). The Arduino Ticker wrapper's
// _timer pointer write/free is not atomic — spinlock the detach+attach
// pair. Boot-time arm in setup_homekit() is single-threaded — no lock.
static portMUX_TYPE healthTickerMux = portMUX_INITIALIZER_UNLOCKED;
// log-audit-010: adaptive sampler cadence — drops to a 30s interval when
// free heap falls below the watermark, holds fast for 5 min after recovery
// so we don't oscillate. Visibility for transient heap dips that hide
// between the default 180s samples (log-audit found heap_min ~6.4 KB on a
// 180s tick with no obvious upstream pressure event in the prior window).
//
// Threshold rationale:
//   * HEAP_WATERMARK = 20000 — well above SYSLOG_HEAP_FLOOR_BYTES (4096,
//     log.cpp) so we start fast-sampling BEFORE the syslog-floor kicks in
//     and we lose the network fan-out for diagnostic lines.
//   * INTERVAL_FAST_MS = 30000 — 6x finer than the default; still well
//     under any watchdog threshold (minutes).
//   * FAST_DURATION_MS = 300000 — 10 fast samples worth of trailing
//     visibility past recovery; prevents hysteresis flap if heap bounces
//     across the watermark.
constexpr uint32_t HOMEKIT_HEALTH_INTERVAL_FAST_MS = 30000;
// HOMEKIT_HEALTH_HEAP_WATERMARK declared in homekit.h (shared with the
// 1Hz loopTask caller in ratgdo.cpp). (codebase-audit-20260517-003)
constexpr uint32_t HOMEKIT_HEALTH_FAST_DURATION_MS = 300000;
// NOT static — web.cpp's /heap handler externs this for the
// sampler_interval_ms field. Volatile + atomic ops because writes
// happen from esp_timer task (homekit_health_log Ticker callback)
// and reads happen from loopTask (HTTP handler dispatch).
volatile uint32_t currentHealthIntervalMs = HOMEKIT_HEALTH_INTERVAL_MS;
// fastModeEntryMs and lastTickMs are written from BOTH the in-callback
// adaptive block in homekit_health_log() (esp_timer task) AND the .84
// helper homekit_health_arm_fast_mode_if_low() (loopTask). 64-bit stores
// are not atomic on Xtensa at the C-abstract-machine level — torn r/w
// across these two task contexts is possible. All accesses go through
// __atomic_load_n / __atomic_store_n with acquire/release semantics.
// (codebase-audit-20260517-001)
//
// lastTickMs is also zeroed by the helper and the in-callback re-arm
// path so the first sample after a cadence transition reports
// tickDrift=0 instead of garbage against the old-cadence reference
// (log-audit-20260517-002).
static volatile _millis_t fastModeEntryMs = 0;
static volatile _millis_t lastTickMs      = 0;

// log-audit-20260520-001: Ticker arm-failure observability + retry state.
// All three counters are written from BOTH task contexts (loopTask arm-fast
// helper, esp_timer-task adaptive helper, loopTask deferred retry below)
// under healthTickerMux — same critical section as detach+attach. Web /heap
// handler reads them under the same lock. armCount uses uint32 because slow-
// cadence (180s) re-arms can accumulate >1000 over a 7-day uptime; uint16
// would wrap. Heap delta: 4+4+1 = 9 B static.
static volatile uint32_t  homekitHealthTicker_armCount        = 0;
static volatile uint32_t  homekitHealthTicker_lastArmFailedMs = 0;
// NOT static — read directly from service_timer_loop (loopTask 1Hz fast
// path) without the spinlock. Volatile + monotonic transitions make the
// unlocked read race-safe.
volatile bool homekitHealthTicker_armFailed = false;

// log-audit-20260520-001: gate the detach+attach pair when largest free
// block is too small to fit an esp_timer struct (~70 B observed; 256 gives
// allocator-overhead + fragmentation headroom). Hitting this skips the
// cadence change; the existing ticker keeps firing at its prior interval.
constexpr uint32_t HOMEKIT_HEALTH_TICKER_REARM_MIN_BLOCK = 256;
#endif

// Self-healing watchdog. Settings cached at boot + on settings-save
// (via homekit_refresh_watchdog_config) — Ticker callback reads cached
// values, no userConfig mutex inside the callback. Recovery escalation
// when enabled: mDNS refresh first, then WiFi reconnect (~3-5s outage).
// After HK_AUTO_RECOVER_MAX attempts we stop and wait for a HAP read
// — no auto-reboot, that's too disruptive for a daemon to do on its own.
constexpr uint8_t  HK_AUTO_RECOVER_MAX   = 2;
static uint8_t     hkRecoverAttempts     = 0;
static uint8_t     hkLastHintLevel       = 0;     // 0=none, 1=QUIET, 2=STALE, 3=LIKELY_NR
// Require N consecutive healthy ticks (last_hap_read_ago < quietSecs)
// before clearing hkRecoverAttempts. At the 180s tick cadence, N=3 is
// ≥9 min of sustained healthy reads — past typical hub flap intervals.
constexpr uint8_t  HK_HEALTHY_TICKS_TO_RESET = 3;
// log-audit-002: bumped from uint8_t to uint32_t. The counter is now
// always-on (incremented every healthy tick regardless of whether
// auto-recover is enabled or whether a recovery attempt is pending),
// so it can climb above 255 on long healthy uptimes — wrap-to-zero
// would falsely look like "back to unhealthy" in the diag-hk line.
static uint32_t    hkConsecutiveHealthyTicks = 0;

// Cached watchdog config. Refreshed at boot in setup_homekit and on
// settings save via homekit_refresh_watchdog_config(). Read in the
// Ticker callback without taking a mutex.
static volatile bool     hkCfgEnabled       = false;
static volatile uint32_t hkCfgRecoverSecs   = 1800;
static volatile uint32_t hkCfgQuietSecs     = 300;
static volatile uint32_t hkCfgStaleSecs     = 900;
static volatile uint32_t hkCfgLikelyNRSecs  = 1800;
// v34: gate periodic 180s diagnostic state-dump + post-action narration
// behind this toggle. Default OFF. See HK_DIAG_LOG macro below.
static volatile bool     hkCfgVerboseLogs   = false;

// v34: dual-level log macro. When the user has enabled hkVerboseLogs,
// these lines emit at INFO (visible in default config). When OFF (default),
// they emit at DEBUG — invisible at the default INFO log level, but a
// developer can still see them by setting global log level to DEBUG
// without flipping the user toggle. Used for periodic 180s state-dump
// lines and post-action narration. Event-occurred lines (auto-recover
// fired, hint level transitions, pair state changes, WiFi disconnects)
// stay unconditional ESP_LOGW.
#define HK_DIAG_LOG(fmt, ...) \
    do { if (hkCfgVerboseLogs) ESP_LOGI(TAG, fmt, ##__VA_ARGS__); \
         else                  ESP_LOGD(TAG, fmt, ##__VA_ARGS__); } while (0)

void homekit_refresh_watchdog_config()
{
    // v38 (audit W3): release/acquire ordering pair. Writers run on
    // loopTask (settings-save / boot bootstrap); readers run on
    // esp_timer Ticker (homekit_health_log every 180s). Same pattern
    // as cachedAutoClose* in comms.cpp — RELEASE on the LAST store,
    // RELAXED on earlier stores; reader does ACQUIRE on the same flag
    // first then RELAXED on the rest. hkCfgEnabled is the natural
    // ordering anchor because the reader (homekit_health_log) tests
    // it first to gate auto-recover behavior.
    __atomic_store_n(&hkCfgRecoverSecs,  userConfig->getHKAutoRecoverSecs(),  __ATOMIC_RELAXED);
    __atomic_store_n(&hkCfgQuietSecs,    userConfig->getHKHintQuietSecs(),    __ATOMIC_RELAXED);
    __atomic_store_n(&hkCfgStaleSecs,    userConfig->getHKHintStaleSecs(),    __ATOMIC_RELAXED);
    __atomic_store_n(&hkCfgLikelyNRSecs, userConfig->getHKHintLikelyNRSecs(), __ATOMIC_RELAXED);
    __atomic_store_n(&hkCfgVerboseLogs,  userConfig->getHKVerboseLogs(),      __ATOMIC_RELAXED);
    // v23: reset hint-level + recovery-attempts state when thresholds
    // change. Both are only meaningful relative to the active thresholds.
    // Without these resets, lowering the trigger threshold mid-episode
    // would skip the cheap mDNS-refresh attempt (recovery counter still
    // says "1 attempt already used"), and toggling the watchdog off→on
    // would carry over a stale level/counter from before. Wipe the
    // slate so each settings change is a fresh start.
    //
    // v40 (audit W17): MUST be reset BEFORE the RELEASE-store on
    // hkCfgEnabled below. Pre-v40 these were reset AFTER the release-
    // store, so a Ticker tick that observed `hkCfgEnabled=true` (just-
    // refreshed) could still observe stale `hkRecoverAttempts` /
    // `hkLastHintLevel` / `hkConsecutiveHealthyTicks` for one tick.
    // Reordering moves them under the release-store's publication
    // umbrella — when a reader observes the new hkCfgEnabled value,
    // it also observes the freshly-cleared counters.
    hkLastHintLevel             = 0;
    hkRecoverAttempts           = 0;
    hkConsecutiveHealthyTicks   = 0;
    __atomic_store_n(&hkCfgEnabled,      userConfig->getHKAutoRecover(),      __ATOMIC_RELEASE);
    HK_DIAG_LOG("HomeKit watchdog config refreshed: enabled=%d trigger=%us hints=%u/%u/%u",
                (int)__atomic_load_n(&hkCfgEnabled,      __ATOMIC_RELAXED),
                (unsigned)__atomic_load_n(&hkCfgRecoverSecs,  __ATOMIC_RELAXED),
                (unsigned)__atomic_load_n(&hkCfgQuietSecs,    __ATOMIC_RELAXED),
                (unsigned)__atomic_load_n(&hkCfgStaleSecs,    __ATOMIC_RELAXED),
                (unsigned)__atomic_load_n(&hkCfgLikelyNRSecs, __ATOMIC_RELAXED));
}

#ifndef ESP8266
// Forward declaration: homekit_health_update_adaptive_cadence re-arms the
// Ticker with homekit_health_log as the callback, but the callback is
// defined below this helper. (codebase-audit-20260517-005)
static void homekit_health_log();

// log-audit-010: adaptive cadence — react to transient heap dips that
// hide between 180s ticks. Below the watermark we sample every 30s for
// up to 5 min after recovery; otherwise we hold the default 180s.
//
// Context: this runs in the esp_timer task (arduino-esp32 Ticker
// dispatches there — see v43/audit-W18 comment in homekit_health_log).
// detach() + attach_ms() from within the callback re-arms the underlying
// esp_timer via esp_timer_stop / esp_timer_start_periodic. The IDF
// calls are task-safe, but the Ticker wrapper's _timer pointer
// write/free is not — the detach+attach pair below is spinlock-
// guarded (healthTickerMux) against the loopTask helper
// homekit_health_arm_fast_mode_if_low(). (codebase-audit-20260517-002)
//
// All deltas unsigned (uint64_t cast); never (int32_t)(now - past) —
// _millis_t is int64 on ESP32 so a cross-callback delta math bug here
// would not be a 25-day rollover but a stricter signedness audit.
//
// codebase-audit-20260517-005: extracted from homekit_health_log to keep
// the entry-point callback below ~240 LoC. Pairs with the loopTask-side
// homekit_health_arm_fast_mode_if_low() helper. Pure mechanical move —
// runs in the same esp_timer task context as before.

// log-audit-20260520-001: centralizes the guarded detach+attach+verify
// sequence used by both task contexts. Caller must hold healthTickerMux
// across the entire call. Returns true on success, false on arm failure.
static bool homekit_health_ticker_rearm_locked(uint32_t intervalMs)
{
    homekitHealthTicker.detach();
    homekitHealthTicker.attach_ms(intervalMs, homekit_health_log);
    if (homekitHealthTicker.active())
    {
        homekitHealthTicker_armCount++;
        homekitHealthTicker_armFailed = false;
        return true;
    }
    homekitHealthTicker_armFailed       = true;
    homekitHealthTicker_lastArmFailedMs = (uint32_t)_millis();
    return false;
}

static void homekit_health_update_adaptive_cadence(uint32_t freeHeapNow)
{
    _millis_t now = _millis();
    // Single acquire-load snapshot — used for both the duration check
    // and the !=0 guard so we evaluate against one consistent value
    // even if loopTask races a write between the two reads.
    // (codebase-audit-20260517-001)
    _millis_t fastModeEntry_snapshot = __atomic_load_n(&fastModeEntryMs, __ATOMIC_ACQUIRE);
    uint32_t desired;
    if (freeHeapNow < HOMEKIT_HEALTH_HEAP_WATERMARK)
    {
        // Below watermark — (re-)enter fast mode and refresh the
        // hold timer. Re-arming on every tick while we're under
        // pressure means recovery is measured from the LAST dip,
        // not the first.
        __atomic_store_n(&fastModeEntryMs, now, __ATOMIC_RELEASE);
        desired = HOMEKIT_HEALTH_INTERVAL_FAST_MS;
    }
    else if (fastModeEntry_snapshot != 0 &&
             (uint64_t)(now - fastModeEntry_snapshot) < (uint64_t)HOMEKIT_HEALTH_FAST_DURATION_MS)
    {
        // Heap recovered but we're still inside the trailing-visibility
        // window. Hold fast.
        desired = HOMEKIT_HEALTH_INTERVAL_FAST_MS;
    }
    else
    {
        // Fully out of fast mode — clear the entry timestamp so the
        // next dip cleanly re-triggers the fast-mode window from
        // its own first-tick timestamp.
        __atomic_store_n(&fastModeEntryMs, (_millis_t)0, __ATOMIC_RELEASE);
        desired = HOMEKIT_HEALTH_INTERVAL_MS;
    }

    uint32_t current = __atomic_load_n(&currentHealthIntervalMs, __ATOMIC_RELAXED);
    if (desired != current)
    {
        HK_DIAG_LOG("HomeKit health sampler cadence: %lu ms -> %lu ms (free_heap=%lu)",
                    (unsigned long)current,
                    (unsigned long)desired,
                    (unsigned long)freeHeapNow);
        __atomic_store_n(&currentHealthIntervalMs, desired, __ATOMIC_RELAXED);
        // Re-arm under the new cadence. Safe from within the
        // callback (see context note above). Zero lastTickMs so the
        // first sample after the transition reports tickDrift=0
        // rather than (newInterval - oldInterval) garbage.
        // (log-audit-20260517-002)
        __atomic_store_n(&lastTickMs, (_millis_t)0, __ATOMIC_RELEASE);
        // log-audit-20260520-001: skip the re-arm if the largest free block
        // can't fit an esp_timer struct — failed esp_timer_create would leave
        // the Ticker dead. Better to keep firing at the old interval and
        // re-evaluate next callback.
        size_t lfb = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        if (lfb < HOMEKIT_HEALTH_TICKER_REARM_MIN_BLOCK)
        {
            ESP_LOGW(TAG, "HomeKit health: skipping cadence re-arm (largest_free=%uB < %uB), keeping interval=%lums",
                     (unsigned)lfb,
                     (unsigned)HOMEKIT_HEALTH_TICKER_REARM_MIN_BLOCK,
                     (unsigned long)current);
            __atomic_store_n(&currentHealthIntervalMs, current, __ATOMIC_RELAXED);
        }
        else
        {
            taskENTER_CRITICAL(&healthTickerMux);
            bool ok = homekit_health_ticker_rearm_locked(desired);
            taskEXIT_CRITICAL(&healthTickerMux);
            if (!ok)
            {
                ESP_LOGE(TAG, "HomeKit health: Ticker arm FAILED (in-callback adaptive, desired=%lums, largest_free=%uB) — deferred-retry armed",
                         (unsigned long)desired, (unsigned)lfb);
                __atomic_store_n(&currentHealthIntervalMs, current, __ATOMIC_RELAXED);
            }
        }
    }
}
#endif

static void homekit_health_log()
{
    if (rebooting) return;
    // v55: bail early during OTA. helperUpdateUnderway calls
    // "Shutdown HomeKit and GDO communications" which tears down
    // HomeSpan tasks (freeing the autoPoll task TCB). This Ticker
    // keeps firing every 3 min on a separate timer. If it fires
    // mid-OTA, homeSpan.getAutoPollTask() at line 677 returns a
    // stale pointer to the freed TCB and the subsequent
    // uxTaskGetStackHighWaterMark dereferences it → LoadProhibited
    // panic in esp_timer task. Verified via the v52 crash log:
    // crashed at 80% upload, addr2line resolved to homekit.cpp:679
    // calling prvTaskCheckFreeStackSpace on the stale apTask.
    // Same pattern as the audit's W20 fix that defers other drains
    // during OTA. Health logging resumes naturally after the
    // post-OTA reboot.
    if (firmware_update_in_progress()) return;
    int rssi = WiFi.isConnected() ? WiFi.RSSI() : 0;
    const char *wifiState = WiFi.isConnected() ? "connected" : "disconnected";
    // v24: read the cached count instead of iterating HomeSpan's list
    // from Ticker context (avoids second mutex surface).
    size_t paired_controllers = pairedControllersCount;
    uint32_t nowSec = (uint32_t)(_millis() / 1000);
    // If hapLastReadSec is 0 we've never seen a read since boot.
    // Otherwise log seconds since last read — the smoking gun for
    // "No Response" diagnosis. If this number keeps growing while WiFi
    // is connected and paired_controllers > 0, the hub stopped talking.
    int32_t lastReadAgo = hapLastReadSec ? (int32_t)(nowSec - hapLastReadSec) : -1;
    // Instrumentation snapshot for the periodic diag log:
    //   logMtxMaxWaitMs : max log mutex wait this 180s window (climbing
    //                     pre-freeze = wedged SSE subscriber blocking
    //                     the broadcast)
    //   sseSlowWrites   : SSE writes > CLIENT_SLOW_WRITE_MS since boot
    //   tickDriftMs     : cadence drift vs expected 180s (positive growth
    //                     = esp_timer task starved)
    //   maxAllocBlock   : largest contiguous heap (gap from freeHeap =
    //                     fragmentation buildup)
    // lastTickMs is file-scope (volatile _millis_t, see declaration near
    // currentHealthIntervalMs) so the .84 helper and the in-callback
    // re-arm path can zero it after a cadence transition. The drift
    // expected-interval is read from the currently-armed cadence rather
    // than the static slow-mode constant — otherwise the first sample
    // after a fast-mode arm reports a -150000ms artifact
    // (30000 actual - 180000 expected). (log-audit-20260517-002)
    int32_t tickDriftMs = 0;
    _millis_t lastTickSnapshot = __atomic_load_n(&lastTickMs, __ATOMIC_ACQUIRE);
    if (lastTickSnapshot) {
        uint32_t armedInterval = __atomic_load_n(&currentHealthIntervalMs, __ATOMIC_RELAXED);
        tickDriftMs = (int32_t)((int64_t)(_millis() - lastTickSnapshot) - (int64_t)armedInterval);
    }
    __atomic_store_n(&lastTickMs, _millis(), __ATOMIC_RELEASE);
    // Instrumentation counters (W41: declarations in src/instrumentation.h):
    //   logMtxMaxWaitMs      : max log mutex wait, see log.cpp
    //   sseSlowWrites        : SSE writes > CLIENT_SLOW_WRITE_MS since boot
    //   sseBufferFullSkips   : cumulative lwIP-send-buffer-full skips since
    //                          boot (flow-control diagnostic; trend matters
    //                          more than absolute).
    //   sseSlotsAlloc        : live count refreshed by sweep_sse_orphans.
    //   sseOrphansReaped     : per-window counter, atomic-exchange-zeroed
    //                          below.
    //   statusJsonPeakLen    : MH6 — peak JSON length this window. Inform
    //                          future STATUS_JSON_BUFFER_SIZE retune
    //                          decision in v34.
    uint32_t mtxWait     = __atomic_exchange_n(&logMtxMaxWaitMs,    0u, __ATOMIC_RELAXED);
    uint32_t sseReaped   = __atomic_exchange_n(&sseOrphansReaped,   0u, __ATOMIC_RELAXED);
    uint32_t jsonPeak    = __atomic_exchange_n(&statusJsonPeakLen,  0u, __ATOMIC_RELAXED);
    uint32_t sseAlloc = sseSlotsAlloc;
#ifndef ESP8266
    size_t maxAllocBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
#else
    size_t maxAllocBlock = 0; // ESP8266: heap_caps API not available
#endif
    // Stack high-water-mark per task (BYTES — ESP-IDF wrapper returns
    // bytes natively, unlike vanilla FreeRTOS which returns words).
    // Trending toward zero indicates near-overflow. loopTask handle
    // captured at setup_homekit so this Ticker-context query gets the
    // right task (NULL would return the timer service task = us).
    uint32_t loopHWM   = loopTaskHandleForHWM
                          ? (uint32_t)uxTaskGetStackHighWaterMark(loopTaskHandleForHWM)
                          : 0;
    // v43 (audit W18): arduino-esp32 Ticker dispatches via esp_timer task —
    // current task IS the timer task here. xTaskGetHandle("Tmr Svc") still
    // resolves but points at the FreeRTOS Tmr Svc daemon, which on
    // arduino-esp32 has minimal traffic; the HWM logged here would be that
    // unrelated task's HWM, not the esp_timer task running this Ticker.
    // Keep the variable name and the `tmrHWM=` log key to avoid breaking
    // external grep/log-tooling.
    TaskHandle_t tmrSvc = xTaskGetCurrentTaskHandle();
    uint32_t tmrSvcHWM  = tmrSvc
                          ? (uint32_t)uxTaskGetStackHighWaterMark(tmrSvc)
                          : 0;
    TaskHandle_t apTask = homeSpan.getAutoPollTask();
    uint32_t apHWM      = apTask
                          ? (uint32_t)uxTaskGetStackHighWaterMark(apTask)
                          : 0;

    // v31: split into two ESP_LOGI calls because the combined line
    // exceeds LINE_BUFFER_SIZE=256 (the original line was already
    // being truncated mid-token at sseOrphansReaped). Line 1 keeps
    // the wifi/heap/uptime/HomeKit-state snapshot; line 2 carries
    // the SSE + watchdog + stack diagnostics. Both well under 256.
    HK_DIAG_LOG("HomeKit health: wifi=%s rssi=%ddBm heap=%lu maxBlock=%lu uptime=%us paired=%s controllers=%u last_hap_read_ago=%ds",
                wifiState,
                rssi,
                (unsigned long)esp_get_free_heap_size(),
                (unsigned long)maxAllocBlock,
                nowSec,
                isPaired ? "yes" : "no",
                (unsigned)paired_controllers,
                lastReadAgo);
    // Split into two diag lines (combined > LINE_BUFFER_SIZE=256).
    // diag-sse: SSE pipeline + log-mutex pressure indicators.
    // diag-hk:  watchdog state + stack HWMs + tick cadence drift.
    HK_DIAG_LOG("HomeKit diag-sse: logMtxMaxWait=%ums sseSlowWrites=%u sseBufferFullSkips=%u sseSlotsAlloc=%u sseOrphansReaped=%u jsonPeak=%uB",
                (unsigned)mtxWait,
                (unsigned)sseSlowWrites,
                (unsigned)sseBufferFullSkips,
                (unsigned)sseAlloc,
                (unsigned)sseReaped,
                (unsigned)jsonPeak);
    // v.92 adds `minFreeEver` — `esp_get_minimum_free_heap_size()` is
    // the IDF intrinsic all-time lowest free-heap reading, updated by
    // the allocator on every alloc/free with no sampling resolution
    // limit. The 1Hz Ticker can miss sub-second spikes (e.g. TLS
    // handshake transients) that the intrinsic does NOT miss — `/heap`
    // already exposes it, but plotting the diag-hk timeseries lets us
    // watch it for slope vs steady-state without HTTP polling. If the
    // device panics in `tiT` again (log-audit-20260527-001 / #156:
    // heap-OOM via lwIP memp_malloc since arduino-esp32 lwIP runs
    // with MEMP_MEM_MALLOC=1, so all "lwIP pools" come from the
    // regular heap), the next pre-crash diag-hk line tells us how
    // close we were already running to zero.
    uint32_t minFreeEver  = 0;
    int32_t  minFreeDelta = 0;
#ifndef ESP8266
    minFreeEver = (uint32_t)esp_get_minimum_free_heap_size();
    // v.93 (log-audit-20260527-001 / #156): minFreeEver is the all-time
    // heap floor — it only ever decreases. The signed delta vs the prior
    // diag sample is therefore <= 0: a NEGATIVE value means the floor
    // dropped FURTHER this window (the general heap that lwIP's sys_timeo
    // mallocs draw from is still grazing deeper toward the tiT OOM), ZERO
    // means it has plateaued. Watching this slope across the diag-hk
    // timeseries distinguishes "still leaking" from "bounded" WITHOUT
    // waiting for a rare panic — the gap .92's one-shot minFreeEver left
    // open. Unsigned subtraction, cast to signed only for display (same
    // wrap-safe convention as tickDrift). This function runs only in the
    // esp_timer (Ticker) task, so lastMinFreeEver needs no cross-task
    // atomic. First sample (lastMinFreeEver==0) reports 0 to avoid a
    // bogus first-window jump.
    static uint32_t lastMinFreeEver = 0;
    if (lastMinFreeEver != 0)
    {
        minFreeDelta = (int32_t)(minFreeEver - lastMinFreeEver);
    }
    lastMinFreeEver = minFreeEver;
#endif
    HK_DIAG_LOG("HomeKit diag-hk: recoverAttempts=%u hintLevel=%u hkHealthyTicks=%u loopHWM=%uB tmrHWM=%uB apHWM=%uB tickDrift=%dms minFreeEver=%uB minFreeDelta=%dB",
                (unsigned)hkRecoverAttempts,
                (unsigned)hkLastHintLevel,
                (unsigned)hkConsecutiveHealthyTicks,
                (unsigned)loopHWM,
                (unsigned)tmrSvcHWM,
                (unsigned)apHWM,
                (int)tickDriftMs,
                (unsigned)minFreeEver,
                (int)minFreeDelta);

    // Self-healing watchdog. Trigger only when:
    //   * we've seen a HAP read at least once (lastReadAgo > 0) — so
    //     we don't fire on a brand-new boot with no controllers paired
    //   * iOS has been quiet longer than the recovery threshold
    //   * WiFi is up and we have at least one paired controller
    //   * we haven't already exhausted recovery attempts this episode
    // Recovery escalation: mDNS refresh first (cheapest, ~1s, no
    // outage), then WiFi reconnect (heavier, ~3-5s outage). After
    // HK_AUTO_RECOVER_MAX attempts we stop and wait for a HAP read
    // (which resets the counter) — no auto-reboot, that's too
    // disruptive for a daemon to do on its own.
    // v22: read cached values (refreshed at boot + on settings-save)
    // instead of taking the userConfig mutex inside this Ticker callback.
    // v38 (audit W3): ACQUIRE on hkCfgEnabled pairs with the RELEASE
    // store in homekit_refresh_watchdog_config. Synchronizes-with the
    // four RELAXED stores from the writer; the rest of these reads
    // can be RELAXED.
    const bool     hkEnabled      = __atomic_load_n(&hkCfgEnabled,      __ATOMIC_ACQUIRE);
    const uint32_t hkRecoverSecs  = __atomic_load_n(&hkCfgRecoverSecs,  __ATOMIC_RELAXED);
    const uint32_t hkQuietSecs    = __atomic_load_n(&hkCfgQuietSecs,    __ATOMIC_RELAXED);
    const uint32_t hkStaleSecs    = __atomic_load_n(&hkCfgStaleSecs,    __ATOMIC_RELAXED);
    const uint32_t hkLikelyNRSecs = __atomic_load_n(&hkCfgLikelyNRSecs, __ATOMIC_RELAXED);

    // Tiered diagnostic hints — ALWAYS logged regardless of whether
    // auto-recover is enabled. Lets the user observe how silent
    // their iOS hub gets during normal operation, so they can pick
    // an informed threshold before enabling auto-recover.
    if (lastReadAgo > 0 && paired_controllers > 0)
    {
        uint8_t newLevel = 0;
        if (lastReadAgo > (int32_t)hkLikelyNRSecs)     newLevel = 3;
        else if (lastReadAgo > (int32_t)hkStaleSecs)   newLevel = 2;
        else if (lastReadAgo > (int32_t)hkQuietSecs)   newLevel = 1;

        // Only emit a hint when crossing INTO a higher level (don't
        // spam every 60s while sitting at the same level).
        if (newLevel > hkLastHintLevel)
        {
            const char *label = "";
            switch (newLevel) {
                case 1: label = "iOS extended idle — could be normal hub idle, watch the trend"; break;
                case 2: label = "iOS gone quiet — possibly stale, hub may be drifting toward No Response"; break;
                case 3: label = "iOS silent — likely No Response on hub side, manual Reconnect or Refresh mDNS may help"; break;
            }
            ESP_LOGW(TAG, "HomeKit watchdog hint: %s (last_hap_read_ago=%ds, threshold-level=%u)",
                     label, lastReadAgo, newLevel);
            hkLastHintLevel = newLevel;
        }
        else if (newLevel < hkLastHintLevel && lastReadAgo < (int32_t)hkQuietSecs)
        {
            // Crossed back below the lowest hint threshold — quiet phase ended.
            ESP_LOGI(TAG, "HomeKit watchdog hint: HAP reads resumed (last_hap_read_ago=%ds), recovering from level %u", lastReadAgo, hkLastHintLevel);
            hkLastHintLevel = 0;
        }
    }

    // log-audit-002: track consecutive healthy ticks UNCONDITIONALLY
    // (independent of hkAutoRecover and of any active recovery state).
    // Pre-fix the increment was nested inside `else if (hkRecoverAttempts > 0)`
    // — with auto-recover disabled (default), `hkRecoverAttempts` stays 0
    // forever, so the counter never moved and the diag-hk line falsely
    // reported `hkHealthyTicks=0` even when HomeKit was clearly working.
    // The recover-counter clear logic still gates on `hkRecoverAttempts > 0`
    // (no-op when there's nothing to clear), but the streak is always
    // observable in diag-hk now.
    const bool healthyTick = (lastReadAgo > 0 && lastReadAgo < (int32_t)hkQuietSecs);
    if (healthyTick)
    {
        hkConsecutiveHealthyTicks++;
    }
    else
    {
        hkConsecutiveHealthyTicks = 0;
    }

    // Auto-recover ACTIONS — only run if explicitly enabled. Defaults
    // ship disabled; the hints above run unconditionally so the user
    // can decide whether to enable based on real-world data.
    // F5: inhibited during OTA — a WiFi cycle mid-upload aborts the
    // transfer and falls into the rollback path.
    if (hkEnabled &&
        !firmware_update_in_progress() &&
        lastReadAgo > (int32_t)hkRecoverSecs &&
        WiFi.isConnected() &&
        paired_controllers > 0)
    {
        if (hkRecoverAttempts == 0)
        {
            ESP_LOGW(TAG, "HomeKit auto-recover (1/2): no HAP read in %ds, requesting mDNS refresh", lastReadAgo);
            // v31: defer through main-loop drain. updateDatabase() from
            // esp_timer task is the same anti-pattern audit #7b flags
            // for the web handlers.
            homekit_request_refresh_mdns(DEFERRED_REASON_WATCHDOG);
            hkRecoverAttempts = 1;
        }
        else if (hkRecoverAttempts == 1)
        {
            ESP_LOGW(TAG, "HomeKit auto-recover (2/2): mDNS refresh didn't help, cycling WiFi (last_hap_read_ago=%ds)", lastReadAgo);
            // v24: defer the cycle — homekit_force_reconnect blocks
            // ~750ms which would stall every other Ticker callback.
            homekit_request_reconnect(DEFERRED_REASON_WATCHDOG);
            hkRecoverAttempts = 2;
        }
        else
        {
            ESP_LOGW(TAG, "HomeKit auto-recover: still no HAP read after %d attempts; giving up (user reboot may be required)", hkRecoverAttempts);
        }
    }
    else if (hkRecoverAttempts > 0 && hkConsecutiveHealthyTicks >= HK_HEALTHY_TICKS_TO_RESET)
    {
        // v31: require N consecutive healthy ticks before clearing the
        // recovery counter. A single sporadic read inside the trailing
        // window of one tick (pre-v31 logic) is not enough — a flapping
        // hub could re-arm the watchdog indefinitely.
        // log-audit-002: streak is now tracked above, independent of this
        // branch. Don't reset the streak counter on clear — it keeps
        // representing "consecutive healthy ticks" for diag visibility.
        ESP_LOGI(TAG, "HomeKit auto-recover: %u consecutive healthy ticks (last_hap_read_ago=%ds), clearing recovery counter",
                 (unsigned)hkConsecutiveHealthyTicks, lastReadAgo);
        hkRecoverAttempts = 0;
    }

#ifndef ESP8266
    // Adaptive cadence decision — extracted into a helper above to keep
    // this callback under ~240 LoC. (codebase-audit-20260517-005)
    homekit_health_update_adaptive_cadence(esp_get_free_heap_size());
#endif
}

#ifndef ESP8266
// log-audit-010 follow-up: 1Hz heap-watermark trigger. The .83 adaptive
// sampler only re-evaluates cadence at the 180s slow-mode tick — so a
// sub-180s heap dip (observed on .83 at 2026-05-16: free heap reached
// 3424 B and recovered before the next 180s sample) never arms fast
// mode. This helper is called from service_timer_loop on loopTask at
// 1Hz and arms fast cadence IMMEDIATELY when freeHeap < watermark,
// independent of the slow-mode poll. The existing in-callback adaptive
// block above continues to handle the "stay fast 5 min after recovery,
// then revert to slow" logic on each fast-mode sample — this helper
// only covers the *entry* path.
//
// Context: called from loopTask. Ticker detach/attach_ms calls into
// esp_timer_stop / esp_timer_start_periodic, which are safe from any
// task context (they take the esp_timer service lock). The Ticker
// wrapper's _timer pointer write/free is NOT atomic, however — the
// detach+attach pair below is spinlock-guarded (healthTickerMux)
// against the esp_timer-task in-callback re-arm. Both writers also
// use __atomic_store_n on currentHealthIntervalMs so there is no torn
// read for the /heap handler. (codebase-audit-20260517-002)
void homekit_health_arm_fast_mode_if_low(uint32_t freeHeap, uint32_t maxBlock)
{
    if (freeHeap >= HOMEKIT_HEALTH_HEAP_WATERMARK)
        return;
    uint32_t interval = __atomic_load_n(&currentHealthIntervalMs, __ATOMIC_RELAXED);
    if (interval == HOMEKIT_HEALTH_INTERVAL_FAST_MS)
    {
        // Already in fast mode — refresh the hold timer so the trailing
        // 5-min visibility window is measured from the LAST dip rather
        // than the first. Mirrors the in-callback "below watermark"
        // branch behavior. Atomic store for the cross-task writer.
        // (codebase-audit-20260517-001)
        __atomic_store_n(&fastModeEntryMs, _millis(), __ATOMIC_RELEASE);
        return;
    }
    // Heap below watermark in slow mode — arm fast cadence NOW, don't
    // wait for the next 180s slow-mode sample.
    __atomic_store_n(&fastModeEntryMs, _millis(), __ATOMIC_RELEASE);
    __atomic_store_n(&currentHealthIntervalMs, HOMEKIT_HEALTH_INTERVAL_FAST_MS, __ATOMIC_RELAXED);
    HK_DIAG_LOG("HomeKit health: heap-watermark trigger (free=%u maxBlock=%u < %u), arming fast cadence (%lu ms -> %lu ms)",
                (unsigned)freeHeap, (unsigned)maxBlock,
                (unsigned)HOMEKIT_HEALTH_HEAP_WATERMARK,
                (unsigned long)HOMEKIT_HEALTH_INTERVAL_MS,
                (unsigned long)HOMEKIT_HEALTH_INTERVAL_FAST_MS);
    // Zero lastTickMs so the FIRST sample after this cadence transition
    // reports tickDrift=0 instead of a stale-reference artifact (the
    // existing if (lastTickSnapshot) guard in homekit_health_log()
    // handles the zero case correctly). (log-audit-20260517-002)
    __atomic_store_n(&lastTickMs, (_millis_t)0, __ATOMIC_RELEASE);
    // log-audit-20260520-001: heap-pressure-guarded re-arm. This site fires
    // PRECISELY when free heap < 20KB — the highest-risk arm in the codebase.
    if (maxBlock < HOMEKIT_HEALTH_TICKER_REARM_MIN_BLOCK)
    {
        ESP_LOGW(TAG, "HomeKit health: skipping fast-cadence re-arm (largest_free=%uB < %uB), staying at slow interval",
                 (unsigned)maxBlock,
                 (unsigned)HOMEKIT_HEALTH_TICKER_REARM_MIN_BLOCK);
        __atomic_store_n(&currentHealthIntervalMs, HOMEKIT_HEALTH_INTERVAL_MS, __ATOMIC_RELAXED);
        return;
    }
    taskENTER_CRITICAL(&healthTickerMux);
    bool ok = homekit_health_ticker_rearm_locked(HOMEKIT_HEALTH_INTERVAL_FAST_MS);
    taskEXIT_CRITICAL(&healthTickerMux);
    if (!ok)
    {
        ESP_LOGE(TAG, "HomeKit health: Ticker arm FAILED (loopTask fast-mode entry, largest_free=%uB) — deferred-retry armed",
                 (unsigned)maxBlock);
        __atomic_store_n(&currentHealthIntervalMs, HOMEKIT_HEALTH_INTERVAL_MS, __ATOMIC_RELAXED);
    }
}

// log-audit-20260520-001: deferred-retry path for Ticker arm failures.
// Called from service_timer_loop on loopTask at 1Hz. No-op when armed
// healthy. Idempotent.
void homekit_health_retry_arm_if_failed(uint32_t freeHeap, uint32_t maxBlock)
{
    if (!homekitHealthTicker_armFailed)
        return;
    if (maxBlock < HOMEKIT_HEALTH_TICKER_REARM_MIN_BLOCK)
        return;
    uint32_t intervalToArm = __atomic_load_n(&currentHealthIntervalMs, __ATOMIC_RELAXED);

    // Rate-limit the retry-attempt warning to once per minute — the
    // retry itself runs every 1Hz, but emitting that log at 1Hz under
    // sustained heap pressure would flood syslog. Unsigned subtraction
    // is wraparound-safe per the codebase's unsigned-time-math convention.
    static uint32_t lastRetryLoggedMs = 0;
    uint32_t nowMs = (uint32_t)_millis();
    if (lastRetryLoggedMs == 0 || (nowMs - lastRetryLoggedMs) >= 60000U)
    {
        ESP_LOGW(TAG, "HomeKit health: retrying Ticker arm (prior failure at %ums, free=%uB, largest_free=%uB, interval=%lums)",
                 (unsigned)homekitHealthTicker_lastArmFailedMs,
                 (unsigned)freeHeap,
                 (unsigned)maxBlock,
                 (unsigned long)intervalToArm);
        lastRetryLoggedMs = nowMs;
    }

    taskENTER_CRITICAL(&healthTickerMux);
    bool ok = homekit_health_ticker_rearm_locked(intervalToArm);
    taskEXIT_CRITICAL(&healthTickerMux);
    if (ok)
    {
        ESP_LOGI(TAG, "HomeKit health: Ticker re-armed successfully after prior failure");
        lastRetryLoggedMs = 0;  // reset so the next failure cycle logs immediately
    }
}

// log-audit-20260520-001: spinlock-protected accessor for /heap handler.
void homekit_health_ticker_get_status(bool *outActive, uint32_t *outArmCount, uint32_t *outLastFailedMs)
{
    taskENTER_CRITICAL(&healthTickerMux);
    bool a = homekitHealthTicker.active();
    uint32_t c = homekitHealthTicker_armCount;
    uint32_t f = homekitHealthTicker_lastArmFailedMs;
    taskEXIT_CRITICAL(&healthTickerMux);
    if (outActive)        *outActive        = a;
    if (outArmCount)      *outArmCount      = c;
    if (outLastFailedMs)  *outLastFailedMs  = f;
}
#endif

void WiFiStaDisconnected(WiFiEvent_t event, WiFiEventInfo_t info)
{
    // ESP-IDF reason codes — most useful ones called out by name; rest
    // fall through to numeric so we can look them up if they're new.
    const uint8_t reason = info.wifi_sta_disconnected.reason;
    const char *why = "unknown";
    switch (reason)
    {
        case WIFI_REASON_AUTH_EXPIRE:        why = "auth expired"; break;
        case WIFI_REASON_AUTH_LEAVE:         why = "deauth (we left)"; break;
        case WIFI_REASON_ASSOC_EXPIRE:       why = "assoc expired"; break;
        case WIFI_REASON_ASSOC_TOOMANY:      why = "AP too many clients"; break;
        case WIFI_REASON_NOT_AUTHED:         why = "not authed"; break;
        case WIFI_REASON_NOT_ASSOCED:        why = "not assoced"; break;
        case WIFI_REASON_ASSOC_LEAVE:        why = "assoc leave"; break;
        case WIFI_REASON_ASSOC_NOT_AUTHED:   why = "assoc not authed"; break;
        case WIFI_REASON_BEACON_TIMEOUT:     why = "beacon timeout (AP gone)"; break;
        case WIFI_REASON_NO_AP_FOUND:        why = "AP not found"; break;
        case WIFI_REASON_AUTH_FAIL:          why = "auth fail"; break;
        case WIFI_REASON_ASSOC_FAIL:         why = "assoc fail"; break;
        case WIFI_REASON_HANDSHAKE_TIMEOUT:  why = "handshake timeout"; break;
        default: break;
    }
    ESP_LOGW(TAG, "WiFi disconnected: reason=%d (%s); HomeSpan will auto-reconnect", reason, why);
}

void WiFiStaConnected(WiFiEvent_t event, WiFiEventInfo_t info)
{
    ESP_LOGD(TAG, "WiFi (re)connected to AP — waiting for IP");
}

// Lighter-touch HomeKit recovery — re-advertises mDNS via HomeSpan's
// updateDatabase(true). Doesn't cycle WiFi, doesn't drop HAP TCP.
// First thing to try when the iOS hub says "No Response" but device-side
// is healthy in the syslog — often this is just stale mDNS. Roughly
// 1-2 seconds of disruption vs ~3-5s for the WiFi cycle vs ~25s for
// a full reboot.
void homekit_refresh_mdns(const char *reason)
{
    ESP_LOGW(TAG, "HomeKit mDNS refresh requested (%s) — re-broadcasting accessory advert",
             reason ? reason : "unspecified");
    // updateDatabase(true) bumps the HAP config number, calls
    // updateMDNS (which re-advertises), and triggers HomeSpan's
    // internal database update. Safe to call when nothing has actually
    // changed in the accessory tree — controllers will see the same
    // config number on a no-op and ignore the re-fetch.
    homeSpan.updateDatabase(true);
    HK_DIAG_LOG("HomeKit mDNS refresh complete");
}

// Programmatic invocation of HomeSpan's diagnostic CLI commands. Lets us
// dump full HomeSpan state to the syslog from a web button without
// requiring a USB serial cable. Only read-only commands ('s' status,
// 'i' accessory database, 'd' diagnostics) — no 'P' (pairing data is
// sensitive), no state-changing commands.
//
// R-?-fork: these three commands are READ-ONLY in HomeSpan's
// processSerialCommand switch (they LOG0 internal state without
// mutating it). HomeSpan's pollTask holds `pollMutex` (a
// std::shared_mutex) for the entire iteration body and could in
// principle update the state we read here mid-iteration, producing
// a torn read in the LOG0 output. Cosmetic only — no functional
// impact, no crash risk. We deliberately do NOT take
// `homeSpan.getMutex()` around these calls: pollTask iterations can
// take seconds (HAP transactions, mDNS queries), and waiting on the
// mutex from loopTask context could trip the loop watchdog.
// Caller is `homekit_drain_pending_state_dump` (loopTask, deferred
// via flag from web/Ticker contexts).
void homekit_dump_state(const char *reason)
{
    ESP_LOGW(TAG, "HomeSpan state dump requested (%s) — running CLI commands s, i, d", reason ? reason : "unspecified");
    // 's' — overall status (WiFi, pair, config number)
    homeSpan.processSerialCommand("s");
    // 'i' — accessory database with IIDs/values/permissions
    homeSpan.processSerialCommand("i");
    // 'd' — operational state diagnostics
    homeSpan.processSerialCommand("d");
    HK_DIAG_LOG("HomeSpan state dump complete");
}

// Deferred HomeSpan-API request flags. Web handlers and the watchdog
// Ticker set the request; loopTask drains and runs the actual call
// (force_reconnect ~750ms blocking, updateDatabase ~100ms,
// processSerialCommand ×3 ~1-2s). Each pair is { reason, flag } —
// reason is written FIRST, then flag is set with __ATOMIC_RELEASE
// so the drain's __ATOMIC_ACQUIRE load of the flag synchronizes-with
// a non-stale reason read. Single-byte volatile enum is atomic on
// Xtensa; last-writer-wins on collision.
static volatile HomekitDeferredReason reconnectHKReason     = DEFERRED_REASON_UNSPECIFIED;
static volatile bool                  reconnectHKRequested  = false;
static volatile HomekitDeferredReason refreshMdnsReason     = DEFERRED_REASON_UNSPECIFIED;
static volatile bool                  refreshMdnsRequested  = false;
static volatile HomekitDeferredReason dumpStateReason       = DEFERRED_REASON_UNSPECIFIED;
static volatile bool                  dumpStateRequested    = false;

static const char *deferredReasonString(HomekitDeferredReason r)
{
    switch (r)
    {
        case DEFERRED_REASON_WEB_UI:    return "via web UI";
        case DEFERRED_REASON_WATCHDOG:  return "watchdog auto-recover";
        default:                         return "unspecified (deferred)";
    }
}

void homekit_request_reconnect(HomekitDeferredReason reason)
{
    reconnectHKReason = reason;
    __atomic_store_n(&reconnectHKRequested, true, __ATOMIC_RELEASE);
}

void homekit_request_refresh_mdns(HomekitDeferredReason reason)
{
    refreshMdnsReason = reason;
    __atomic_store_n(&refreshMdnsRequested, true, __ATOMIC_RELEASE);
}

void homekit_request_dump_state(HomekitDeferredReason reason)
{
    dumpStateReason = reason;
    __atomic_store_n(&dumpStateRequested, true, __ATOMIC_RELEASE);
}

// Drains run on loopTask via service_timer_loop. Acquire-load of the
// flag pairs with the request-side release-store so the reason read
// is never stale relative to the flag.
void homekit_drain_pending_reconnect()
{
    if (!__atomic_load_n(&reconnectHKRequested, __ATOMIC_ACQUIRE)) return;
    HomekitDeferredReason r = reconnectHKReason;
    __atomic_store_n(&reconnectHKRequested, false, __ATOMIC_RELEASE);
    homekit_force_reconnect(deferredReasonString(r));
}

void homekit_drain_pending_mdns_refresh()
{
    if (!__atomic_load_n(&refreshMdnsRequested, __ATOMIC_ACQUIRE)) return;
    HomekitDeferredReason r = refreshMdnsReason;
    __atomic_store_n(&refreshMdnsRequested, false, __ATOMIC_RELEASE);
    homekit_refresh_mdns(deferredReasonString(r));
}

void homekit_drain_pending_state_dump()
{
    if (!__atomic_load_n(&dumpStateRequested, __ATOMIC_ACQUIRE)) return;
    HomekitDeferredReason r = dumpStateReason;
    __atomic_store_n(&dumpStateRequested, false, __ATOMIC_RELEASE);
    homekit_dump_state(deferredReasonString(r));
}

// User-triggered "Reconnect HomeKit" recovery — invoked from the
// /reconnectHomeKit web endpoint or from the watchdog auto-recover
// path. Cycles the WiFi association without rebooting; HomeSpan
// reattaches automatically when WiFi returns and re-advertises mDNS.
// Less disruptive than /reboot for cases where the device is otherwise
// healthy but the HomeKit hub thinks it's unresponsive (stale HAP TCP,
// mDNS gone stale, controller cache).
//
// v34 (F7): split-stage. Pre-v34 this called WiFi.disconnect() then
// delay(250) then WiFi.reconnect() — blocking loopTask for the full
// 250 ms window. Concurrent HTTP requests, comms_loop, SSE broadcasts
// all stalled. v34 issues the disconnect, records a timestamp, and
// returns. homekit_drain_pending_reconnect_stage2() (called every
// service_timer_loop tick on loopTask) checks elapsed time and fires
// WiFi.reconnect() when ≥250ms have passed.
//
// v43 (audit W36): pass `timeoutLength=0` to make disconnect
// fire-and-forget. arduino-esp32's `WiFi.disconnect(wifioff=false,
// eraseap=false)` (2-arg form) defaults `timeoutLength=100` and blocks
// for up to 100 ms waiting for the SYSTEM_EVENT_STA_DISCONNECTED event.
// 100 ms is well under any watchdog but the v34 comment claimed
// "~0 ms"; with timeoutLength=0 the call returns immediately and
// stage 2 still drives the re-associate at ≥250 ms.
static volatile uint8_t  reconnectStage        = 0;  // 0=idle, 1=disconnect-issued
static volatile uint32_t reconnectStageStartMs = 0;

void homekit_force_reconnect(const char *reason)
{
    // v36 (V7): rapid re-entry guard. If a previous reconnect is still in
    // its 250ms disconnect→reconnect window (stage 1), a second call would
    // overwrite reconnectStageStartMs, deferring the WiFi.reconnect() that
    // stage 2 was about to fire. Two near-simultaneous triggers (watchdog
    // auto-recover + user-clicked /reconnectHomeKit, or two watchdog
    // recoveries firing back-to-back) could keep the device stuck in the
    // disconnected window indefinitely. Drop duplicates instead.
    if (__atomic_load_n(&reconnectStage, __ATOMIC_ACQUIRE) == 1)
    {
        ESP_LOGW(TAG, "HomeKit reconnect already in progress — ignoring duplicate (%s)", reason ? reason : "unspecified");
        return;
    }
    ESP_LOGW(TAG, "HomeKit reconnect requested (%s) — cycling WiFi", reason ? reason : "unspecified");
    // Don't erase WiFi credentials — pass false. The reconnect call will
    // re-associate using the same SSID/password from NVRAM.
    // v43 (audit W36): explicit (wifioff=false, eraseap=false,
    // timeoutLength=0) — fire-and-forget, no 100 ms wait.
    WiFi.disconnect(false, false, 0);
    reconnectStageStartMs = (uint32_t)_millis();
    __atomic_store_n(&reconnectStage, (uint8_t)1, __ATOMIC_RELEASE);
    HK_DIAG_LOG("HomeKit reconnect: disconnect issued, re-associate in ~250ms");
}

// v34 (F7): stage-2 driver. Called every service_timer_loop tick on
// loopTask. No-op until stage 1 was set + 250ms elapsed; then issues
// WiFi.reconnect() and returns to idle. Eliminates the 250ms loopTask
// stall that v24's deferral inherited from the pre-v24 implementation.
void homekit_drain_pending_reconnect_stage2()
{
    if (__atomic_load_n(&reconnectStage, __ATOMIC_ACQUIRE) != 1) return;
    if ((uint32_t)_millis() - reconnectStageStartMs < 250) return;
    // v36 (V6): some chipsets / supplicant configurations auto-reconnect
    // during the 250ms gap between WiFi.disconnect(false) and this drain
    // tick. Calling WiFi.reconnect() on an already-connected interface is
    // documented as idempotent but can briefly disrupt the just-established
    // association (esp_wifi_disconnect+esp_wifi_connect under the hood).
    // Only re-associate if we're still actually disconnected.
    if (!WiFi.isConnected())
    {
        WiFi.reconnect();
        HK_DIAG_LOG("HomeKit reconnect: re-associate issued");
    }
    else
    {
        HK_DIAG_LOG("HomeKit reconnect: WiFi auto-reconnected during gap, skipping reconnect()");
    }
    __atomic_store_n(&reconnectStage, (uint8_t)0, __ATOMIC_RELEASE);
}

void WiFiGotIP(WiFiEvent_t event, WiFiEventInfo_t info)
{
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP6)
    {
        // Got IPv6 address
        ESP_LOGI(TAG, "Received IPv6 Address: %s", IPAddress(IPv6, reinterpret_cast<uint8_t *>(info.got_ip6.ip6_info.ip.addr), info.got_ip6.ip6_info.ip.zone).toString(true).c_str());

        // Now build string of IPv6 addresses
        ipv6_addresses[0] = '\0'; // Clear the buffer
        if (userConfig->getEnableIPv6())
        {
            esp_ip6_addr_t if_ip6[LWIP_IPV6_NUM_ADDRESSES];
            int nIPv6 = esp_netif_get_all_preferred_ip6(WiFi.STA.netif(), if_ip6);
            ESP_LOGI(TAG, "Total IPv6 addresses: %d", nIPv6);

            for (int i = 0; i < nIPv6; i++)
            {
                String addrStr = IPAddress(IPv6, reinterpret_cast<uint8_t *>(if_ip6[i].addr), if_ip6[i].zone).toString();
                ESP_LOGI(TAG, "  %s", addrStr.c_str());
                // Append to buffer, separated by comma if not first
                if (i > 0)
                {
                    strlcat(ipv6_addresses, ",", sizeof(ipv6_addresses));
                }
                strlcat(ipv6_addresses, addrStr.c_str(), sizeof(ipv6_addresses));
            }
            notify_new_ipv6_address();
        }
    }
    else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
    {
        // Got IPv4 address
        ESP_LOGI(TAG, "Received IPv4 Address: %s", IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
    }
    else
    {
        ESP_LOGI(TAG, "WiFi event: %s (unhandled)", WiFi.eventName(event));
    }
}

void statusCallback(HS_STATUS status)
{
    switch (status)
    {
    case HS_WIFI_NEEDED:
        ESP_LOGI(TAG, "Status: No WiFi Credentials, need to provision");
        break;
    case HS_WIFI_CONNECTING:
        ESP_LOGI(TAG, "Status: WiFi connecting");
        // v40 (audit W12): one-shot guard. HS_WIFI_CONNECTING fires on
        // EVERY WiFi connect/reconnect. Pre-v40 these four onEvent calls
        // appended duplicate handler nodes to arduino-esp32's NetworkEvents
        // list on every flap — slow heap leak (~24 B per duplicate), and
        // every WiFi event was logged 1×, 2×, 3×, ... times after each
        // reconnect. NetworkEvents has no dedup; the only fix is to
        // register exactly once.
        {
            static bool wifiHandlersRegistered = false;
            if (!wifiHandlersRegistered)
            {
                // Monitor IP address events, so we can show user IPv6 addresses
                WiFi.onEvent(WiFiGotIP, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP6);
                WiFi.onEvent(WiFiGotIP, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
                // Monitor disconnect/reconnect transitions so HomeKit "No Response"
                // events can be correlated with WiFi flaps in the syslog history.
                WiFi.onEvent(WiFiStaDisconnected, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
                WiFi.onEvent(WiFiStaConnected, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_CONNECTED);
                wifiHandlersRegistered = true;
            }
        }
        break;
    case HS_PAIRING_NEEDED:
        ESP_LOGI(TAG, "Status: Need to pair");
        isPaired = false;
        break;
    case HS_PAIRED:
        ESP_LOGI(TAG, "Status: Paired");
        isPaired = true;
        break;
    case HS_REBOOTING:
        rebooting = true;
        shutdown_comms();
        ESP_LOGI(TAG, "Status: Rebooting");
        break;
    case HS_FACTORY_RESET:
        ESP_LOGI(TAG, "Status: Factory Reset");
        break;
    case HS_WIFI_SCANNING:
        ESP_LOGI(TAG, "Status: WiFi Scanning");
        break;
    default:
        ESP_LOGI(TAG, "HomeSpan Status: %s", homeSpan.statusString(status));
        break;
    }
}

/****************************************************************************
 * Functions called from HomeSpan CLI that provide ratgdo specific
 * diagnostic info.  Used in setup_homekit()
 */

void printLogInfo(const char *buf)
{
    ratgdoLogger->printMessageLog(Serial);
}

void setLogLevel(const char *buf)
{
    long value = 0;
    char *p = const_cast<char *>(buf);
    while (*p)
    {
        if (isdigit(*p))
        {
            value = strtol(p, &p, 10);
        }
        else
        {
            p++;
        }
    }
    if (value >= 0 && value <= 5)
    {
        Serial.printf("Set log level to %d\n", value);
        userConfig->set(cfg_logLevel, (int)value);
        esp_log_level_set("*", (esp_log_level_t)userConfig->getLogLevel());
    }
    else
    {
        Serial.print("Invalid log level, value must be between 0(none) and 5(verbose)\n");
    }
}

void enableImprov(const char *buf)
{
    userConfig->set(cfg_homespanCLI, false);
    setup_improv();
}

#ifdef USE_GDOLIB
void testMoveDoor(const char *buf)
{
    long value = 0;
    char *p = const_cast<char *>(buf);
    while (*p)
    {
        if (isdigit(*p))
        {
            value = strtol(p, &p, 10);
        }
        else
        {
            p++;
        }
    }
    if (value >= 0 && value <= 100)
    {
        // v40 (audit W22): `%\n` is an invalid printf conversion specification
        // (C99 §7.19.6.1¶9 / C++ inheriting) — UB, most implementations print
        // a stray `%`, stricter ones may abort. Fixed to `%%\n` (literal `%`
        // + newline) to match the obvious intent of "Move door to: NN%".
        Serial.printf("Move door to: %d%%\n", value);
        gdo_door_move_to_target(value * 100);
    }
    else
    {
        Serial.print("Invalid door postion, value must be between 0(open) and 100(closed)\n");
    }
}
#endif // USE_GDOLIB

/****************************************************************************
 * HomeKit accessory enable functions (with HomeSpan)
 */
void createMotionAccessories()
{

    // Exit if already setup
    if (motion)
        return;

    // Define the Motion Sensor accessory...
    new SpanAccessory(HOMEKIT_AID_MOTION);
    new DEV_Info("Motion");
    motion = new DEV_Motion("Motion");
}

#ifdef RATGDO32_DISCO
void enable_service_homekit_vehicle(bool enable)
{
    const bool allowOccupancy = enable && userConfig->getVehicleOccupancyHomeKit();
    const bool allowArriving = enable && userConfig->getVehicleArrivingHomeKit();
    const bool allowDeparting = enable && userConfig->getVehicleDepartingHomeKit();

    bool databaseChanged = false;

    auto ensureMotionSensor = [&](DEV_Motion *&sensor, uint16_t aid, const char *name, bool shouldExist)
    {
        if (shouldExist)
        {
            if (!sensor)
            {
                new SpanAccessory(aid);
                new DEV_Info(name);
                sensor = new DEV_Motion(name);
                databaseChanged = true;
            }
        }
        else if (sensor)
        {
            ESP_LOGI(TAG, "Deleting HomeKit Motion Sensor: %s", name);
            if (homeSpan.deleteAccessory(aid))
            {
                sensor = nullptr;
                databaseChanged = true;
            }
        }
    };

    auto ensureOccupancySensor = [&](DEV_Occupancy *&sensor, uint16_t aid, const char *name, bool shouldExist)
    {
        if (shouldExist)
        {
            if (!sensor)
            {
                new SpanAccessory(aid);
                new DEV_Info(name);
                sensor = new DEV_Occupancy();
                databaseChanged = true;
            }
        }
        else if (sensor)
        {
            ESP_LOGI(TAG, "Deleting HomeKit Occupancy Sensor: %s", name);
            if (homeSpan.deleteAccessory(aid))
            {
                sensor = nullptr;
                databaseChanged = true;
            }
        }
    };

    ensureMotionSensor(arriving, HOMEKIT_AID_ARRIVING, "Arriving", allowArriving);
    ensureMotionSensor(departing, HOMEKIT_AID_DEPARTING, "Departing", allowDeparting);
    ensureOccupancySensor(vehicle, HOMEKIT_AID_VEHICLE, "Vehicle", allowOccupancy);

    if (databaseChanged)
    {
        homeSpan.updateDatabase();
    }

    enable_service_homekit_laser(userConfig->getLaserEnabled() && userConfig->getLaserHomeKit());
}

bool enable_service_homekit_laser(bool enable)
{
    if (enable)
    {
        if (!assistLaser && userConfig->getLaserEnabled() && userConfig->getLaserHomeKit())
        {
            // Define Light accessory for parking assist laser
            // Create only if not already created, and user config requires it.
            new SpanAccessory(HOMEKIT_AID_LASER);
            new DEV_Info("Laser");
            assistLaser = new DEV_Light(Light_t::ASSIST_LASER);
            homeSpan.updateDatabase();
            return true;
        }
    }
    else if (assistLaser)
    {
        // Delete the accessory, if it exists
        ESP_LOGI(TAG, "Deleting HomeKit Light Switch for Laser");
        if (homeSpan.deleteAccessory(HOMEKIT_AID_LASER))
        {
            assistLaser = nullptr;
            homeSpan.updateDatabase();
            return true;
        }
    }
    return false;
}
#endif

bool enable_service_homekit_room_occupancy(bool enable)
{
    // Only enable room occupancy if we have a motion sensor as well
    if (enable && motion)
    {
        if (!roomOccupancy)
        {
            // Define the Room Occupancy Sensor accessory...
            new SpanAccessory(HOMEKIT_AID_ROOM_OCCUPANCY);
            new DEV_Info("Room Occupancy");
            roomOccupancy = new DEV_Occupancy();
            return true;
        }
    }
    else if (roomOccupancy)
    {
        // Delete the accessory, if it exists
        ESP_LOGI(TAG, "Deleting HomeKit Occupancy Sensor accessory");
        if (homeSpan.deleteAccessory(HOMEKIT_AID_ROOM_OCCUPANCY))
        {
            roomOccupancy = nullptr;
            homeSpan.updateDatabase();
            garage_door.room_occupied = false;
            return true;
        }
    }
    return false;
}

bool enable_service_homekit_light(bool enable)
{
    // Dry contact (security type 3) cannot control lights
    if (userConfig->getGDOSecurityType() == 3)
    {
        ESP_LOGI(TAG, "Dry contact mode - light control not supported");
        return false;
    }

    if (enable)
    {
        if (!light)
        {
            // Define the Light accessory...
            ESP_LOGI(TAG, "Creating HomeKit Light Service");
            new SpanAccessory(HOMEKIT_AID_LIGHT_BULB);
            new DEV_Info("Light");
            light = new DEV_Light();
            homeSpan.updateDatabase();
            return true;
        }
    }
    else if (light)
    {
        // Delete the accessory, if it exists
        ESP_LOGI(TAG, "Deleting HomeKit Light Service");
        if (homeSpan.deleteAccessory(HOMEKIT_AID_LIGHT_BULB))
        {
            light = nullptr;
            homeSpan.updateDatabase();
            return true;
        }
    }
    return false;
}

bool enable_service_homekit_motion_sensor(bool enable)
{
    if (enable)
    {
        if (!motion)
        {
            // Only create if motion is possible (sensor detected OR triggers configured)
            if (garage_door.has_motion_sensor || userConfig->getMotionTriggers() != 0)
            {
                ESP_LOGI(TAG, "Creating HomeKit Motion Sensor Service");
                createMotionAccessories();
                homeSpan.updateDatabase();
                return true;
            }
            else
            {
                ESP_LOGI(TAG, "Cannot create motion service - no motion sensor and no triggers configured");
            }
        }
    }
    else if (motion)
    {
        // Delete the accessory, if it exists
        ESP_LOGI(TAG, "Deleting HomeKit Motion Sensor Service");
        // First disable room occupancy if it exists (depends on motion)
        enable_service_homekit_room_occupancy(false);
        if (homeSpan.deleteAccessory(HOMEKIT_AID_MOTION))
        {
            motion = nullptr;
            homeSpan.updateDatabase();
            return true;
        }
    }
    return false;
}

// HK-FC: runtime add/remove of the second GarageDoorOpener accessory.
// Mirrors enable_service_homekit_light's lifecycle pattern. iOS Home
// surfaces a one-shot "Configuration Updated" dialog when the bridge
// HAP config number bumps; existing pairings remain valid.
//
// Sec+1.0 only is enforced inside door_command_force_close itself.
// The toggle is exposed regardless of GDOSecurityType so users on
// Sec+2.0 / dry-contact still get the visual second tile (close
// path falls through to a normal close — see comms.cpp:2853-2858).
//
// Tri-state mode (cfg_forceCloseHomeKit):
//   0 = OFF       — no force-close tile, primary close uses normal toggle
//   1 = COMPANION — separate force-close tile alongside primary
//   2 = REPLACE   — no second tile; primary close calls force-close path
// Only mode == 1 instantiates the second accessory; mode 0 and mode 2
// both delete it (if present from a previous mode-1 session).
bool enable_service_homekit_force_close(int mode)
{
    if (mode == 1)
    {
        if (!forceCloseDoor)
        {
            ESP_LOGI(TAG, "Creating HomeKit Force-Close Garage Door Service");
            new SpanAccessory(HOMEKIT_AID_FORCE_CLOSE_DOOR);
            new DEV_Info("Force Close Door");
            new Characteristic::Manufacturer("Ratcloud llc");
            new Characteristic::SerialNumber(Network.macAddress().c_str());
            new Characteristic::Model("ratgdo-ESP32-fc");
            new Characteristic::FirmwareRevision(AUTO_VERSION);
            forceCloseDoor = new DEV_GarageDoorForceClose();
            homeSpan.updateDatabase();
            return true;
        }
    }
    else if (forceCloseDoor)
    {
        // mode 0 or 2 — delete the second tile if it exists. Mode 2's
        // close-replacement is wired in DEV_GarageDoor::update(), not
        // via a second accessory.
        ESP_LOGI(TAG, "Deleting HomeKit Force-Close Garage Door Service (mode=%d)", mode);
        if (homeSpan.deleteAccessory(HOMEKIT_AID_FORCE_CLOSE_DOOR))
        {
            forceCloseDoor = nullptr;
            homeSpan.updateDatabase();
            return true;
        }
    }
    return false;
}

/****************************************************************************
 * Setup HomeKit, HomeSpan version.
 */
void setup_homekit()
{
    if (homekit_setup_done || softAPmode)
        return;

    // v31 final: capture loopTask handle here (we're called from
    // setup() on the Arduino loopTask) so homekit_health_log can
    // query its stack high-water mark from Ticker context later.
    loopTaskHandleForHWM = xTaskGetCurrentTaskHandle();

    ESP_LOGI(TAG, "=== Setup HomeKit accessories and services ===");

    // homeSpan.setLogLevel(0); Zero is default (top level messages only), comment out so can be controlled by Improv setup.
    homeSpan.setSketchVersion(AUTO_VERSION);
    homeSpan.setHostNameSuffix("");
    homeSpan.setPortNum(5556);
    // We will manage LED flashing ourselves
    // homeSpan.setStatusPin(LED_BUILTIN);
    homeSpan.enableAutoStartAP();
    homeSpan.setApFunction(start_soft_ap);

    // Generate a QR Code ID from our MAC address, which should create unique pairing QR codes
    // for each of multiple devices on a network... although we do have to clip to 4 characters,
    // so we loose ~2 most significant bits.
    uint8_t mac[6];
    Network.macAddress(mac);
    uint32_t uid = (mac[3] << 16) + (mac[4] << 8) + mac[5];
    char setupID[6];
    toBase62(setupID, sizeof(setupID), uid); // always includes leading zeros
    ESP_LOGI(TAG, "HomeKit pairing QR Code ID: %s", &setupID[1]);
    HapQR qrCode;
    strlcpy(qrPayload, qrCode.get((uint32_t)25102023, &setupID[1], (uint8_t)Category::GarageDoorOpeners), sizeof(qrPayload));
    ESP_LOGI(TAG, "HomeKit QR setup payload: %s", qrPayload);
    homeSpan.setQRID(&setupID[1]);
    homeSpan.setPairingCode("25102023"); // On Oct 25, 2023, Chamberlain announced they were disabling API
                                         // access for "unauthorized" third parties.
    homeSpan.setWifiBegin(wifiBegin);
    homeSpan.setConnectionCallback(connectionCallback);
    homeSpan.setStatusCallback(statusCallback);
    // Real-time HomeKit event visibility (helps diagnose "No Response"):
    //   setPairCallback           — pair/unpair transitions (incl. unexpected unpair)
    //   setControllerCallback     — controller list changes (pairings added/removed)
    //   setGetCharacteristicsCallback — fires when iOS reads any characteristic;
    //                                used to track "last time iOS talked to us"
    //                                in the periodic health log.
    homeSpan.setPairCallback(hap_pair_cb);
    homeSpan.setControllerCallback(hap_controller_change_cb);
    homeSpan.setGetCharacteristicsCallback(hap_get_characteristics_cb);

    homeSpan.begin(Category::Bridges, device_name, device_name_rfc952, "ratgdo-ESP32");

#ifdef CONFIG_FREERTOS_USE_TRACE_FACILITY
    new SpanUserCommand('t', "- print FreeRTOS task info", printTaskInfo);
#endif
    new SpanUserCommand('l', "- print RATGDO buffered message log", printLogInfo);
    new SpanUserCommand('d', "<level> - set ESP log level 0(none), 1(error), 2(warn), 3(info), 4(debug), 5(verbose)", setLogLevel);
#ifdef USE_GDOLIB
    new SpanUserCommand('m', "<percent> - move door to position between 0(open) and 100 (closed)", testMoveDoor);
#endif
#ifdef CRASH_DEBUG
    new SpanUserCommand('z', "- test function", testDelayFn);
#endif
    new SpanUserCommand('C', "switch to RATGDO CLI (and enable Improv WiFi provisioning)", enableImprov);

    // Define a bridge (as more than 3 accessories)
    new SpanAccessory(HOMEKIT_AID_BRIDGE);
    new DEV_Info(default_device_name);

    // Define the Garage Door accessory...
    new SpanAccessory(HOMEKIT_AID_GARAGE_DOOR);
    new DEV_Info(device_name);
    new Characteristic::Manufacturer("Ratcloud llc");
    new Characteristic::SerialNumber(Network.macAddress().c_str());
    new Characteristic::Model("ratgdo-ESP32");
    new Characteristic::FirmwareRevision(AUTO_VERSION);
    door = new DEV_GarageDoor();

    // Dry contact (security type 3) cannot control lights
    if (userConfig->getGDOSecurityType() != 3)
    {
        // Only create Light accessory if enabled in settings (default: true)
        if (userConfig->getLightHomeKit())
        {
            // Define the Light accessory...
            new SpanAccessory(HOMEKIT_AID_LIGHT_BULB);
            new DEV_Info("Light");
            light = new DEV_Light();
        }
        else
        {
            ESP_LOGI(TAG, "Light HomeKit accessory disabled in settings");
        }
    }
    else
    {
        ESP_LOGI(TAG, "Dry contact mode. Disabling light switch service");
    }

    // HK-FC: optionally create the second GarageDoorOpener accessory at boot.
    // Tri-state mode: only mode 1 (companion) instantiates the second tile.
    // Mode 0 (off) and mode 2 (replace) both skip — mode 2's close-replacement
    // wires into DEV_GarageDoor::update() instead. Runtime toggle of the tile
    // handled via enable_service_homekit_force_close so users don't reboot.
    const int fcMode = userConfig->getForceCloseHomeKit();
    if (fcMode == 1)
    {
        ESP_LOGI(TAG, "Creating HomeKit Force-Close Garage Door Service (mode=1 companion)");
        new SpanAccessory(HOMEKIT_AID_FORCE_CLOSE_DOOR);
        new DEV_Info("Force Close Door");
        new Characteristic::Manufacturer("Ratcloud llc");
        new Characteristic::SerialNumber(Network.macAddress().c_str());
        new Characteristic::Model("ratgdo-ESP32-fc");
        new Characteristic::FirmwareRevision(AUTO_VERSION);
        forceCloseDoor = new DEV_GarageDoorForceClose();
    }
    else if (fcMode == 2)
    {
        ESP_LOGI(TAG, "Force-close HomeKit mode=2 (replace) — primary close will use force-close path");
    }
    else
    {
        ESP_LOGI(TAG, "Force-close HomeKit disabled in settings (mode=0)");
    }

    // only create motion if we know we have motion sensor(s) AND it's enabled in settings
    garage_door.has_motion_sensor = (bool)read_door_int(nvram_has_motion);
    if (garage_door.has_motion_sensor || userConfig->getMotionTriggers() != 0)
    {
        if (userConfig->getMotionHomeKit())
        {
            createMotionAccessories();
        }
        else
        {
            ESP_LOGI(TAG, "Motion HomeKit accessory disabled in settings");
        }
    }
    else
    {
        ESP_LOGI(TAG, "No motion sensor. Skipping motion service");
    }

#ifdef RATGDO32_DISCO
    // only create sensors if we know we have time-of-flight distance sensor
    garage_door.has_distance_sensor = (bool)read_door_int(nvram_has_distance);
    if (garage_door.has_distance_sensor)
    {
        enable_service_homekit_vehicle(userConfig->getVehicleHomeKit());
    }
    else
    {
        ESP_LOGI(TAG, "No vehicle presence sensor. Skipping motion and occupancy services");
    }
#endif
    // Create a room occupancy sensor if timer for it is greater than 0
    enable_service_homekit_room_occupancy(userConfig->getOccupancyDuration() > 0);

    // Auto poll starts up a new FreeRTOS task to do the HomeKit comms
    // so no need to handle in our Arduino loop.
    homeSpan.autoPoll((1024 * 16), 1, 0);

    // v22: seed the watchdog config cache before the first health-tick
    // fires. After this, the Ticker callback reads the cached values
    // instead of taking the userConfig mutex.
    homekit_refresh_watchdog_config();

    // Start periodic HomeKit health logging — see homekit_health_log()
    // above. v22 bumped to 180s.
    // detach homekitHealthTicker: defensive kill before re-arming the
    // health-log periodic ticker at boot. Distinct from TTCtimer; no
    // force-close interaction.
    // log-audit-20260520-001: verify the boot-time arm. No spinlock needed
    // (single-threaded — homekit_setup_done one-shot enforces this).
    homekitHealthTicker.detach();
    homekitHealthTicker.attach_ms(HOMEKIT_HEALTH_INTERVAL_MS, homekit_health_log);
    if (homekitHealthTicker.active())
    {
        homekitHealthTicker_armCount++;
        homekitHealthTicker_armFailed = false;
    }
    else
    {
        homekitHealthTicker_armFailed       = true;
        homekitHealthTicker_lastArmFailedMs = (uint32_t)_millis();
        ESP_LOGE(TAG, "HomeKit health: Ticker arm FAILED at boot — deferred-retry armed");
    }

    // v27: HomeSpan does not invoke the controller-change callback for
    // pairings loaded from NVS at boot, only for live add/remove events.
    // Seed the cache here so homekit_health_log doesn't report
    // controllers=0 between boot and the next live pairing change.
    hap_controller_change_cb();

    homekit_setup_done = true;
}

void queueSendHelper(QueueHandle_t q, GDOEvent e, const char *txt)
{
    if (!q || xQueueSend(q, &e, 0) == errQUEUE_FULL)
    {
        ESP_LOGE(TAG, "Could not queue homekit notify of %s state: %d", txt, e.value.u);
    }
}

void homekit_unpair()
{
    if (!isPaired)
        return;

    homeSpan.processSerialCommand("U");
}

/****************************************************************************
 * Accessory Information Handler
 */
DEV_Info::DEV_Info(const char *name) : Service::AccessoryInformation()
{
    new Characteristic::Identify();
    new Characteristic::Name(name);
}

boolean DEV_Info::update()
{
    ESP_LOGI(TAG, "Request to identify accessory, flash LED, etc.");
    // LED, Laser and Tone calls are all asynchronous.  We will illuminate LED and Laser
    // for 2 seconds, during which we will play tone.  Function will return after 1.5 seconds.
    led.flash(2000);
#ifdef RATGDO32_DISCO
    laser.flash(2000);
    tone(BEEPER_PIN, 1300);
    delay(500);
    tone(BEEPER_PIN, 2000);
    delay(500);
    tone(BEEPER_PIN, 1300);
    delay(500);
    tone(BEEPER_PIN, 2000, 500);
#endif
    return true;
}

/****************************************************************************
 * Garage Door Service Handler
 */
DEV_GarageDoor::DEV_GarageDoor() : Service::GarageDoorOpener()
{
    ESP_LOGI(TAG, "Configuring HomeKit Garage Door Service");
    event_q = xQueueCreate(10, sizeof(GDOEvent));
    current = new Characteristic::CurrentDoorState(current->CLOSED);
    target = new Characteristic::TargetDoorState(target->CLOSED);
    obstruction = new Characteristic::ObstructionDetected(obstruction->NOT_DETECTED);
    if (userConfig->getGDOSecurityType() != 3)
    {
        // Dry contact cannot control lock ?
        lockCurrent = new Characteristic::LockCurrentState(lockCurrent->UNKNOWN);
        lockTarget = new Characteristic::LockTargetState(lockTarget->UNLOCK);
    }
    else
    {
        lockCurrent = nullptr;
        lockTarget = nullptr;
    }
}

// log-audit-20260515-008/009 dispatch-storm dedup. iOS HomeKit periodically
// fires bursts of redundant target-state writes (typically 4-13 within 1-2 s)
// to verify HK accessory state; pre-dedup every burst write reached
// open_door()/close_door()/door_command_force_close(), producing a cascade of
// SEC1 packets, internal retries, and (in the 009 path) a real risk of a
// second force-close sequence firing inside the cross-task state-propagation
// race window.
//
// Decision logic uses garage_door.current_state ONLY — NOT target_state.
// target_state is itself a HomeKit-driven echo and using it as the gate would
// race the iOS burst writer (the first burst write updates target, the
// second-through-Nth would all see "target already matches my intent" and
// drop the legitimate first-of-burst that drove the GDO command). current_state
// is GDO-truth and lags HK by physical motion, which is the property we want.
//
// CRITICAL: mid-cycle reversal (CURR_OPENING + target=CLOSED, or
// CURR_CLOSING + target=OPEN) MUST fire — user changed their mind mid-motion
// and the GDO's wall-button packet semantics will honor a reversal toggle.
// Eating the reversal would leave the door doing the opposite of what the
// user just asked for.
//
// Returning true ACKs the HK characteristic write (HomeKit is happy and
// settled); we just skip pushing to the GDO. Rate-limited summary log
// uses the same 5s-window + [+N suppressed] shape as comms.cpp:3068.
//
// Two pairs of file-scope counters so Open and Close bursts are independently
// observable in /showlog — diagnosing 008 vs 009 root cause requires
// distinguishing direction.
static uint32_t hkOpenDedupLastMs    = 0;
static uint32_t hkOpenDedupSuppressed = 0;
static uint32_t hkCloseDedupLastMs   = 0;
static uint32_t hkCloseDedupSuppressed = 0;

static bool hk_target_is_redundant(uint8_t requestedTarget, const char *tag)
{
    // Single byte read — garage_door.current_state is uint8_t-sized and
    // updated by the loopTask same as this caller (both DEV_GarageDoor::update
    // and DEV_GarageDoorForceClose::update run from HomeSpan poll on loopTask).
    // No cross-task concern at the read site.
    const GarageDoorCurrentState cs = garage_door.current_state;
    bool drop = false;
    if (requestedTarget == Characteristic::TargetDoorState::OPEN)
    {
        // OPEN intent: drop when already open or actively opening. CURR_CLOSING
        // is a legit reversal — fire. CURR_CLOSED / CURR_STOPPED / 0xFF fire.
        if (cs == GarageDoorCurrentState::CURR_OPEN || cs == GarageDoorCurrentState::CURR_OPENING)
        {
            drop = true;
        }
    }
    else if (requestedTarget == Characteristic::TargetDoorState::CLOSED)
    {
        // CLOSED intent: drop when already closed or actively closing. CURR_OPENING
        // is a legit reversal — fire. CURR_OPEN / CURR_STOPPED / 0xFF fire.
        if (cs == GarageDoorCurrentState::CURR_CLOSED || cs == GarageDoorCurrentState::CURR_CLOSING)
        {
            drop = true;
        }
    }
    // Any unexpected target value (shouldn't occur — HK enum is OPEN/CLOSED only)
    // falls through with drop=false to preserve forward-compat.

    if (!drop) return false;

    // Rate-limited summary log — pattern matches comms.cpp:3068 (force-close
    // reject) and comms.cpp:1684 (SEC1 retry). 5s window, [+N suppressed]
    // count on the next post-window fire.
    uint32_t *lastMs       = (requestedTarget == Characteristic::TargetDoorState::OPEN) ? &hkOpenDedupLastMs       : &hkCloseDedupLastMs;
    uint32_t *suppressed   = (requestedTarget == Characteristic::TargetDoorState::OPEN) ? &hkOpenDedupSuppressed   : &hkCloseDedupSuppressed;
    const uint32_t nowMs = (uint32_t)_millis();
    const uint32_t deltaMs = (*lastMs == 0) ? UINT32_MAX : (nowMs - *lastMs);
    if (deltaMs > 5000UL)
    {
        if (*suppressed > 0)
        {
            ESP_LOGI(TAG, "HK %s: redundant target=%s dropped (current=%s) [+%u suppressed in last %ums — iOS HomeKit dispatch burst]",
                     tag, DOOR_STATE(requestedTarget), DOOR_STATE(cs),
                     (unsigned)*suppressed, (unsigned)deltaMs);
        }
        else
        {
            ESP_LOGI(TAG, "HK %s: redundant target=%s dropped (current=%s)",
                     tag, DOOR_STATE(requestedTarget), DOOR_STATE(cs));
        }
        *lastMs = nowMs;
        *suppressed = 0;
    }
    else
    {
        (*suppressed)++;
    }
    return true;
}

boolean DEV_GarageDoor::update()
{
    ESP_LOGI(TAG, "Garage Door Characteristics Update, door target: %s", DOOR_STATE(target->getNewVal()));
    if (hk_target_is_redundant(target->getNewVal(), "primary tile")) return true;
    GarageDoorCurrentState state;
    if (target->getNewVal() == target->OPEN)
    {
        state = open_door();
    }
    else
    {
        // HK-FC mode 2 (replace): the primary close button calls
        // force-close directly — saves the cascade of "try normal close
        // → detect failure → fall back to force-close" for setups whose
        // GDO always needs the long-press hold. Mode 0/1 keep the
        // standard close path. door_command_force_close clamps hold-ms
        // and falls back to a normal close for Sec+2.0 / dry-contact
        // (comms.cpp:2880-2881), so this is safe across security types.
#ifndef ESP8266
        if (userConfig->getForceCloseHomeKit() == 2)
        {
            // Pick the active hold-ms field based on the press mechanic
            // selection. Two-attempt and single-hold need different
            // timings; flipping the checkbox keeps both intact.
            const bool single = userConfig->getForceCloseSingleHold();
            const uint32_t holdMs = single ? userConfig->getForceCloseHoldMsSingle()
                                           : userConfig->getForceCloseHoldMs();
            ESP_LOGI(TAG, "HK-FC mode=2 — primary close dispatching force-close (%s, hold=%ums)",
                     single ? "single-hold" : "2-attempt", (unsigned)holdMs);
            door_command_force_close(holdMs);
            state = garage_door.current_state;
        }
        else
#endif
        {
            state = close_door();
        }
    }
    obstruction->setVal(false);
    current->setVal(state);

    if (userConfig->getGDOSecurityType() != 3)
    {
        // Dry contact cannot control lock
        set_lock(lockTarget->getNewVal() == lockTarget->LOCK);
    }
    return true;
}

void DEV_GarageDoor::loop()
{
    if (uxQueueMessagesWaiting(event_q) > 0)
    {
        GDOEvent e;
        xQueueReceive(event_q, &e, 0);
        if (e.c == current)
            ESP_LOGD(TAG, "Set current door state: %s", DOOR_STATE(e.value.u));
        else if (e.c == target)
            ESP_LOGD(TAG, "Set target door state: %s", DOOR_STATE(e.value.u));
        else if (e.c == obstruction)
            ESP_LOGD(TAG, "Set obstruction: %s", e.value.u ? "Obstructed" : "Clear");
        else if (e.c == lockCurrent)
            ESP_LOGD(TAG, "Set current lock state: %s", LOCK_STATE(e.value.u));
        else if (e.c == lockTarget)
            ESP_LOGD(TAG, "Set target lock state: %s", LOCK_STATE(e.value.u));
        else
            ESP_LOGD(TAG, "Set Unknown: %d", e.value.u);
        e.c->setVal(e.value.u);
    }
}

/****************************************************************************
 * HK-FC: Force-Close Garage Door Service Handler
 * Open mirrors normal open_door; Close fires door_command_force_close
 * with the user-configured hold-ms. State mirrors the primary tile via
 * the notify_homekit_*_door_state_change paths below.
 */
DEV_GarageDoorForceClose::DEV_GarageDoorForceClose() : Service::GarageDoorOpener()
{
    ESP_LOGI(TAG, "Configuring HomeKit Force-Close Garage Door Service");
    event_q     = xQueueCreate(10, sizeof(GDOEvent));
    current     = new Characteristic::CurrentDoorState(current->CLOSED);
    target      = new Characteristic::TargetDoorState(target->CLOSED);
    obstruction = new Characteristic::ObstructionDetected(obstruction->NOT_DETECTED);
}

boolean DEV_GarageDoorForceClose::update()
{
    const uint8_t tgt = target->getNewVal();
    ESP_LOGI(TAG, "Force-close tile target: %s", DOOR_STATE(tgt));
    if (hk_target_is_redundant(tgt, "FC tile")) return true;
    if (tgt == target->OPEN)
    {
        // Open mirrors a normal open — no hold-override needed.
        open_door();
    }
    else
    {
        // Close fires force-close with the user-configured hold-ms,
        // picking the field that matches the active press mechanic
        // (2-attempt → forceCloseHoldMs, single-hold → forceCloseHoldMsSingle).
        // door_command_force_close clamps to [1000, 15000] internally
        // and falls back to a normal close on Sec+2.0 / dry-contact.
        const bool single = userConfig->getForceCloseSingleHold();
        const uint32_t holdMs = single ? userConfig->getForceCloseHoldMsSingle()
                                       : userConfig->getForceCloseHoldMs();
        door_command_force_close(holdMs);
    }
    return true;
}

void DEV_GarageDoorForceClose::loop()
{
    if (uxQueueMessagesWaiting(event_q) > 0)
    {
        GDOEvent e;
        xQueueReceive(event_q, &e, 0);
        if (e.c == current)
            ESP_LOGD(TAG, "Set force-close current door state: %s", DOOR_STATE(e.value.u));
        else if (e.c == target)
            ESP_LOGD(TAG, "Set force-close target door state: %s", DOOR_STATE(e.value.u));
        else if (e.c == obstruction)
            ESP_LOGD(TAG, "Set force-close obstruction: %s", e.value.u ? "Obstructed" : "Clear");
        else
            ESP_LOGD(TAG, "Set force-close Unknown: %d", e.value.u);
        e.c->setVal(e.value.u);
    }
}

/****************************************************************************
 * Light Service Handler
 */
// v45 W45: param renamed lightType to avoid shadowing member DEV_Light::type
// (caught by -Wshadow=local under build_src_flags).
DEV_Light::DEV_Light(Light_t lightType) : Service::LightBulb()
{
    DEV_Light::type = lightType;
    if (lightType == Light_t::GDO_LIGHT)
        ESP_LOGI(TAG, "Configuring HomeKit Light Service for GDO Light");
    else if (lightType == Light_t::ASSIST_LASER)
        ESP_LOGI(TAG, "Configuring HomeKit Light Service for Laser");
    event_q = xQueueCreate(10, sizeof(GDOEvent));
    DEV_Light::on = new Characteristic::On(DEV_Light::on->OFF);
}

boolean DEV_Light::update()
{
    if (this->type == Light_t::GDO_LIGHT)
    {
        set_light(DEV_Light::on->getNewVal<bool>());
    }
#ifdef RATGDO32_DISCO
    else if (this->type == Light_t::ASSIST_LASER)
    {
        if (on->getNewVal<bool>())
        {
            ESP_LOGI(TAG, "Turn parking assist laser on");
            laser.on();
        }
        else
        {
            ESP_LOGI(TAG, "Turn parking assist laser off");
            laser.off();
        }
    }
#endif
    return true;
}

void DEV_Light::loop()
{
    if (uxQueueMessagesWaiting(event_q) > 0)
    {
        GDOEvent e;
        xQueueReceive(event_q, &e, 0);
        if (this->type == Light_t::GDO_LIGHT)
            ESP_LOGD(TAG, "Light has turned %s", e.value.b ? "on" : "off");
        else if (this->type == Light_t::ASSIST_LASER)
            ESP_LOGD(TAG, "Parking assist laser has turned %s", e.value.b ? "on" : "off");
        DEV_Light::on->setVal(e.value.b);
    }
}

/****************************************************************************
 * Motion Service Handler
 */
// v45 W45: param renamed motionName to avoid shadowing member DEV_Motion::name
// (caught by -Wshadow=local under build_src_flags).
DEV_Motion::DEV_Motion(const char *motionName) : Service::MotionSensor()
{
    ESP_LOGI(TAG, "Configuring HomeKit Motion Service for %s", motionName);
    event_q = xQueueCreate(10, sizeof(GDOEvent));
    strlcpy(this->name, motionName, sizeof(this->name));
    DEV_Motion::motion = new Characteristic::MotionDetected(motion->NOT_DETECTED);
}

void DEV_Motion::loop()
{
    if (uxQueueMessagesWaiting(event_q) > 0)
    {
        GDOEvent e;
        xQueueReceive(event_q, &e, 0);
        ESP_LOGD(TAG, "%s %s", name, e.value.b ? "detected" : "reset");
        DEV_Motion::motion->setVal(e.value.b);
    }
}

/****************************************************************************
 * Occupancy Service Handler
 */
DEV_Occupancy::DEV_Occupancy() : Service::OccupancySensor()
{
    ESP_LOGI(TAG, "Configuring HomeKit Occupancy Service");
    event_q = xQueueCreate(10, sizeof(GDOEvent));
    DEV_Occupancy::occupied = new Characteristic::OccupancyDetected(occupied->NOT_DETECTED);
}

void DEV_Occupancy::loop()
{
    if (uxQueueMessagesWaiting(event_q) > 0)
    {
        GDOEvent e;
        xQueueReceive(event_q, &e, 0);
        ESP_LOGD(TAG, "%s occupancy %s", (this == vehicle) ? "Vehicle" : "Room", e.value.b ? "detected" : "reset");
        DEV_Occupancy::occupied->setVal(e.value.b);
    }
}

/****************************************************************************
 * HomeKit notification functions only for ESP32
 */
void notify_homekit_vehicle_occupancy(bool vehicleDetected)
{
    if (!isPaired || !vehicle)
        return;

    GDOEvent e;
    e.c = nullptr;
    e.value.b = vehicleDetected;
    queueSendHelper(vehicle->event_q, e, "vehicle");
}

void notify_homekit_room_occupancy(bool occupied)
{
    if (!isPaired || !roomOccupancy)
        return;

    GDOEvent e;
    e.c = nullptr;
    e.value.b = garage_door.room_occupied = occupied;
    garage_door.room_occupancy_timeout = (!occupied) ? 0 : _millis() + userConfig->getOccupancyDuration() * 1000; // convert seconds to milliseconds
    queueSendHelper(roomOccupancy->event_q, e, "room occupancy");
}

void notify_homekit_laser(bool on)
{
    if (!isPaired || !assistLaser)
        return;

    GDOEvent e;
    e.c = nullptr;
    e.value.b = on;
    queueSendHelper(assistLaser->event_q, e, "laser");
}

void notify_homekit_vehicle_arriving(bool vehicleArriving)
{
    if (!isPaired || !arriving)
        return;

    GDOEvent e;
    e.c = nullptr;
    e.value.b = vehicleArriving;
    queueSendHelper(arriving->event_q, e, "arriving");
}

void notify_homekit_vehicle_departing(bool vehicleDeparting)
{
    if (!isPaired || !departing)
        return;

    GDOEvent e;
    e.c = nullptr;
    e.value.b = vehicleDeparting;
    queueSendHelper(departing->event_q, e, "departing");
}

// on ESP8266 this is provided by the Arduino HomeKit library
bool homekit_is_paired()
{
    return isPaired;
}
#endif // ESP8266

/****************************************************************************
 * HomeKit notification functions common to both ESP8266 and ESP32
 */
void notify_homekit_target_door_state_change(GarageDoorTargetState state)
{
    garage_door.target_state = state;
    // Ignore invalid states
    if (state == 0xFF)
        return;
#ifdef ESP32
    if (!isPaired)
        return;

    GDOEvent e;
    e.c = door->target;
    e.value.u = (uint8_t)garage_door.target_state;
    queueSendHelper(door->event_q, e, "target door");
    // HK-FC: mirror to the second tile so animation stays in lockstep.
    if (forceCloseDoor)
    {
        GDOEvent fc;
        fc.c = forceCloseDoor->target;
        fc.value.u = (uint8_t)garage_door.target_state;
        queueSendHelper(forceCloseDoor->event_q, fc, "force-close target door");
    }
#else
    if (!arduino_homekit_get_running_server())
        return;

    homekit_characteristic_notify(&target_door_state, HOMEKIT_UINT8_CPP(garage_door.target_state));
#endif
}

void notify_homekit_current_door_state_change(GarageDoorCurrentState state)
{
    garage_door.current_state = state;
    // Ignore invalid states
    if (state == 0xFF)
        return;
#ifdef ESP32
    if (!isPaired)
        return;

    GDOEvent e;
    e.c = door->current;
    e.value.u = (uint8_t)garage_door.current_state;
    queueSendHelper(door->event_q, e, "current door");
    // HK-FC: mirror to the second tile so animation stays in lockstep.
    if (forceCloseDoor)
    {
        GDOEvent fc;
        fc.c = forceCloseDoor->current;
        fc.value.u = (uint8_t)garage_door.current_state;
        queueSendHelper(forceCloseDoor->event_q, fc, "force-close current door");
    }

#ifdef RATGDO32_DISCO
    // Notify the vehicle presence code that door state is changing
    if (garage_door.current_state == GarageDoorCurrentState::CURR_OPENING)
        doorOpening();
    if (garage_door.current_state == GarageDoorCurrentState::CURR_CLOSING)
        doorClosing();
#endif
#else
    if (!arduino_homekit_get_running_server())
        return;

    homekit_characteristic_notify(&current_door_state, HOMEKIT_UINT8_CPP(garage_door.current_state));
#endif
}

void notify_homekit_target_lock(LockTargetState state)
{
    garage_door.target_lock = state;
    // Ignore invalid states
    if (state == 0xFF)
        return;
#ifdef ESP32
    if (!isPaired)
        return;

    GDOEvent e;
    e.c = door->lockTarget;
    e.value.u = (uint8_t)garage_door.target_lock;
    queueSendHelper(door->event_q, e, "target lock");
#else
    if (!arduino_homekit_get_running_server())
        return;

    homekit_characteristic_notify(&target_lock_state, HOMEKIT_UINT8_CPP(garage_door.target_lock));
#endif
}

void notify_homekit_current_lock(LockCurrentState state)
{
    garage_door.current_lock = state;
    // Ignore invalid states
    if (state == 0xFF)
        return;
#ifdef ESP32
    if (!isPaired)
        return;

    GDOEvent e;
    e.c = door->lockCurrent;
    e.value.u = (uint8_t)garage_door.current_lock;
    queueSendHelper(door->event_q, e, "current lock");
#else
    if (!arduino_homekit_get_running_server())
        return;

    homekit_characteristic_notify(&current_lock_state, HOMEKIT_UINT8_CPP(garage_door.current_lock));
#endif
}

void notify_homekit_obstruction(bool state)
{
    garage_door.obstructed = state;
#ifdef ESP32
    if (!isPaired)
        return;

    GDOEvent e;
    e.c = door->obstruction;
    e.value.b = garage_door.obstructed;
    queueSendHelper(door->event_q, e, "obstruction");
    // HK-FC: mirror obstruction state to the second tile so both flag
    // photo-eye breaks identically.
    if (forceCloseDoor)
    {
        GDOEvent fc;
        fc.c = forceCloseDoor->obstruction;
        fc.value.b = garage_door.obstructed;
        queueSendHelper(forceCloseDoor->event_q, fc, "force-close obstruction");
    }
#else
    if (!arduino_homekit_get_running_server())
        return;

    homekit_characteristic_notify(&obstruction_detected, HOMEKIT_BOOL_CPP(garage_door.obstructed));
#endif
}

void notify_homekit_light(bool state)
{
    garage_door.light = state;
#ifdef ESP32
    if (!isPaired || !light)
        return;

    GDOEvent e;
    e.c = nullptr;
    e.value.b = garage_door.light;
    queueSendHelper(light->event_q, e, "light");
#else
    if (!arduino_homekit_get_running_server())
        return;

    homekit_characteristic_notify(&light_state, HOMEKIT_BOOL_CPP(garage_door.light));
#endif
}

void enable_service_homekit_motion(bool reboot)
{
#ifdef ESP32
    // only create if not already created AND motion accessory is enabled in settings
    if (!garage_door.has_motion_sensor)
    {
        write_door_int(nvram_has_motion, 1);
        garage_door.has_motion_sensor = true;
        if (userConfig->getMotionHomeKit())
        {
            createMotionAccessories();
        }
        if (reboot)
        {
            sync_and_restart();
        }
    }
#else
    write_door_int(nvram_has_motion, 1);
    if (reboot)
    {
        sync_and_restart();
    }
#endif
}

void notify_homekit_motion(bool state)
{
    garage_door.motion = state;
#ifdef ESP32
    garage_door.motion_timer = (!state) ? 0 : _millis() + MOTION_TIMER_DURATION;
    if (!isPaired || !motion)
        return;

    GDOEvent e;
    e.c = nullptr;
    e.value.b = garage_door.motion;
    queueSendHelper(motion->event_q, e, "motion");
#else
    garage_door.motion_timer = (!state) ? 0 : _millis() + MOTION_TIMER_DURATION;
    if (!arduino_homekit_get_running_server())
        return;

    homekit_characteristic_notify(&motion_detected, HOMEKIT_BOOL_CPP(garage_door.motion));
#endif
#ifndef ESP8266
    if (state)
        notify_homekit_room_occupancy(true);
#endif
}
