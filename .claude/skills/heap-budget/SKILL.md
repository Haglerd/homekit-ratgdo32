---
name: heap-budget
description: Quantify the heap-impact of a firmware change for both ESP32 and ESP8266 builds. Use when adding/changing buffers, JSON sizes, or static allocations.
---

# Heap budget analysis

## When to invoke

- Adding a new buffer (`char[]`, `uint8_t[]`, `String`, `JsonDocument`)
- Bumping any `*_BUF_SIZE` / `*_LEN` / `*_CAPACITY` constant
- Adding subscriber/listener tables that grow with connections
- Anything that touches SSE, mDNS TXT records, JSON serialization

## Process

### 1. Compute static delta
- Sum all new/changed allocation sizes (bytes, fixed-cost-only)
- Multiply per-instance allocations by max instance count

### 2. Compute peak dynamic delta
- For each new dynamic allocation, identify the worst-case path
- Multiply by max simultaneous connections (SSE subscribers, web clients, etc.)

### 3. Compare against budget

| Board | Free heap at boot (typical) | Headroom for new features |
|-------|------------------------------|---------------------------|
| ESP32 | ~190KB | ~50KB before HomeKit instability |
| ESP8266 | ~30–40KB | **~5KB** before crashes |

### 4. Decide

- **ESP32 only feature** (>5KB delta): wrap in `#ifdef ESP32`
- **Both boards** (<5KB total delta): proceed
- **Both boards but >5KB**: refactor or reject

### 5. Document in plan/PR
```
Heap impact:
- Static: +X bytes
- Dynamic worst-case: +Y bytes (Z subscribers × W bytes each)
- ESP32 headroom remaining: ~AA KB
- ESP8266 headroom remaining: ~BB KB
- Decision: [ship to both | ESP32-only #ifdef | reject]
```

## Real precedent

- Issue #316 (sibling repo): 22KB/hour SSE leak crashed devices. Heap is religion here.
- Status JSON buffer was 256, bumped to 512 only after explicit headroom analysis.
