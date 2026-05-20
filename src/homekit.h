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
 *
 */
#pragma once

// RATGDO project includes
#include "ratgdo.h"

void setup_homekit();

extern void notify_homekit_target_door_state_change(GarageDoorTargetState state);
extern void notify_homekit_current_door_state_change(GarageDoorCurrentState state);
extern void notify_homekit_target_lock(LockTargetState state);
extern void notify_homekit_current_lock(LockCurrentState state);
extern void notify_homekit_obstruction(bool state);
extern void notify_homekit_light(bool state);
extern void enable_service_homekit_motion(bool reboot);
extern void notify_homekit_motion(bool state);

extern char qrPayload[];
extern bool homekit_setup_done;

#ifdef ESP8266
// On ESP8266 we have our own HomeKit module
void homekit_loop();

#else // not ESP8266
// One ESP32 we use HomeSpan module.
// Accessory IDs
#define HOMEKIT_AID_BRIDGE 1
#define HOMEKIT_AID_GARAGE_DOOR 2
#define HOMEKIT_AID_LIGHT_BULB 3
#define HOMEKIT_AID_MOTION 4
#define HOMEKIT_AID_ARRIVING 5
#define HOMEKIT_AID_DEPARTING 6
#define HOMEKIT_AID_VEHICLE 7
#define HOMEKIT_AID_LASER 8
#define HOMEKIT_AID_ROOM_OCCUPANCY 9
// HK-FC (fork addition): optional second GarageDoorOpener accessory.
// Created only when cfg_forceCloseHomeKit is enabled (default OFF).
#define HOMEKIT_AID_FORCE_CLOSE_DOOR 10

enum Light_t : uint8_t
{
    GDO_LIGHT = 1,
    ASSIST_LASER = 2,
};

extern void notify_homekit_vehicle_occupancy(bool vehicleDetected);
extern void notify_homekit_vehicle_arriving(bool vehicleArriving);
extern void notify_homekit_vehicle_departing(bool vehicleDeparting);
extern void notify_homekit_laser(bool on);
extern void enable_service_homekit_vehicle(bool enable);
extern bool enable_service_homekit_laser(bool enable);
extern bool enable_service_homekit_room_occupancy(bool enable);
extern void notify_homekit_room_occupancy(bool occupied);
extern bool enable_service_homekit_light(bool enable);
extern bool enable_service_homekit_motion_sensor(bool enable);
// HK-FC: runtime add/remove of the second GarageDoorOpener accessory.
// Tri-state mode: 0=off, 1=companion tile, 2=replace primary close.
// Returns true on a successful state change, false if the requested
// state already matches current state. Mode 2's close-replacement
// wires into DEV_GarageDoor::update(), not via a second accessory.
extern bool enable_service_homekit_force_close(int mode);

