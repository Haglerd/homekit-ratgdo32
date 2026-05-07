/****************************************************************************
 * RATGDO HomeKit
 * https://ratcloud.llc
 * https://github.com/PaulWieland/ratgdo
 *
 * Copyright (c) 2023-25 David A Kerr... https://github.com/dkerr64/
 * All Rights Reserved.
 * Licensed under terms of the GPL-3.0 License.
 *
 */
#pragma once

// Centralized declarations of fork-added instrumentation counters.
// Definitions live in their respective .cpp TUs:
//   logMtxMaxWaitMs, syslogDrops          -> log.cpp
//   sseSlowWrites, sseBufferFullSkips,
//   sseSlotsAlloc, sseOrphansReaped,
//   statusJsonPeakLen                     -> web.cpp
//
// All counters are accessed via __atomic_* builtins (ATOMIC_RELAXED) from
// multiple FreeRTOS task contexts (loopTask, esp_timer task, async web
// handlers). The volatile qualifier is preserved here for consistency with
// the definition sites; the __atomic_* builtins provide the real ordering.
//
// ESP8266 portability: extern decls are language-level name binding only,
// no codegen impact. volatile uint32_t storage is identical across the
// xtensa-lx106 (ESP8266) and xtensa-lx7 (ESP32) toolchains.

#include <stdint.h>

extern volatile uint32_t logMtxMaxWaitMs;
extern volatile uint32_t sseSlowWrites;
extern volatile uint32_t sseBufferFullSkips;
extern volatile uint32_t sseSlotsAlloc;
extern volatile uint32_t sseOrphansReaped;
extern volatile uint32_t statusJsonPeakLen;
extern volatile uint32_t syslogDrops;
