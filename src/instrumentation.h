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

// v45 (audit W41): cross-TU instrumentation counters consumed by the periodic
// homekit_health_log diag-log line. Centralized here so a type mismatch
// between declaration and definition fails at compile time, and so future
// counters land in one place instead of accreting `extern` forward-decls
// inside consumer functions.

#include <stdint.h>

// Defined in log.cpp.
//   logMtxMaxWaitMs : max log-mutex wait this 180s window (climbing
//                     pre-freeze = wedged SSE subscriber blocking the
//                     broadcast).
extern volatile uint32_t logMtxMaxWaitMs;

//   syslogDrops     : cumulative dropped syslog packets (queue-full or UDP
//                     send error). Not yet read by homekit_health_log;
//                     declared pre-emptively so the next consumer doesn't
//                     repeat the inline-extern leak this header was created
//                     to prevent.
extern volatile uint32_t syslogDrops;

// Defined in web.cpp.
//   sseSlowWrites      : SSE writes > CLIENT_SLOW_WRITE_MS since boot.
extern volatile uint32_t sseSlowWrites;

//   sseBufferFullSkips : cumulative lwIP-send-buffer-full skips since boot
//                        (flow-control diagnostic; trend matters more than
//                        absolute).
extern volatile uint32_t sseBufferFullSkips;

//   sseSlotsAlloc      : live count refreshed by sweep_sse_orphans.
extern volatile uint32_t sseSlotsAlloc;

//   sseOrphansReaped   : per-window counter, atomic-exchange-zeroed by
//                        homekit_health_log.
extern volatile uint32_t sseOrphansReaped;

//   statusJsonPeakLen  : peak JSON length this window. Informs future
//                        STATUS_JSON_BUFFER_SIZE retune decision.
//                        (MH6 instrumentation.)
extern volatile uint32_t statusJsonPeakLen;