extern void homekit_unpair();
extern bool homekit_is_paired();
// Cycle WiFi to recover from "No Response" — HomeSpan auto-reattaches.
// MUST run in main-loop context (blocks ~750ms). For Ticker / web
// request callers, use homekit_request_reconnect instead, which is
// drained by homekit_drain_pending_reconnect() in service_timer_loop.
extern void homekit_force_reconnect(const char *reason);
// v24: deferred-reconnect entry. Safe to call from any thread / any
// context (Ticker, web request, ISR-safe). The actual reconnect runs
// in main loop via homekit_drain_pending_reconnect().
// v31: reason is a small enum (single-byte volatile, atomic on ESP32)
// instead of a strncpy'd char buffer — eliminates torn-write race
// when web handler and watchdog request reconnect concurrently.
// Same enum is reused by the v31 mdns-refresh + dump-state defer paths
// (audit #7b — fork added new HomeSpan-from-web-task entry points; we
// route them through the same drain pattern as reconnect).
enum HomekitDeferredReason : uint8_t
{
    DEFERRED_REASON_UNSPECIFIED  = 0,
    DEFERRED_REASON_WEB_UI       = 1,
    DEFERRED_REASON_WATCHDOG     = 2,
};
extern void homekit_request_reconnect(HomekitDeferredReason reason);
// v31: deferred mdns-refresh + dump-state. Web handlers and the
// watchdog auto-recover path set the request flag; the actual
// homeSpan.* call runs from service_timer_loop on loopTask, matching
// the v24 reconnect deferral. Closes the audit #7b widening.
extern void homekit_request_refresh_mdns(HomekitDeferredReason reason);
extern void homekit_request_dump_state(HomekitDeferredReason reason);
extern void homekit_drain_pending_mdns_refresh();
extern void homekit_drain_pending_state_dump();
extern void homekit_drain_pending_reconnect();
// v34 F7: split-stage WiFi reconnect — drives the post-disconnect
// delay → reconnect transition without blocking loopTask.
extern void homekit_drain_pending_reconnect_stage2();
// Re-advertise mDNS without cycling WiFi (lighter-touch recovery).
extern void homekit_refresh_mdns(const char *reason);
// Dump HomeSpan's CLI diagnostic output (status + accessory DB + diag) to log.
extern void homekit_dump_state(const char *reason);
// v22: refresh cached watchdog config (called from web settings save
// path; also called once at boot from setup_homekit). The Ticker
// callback reads the cache instead of taking the userConfig mutex.
extern void homekit_refresh_watchdog_config();
// log-audit-010 follow-up: 1Hz heap-watermark trigger entry point.
// Called from service_timer_loop on loopTask. Arms adaptive-sampler
// fast cadence (30s) immediately when freeHeap dips below the
// watermark, instead of waiting for the next 180s slow-mode sample.
// Pass freeHeap from esp_get_free_heap_size() and maxBlock from
// heap_caps_get_largest_free_block(MALLOC_CAP_8BIT). Helper handles
// the watermark check internally; safe to call every iteration.
// Watermark exposed so callers can gate the maxBlock heap read
// (observability-only) on the low-heap path. (codebase-audit-20260517-003)
constexpr uint32_t HOMEKIT_HEALTH_HEAP_WATERMARK = 20000;
extern void homekit_health_arm_fast_mode_if_low(uint32_t freeHeap, uint32_t maxBlock);
extern void homekit_health_retry_arm_if_failed(uint32_t freeHeap, uint32_t maxBlock);
extern void homekit_health_ticker_get_status(bool *outActive, uint32_t *outArmCount, uint32_t *outLastFailedMs);
extern volatile bool homekitHealthTicker_armFailed;

extern char ipv6_addresses[];

struct GDOEvent
{
    SpanCharacteristic *c;
    union
    {
        bool b;
        uint8_t u;
    } value;
};

struct DEV_GarageDoor : Service::GarageDoorOpener
{
    Characteristic::CurrentDoorState *current;
    Characteristic::TargetDoorState *target;
    Characteristic::ObstructionDetected *obstruction;
    Characteristic::LockCurrentState *lockCurrent;
    Characteristic::LockTargetState *lockTarget;

    QueueHandle_t event_q;

    DEV_GarageDoor();
    boolean update();
    void loop();
};

// HK-FC: second GarageDoorOpener accessory whose Open mirrors normal
// open and whose Close fires door_command_force_close. State mirrors
// the primary tile in lockstep; no LockMechanism characteristics
// (lock control is not a force-close concern).
struct DEV_GarageDoorForceClose : Service::GarageDoorOpener
{
    Characteristic::CurrentDoorState *current;
    Characteristic::TargetDoorState *target;
    Characteristic::ObstructionDetected *obstruction;

    QueueHandle_t event_q;

    DEV_GarageDoorForceClose();
    boolean update();
    void loop();
};

struct DEV_Info : Service::AccessoryInformation
{
    explicit DEV_Info(const char *name);
    boolean update();
};

struct DEV_Light : Service::LightBulb
{
    Characteristic::On *on;

    QueueHandle_t event_q;
    Light_t type;

    explicit DEV_Light(Light_t lightType = Light_t::GDO_LIGHT);
    boolean update();
    void loop();
};

struct DEV_Motion : Service::MotionSensor
{
    Characteristic::MotionDetected *motion;

    QueueHandle_t event_q;
    char name[16];

    explicit DEV_Motion(const char *motionName);
    void loop();
};

struct DEV_Occupancy : Service::OccupancySensor
{
    Characteristic::OccupancyDetected *occupied;

    QueueHandle_t event_q;

    DEV_Occupancy();
    void loop();
};
#endif // ESP8266
