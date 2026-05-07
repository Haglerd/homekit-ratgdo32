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
extern void delayFnCall(uint32_t ms, void (*callback)(), bool preempt_force_close = true);
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
static uint8_t     hkConsecutiveHealthyTicks = 0;

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
    static _millis_t lastTickMs = 0;
    int32_t tickDriftMs = 0;
    if (lastTickMs) tickDriftMs = (int32_t)(((uint32_t)_millis() - (uint32_t)lastTickMs) - HOMEKIT_HEALTH_INTERVAL_MS);
    lastTickMs = _millis();
    extern volatile uint32_t logMtxMaxWaitMs;
    extern volatile uint32_t sseSlowWrites;
    // sseBufferFullSkips: cumulative lwIP-send-buffer-full skips since
    // boot (flow-control diagnostic; trend matters more than absolute).
    extern volatile uint32_t sseBufferFullSkips;
    // sseSlotsAlloc: live count refreshed by sweep_sse_orphans.
    // sseOrphansReaped: per-window counter, atomic-exchange-zeroed below.
    extern volatile uint32_t sseSlotsAlloc;
    extern volatile uint32_t sseOrphansReaped;
    // MH6 instrumentation: peak JSON length this window. Inform future
    // STATUS_JSON_BUFFER_SIZE retune decision in v34.
    extern volatile uint32_t statusJsonPeakLen;
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
    HK_DIAG_LOG("HomeKit diag-hk: recoverAttempts=%u hintLevel=%u hkHealthyTicks=%u loopHWM=%uB tmrHWM=%uB apHWM=%uB tickDrift=%dms",
                (unsigned)hkRecoverAttempts,
                (unsigned)hkLastHintLevel,
                (unsigned)hkConsecutiveHealthyTicks,
                (unsigned)loopHWM,
                (unsigned)tmrSvcHWM,
                (unsigned)apHWM,
                (int)tickDriftMs);

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
    else if (hkRecoverAttempts > 0)
    {
        // v31: require N consecutive healthy ticks before clearing the
        // recovery counter. A single sporadic read inside the trailing
        // window of one tick (pre-v31 logic) is not enough — a flapping
        // hub could re-arm the watchdog indefinitely. Healthy = lastReadAgo
        // is positive (we've seen at least one read) AND below quietSecs
        // (the user-configured "iOS extended idle" threshold).
        if (lastReadAgo > 0 && lastReadAgo < (int32_t)hkQuietSecs)
        {
            hkConsecutiveHealthyTicks++;
            if (hkConsecutiveHealthyTicks >= HK_HEALTHY_TICKS_TO_RESET)
            {
                ESP_LOGI(TAG, "HomeKit auto-recover: %u consecutive healthy ticks (last_hap_read_ago=%ds), clearing recovery counter",
                         (unsigned)hkConsecutiveHealthyTicks, lastReadAgo);
                hkRecoverAttempts         = 0;
                hkConsecutiveHealthyTicks = 0;
            }
        }
        else
        {
            // Any non-healthy tick resets the consecutive count.
            hkConsecutiveHealthyTicks = 0;
        }
    }
}

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
    homekitHealthTicker.detach();
    homekitHealthTicker.attach_ms(HOMEKIT_HEALTH_INTERVAL_MS, homekit_health_log);

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

boolean DEV_GarageDoor::update()
{
    ESP_LOGI(TAG, "Garage Door Characteristics Update, door target: %s", DOOR_STATE(target->getNewVal()));
    GarageDoorCurrentState state = (target->getNewVal() == target->OPEN) ? open_door() : close_door();
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
 * Light Service Handler
 */
DEV_Light::DEV_Light(Light_t type) : Service::LightBulb()
{
    DEV_Light::type = type;
    if (type == Light_t::GDO_LIGHT)
        ESP_LOGI(TAG, "Configuring HomeKit Light Service for GDO Light");
    else if (type == Light_t::ASSIST_LASER)
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
DEV_Motion::DEV_Motion(const char *name) : Service::MotionSensor()
{
    ESP_LOGI(TAG, "Configuring HomeKit Motion Service for %s", name);
    event_q = xQueueCreate(10, sizeof(GDOEvent));
    strlcpy(this->name, name, sizeof(this->name));
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
