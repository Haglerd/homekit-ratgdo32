#!/usr/bin/env bash
# Post-edit hook: warn when buffer-size or static-allocation constants change
# in firmware files. Reminds to run the heap-budget skill before commit.
# Non-blocking — emits stderr warning, always exits 0.

input=$(cat)
file=$(echo "$input" | grep -oE '"file_path"[[:space:]]*:[[:space:]]*"[^"]*"' | sed -E 's/.*"file_path"[[:space:]]*:[[:space:]]*"([^"]*)".*/\1/')

# Only fire for firmware source files (relative path or absolute)
if ! echo "$file" | grep -qE '(^|/)src/.*\.(cpp|c|h|hpp)$'; then
  exit 0
fi

# Look for buffer-size / static-allocation patterns in the tool input.
# Patterns: anything ending in _SIZE/_LEN/_CAPACITY (constants),
# JsonDocument<N> / StaticJsonDocument<N> (ArduinoJson), char/uint8_t [N] arrays,
# specific known-load-bearing buffers (writeBuffer, outLine, status_json).
if echo "$input" | grep -qE '(_BUF_SIZE|_BUFSIZE|_SIZE[[:space:]]*=|_LEN[[:space:]]*=|_CAPACITY|JsonDocument<|StaticJsonDocument|(char|uint8_t)[[:space:]]+[a-zA-Z_]+\[[0-9]+\]|writeBuffer|outLine|status_json|LINE_BUFFER|RTC_NOINIT)'; then
  echo "[post-edit heap-budget] $file may have changed buffer/allocation sizing." >&2
  echo "                        Run the heap-budget skill before commit:" >&2
  echo "                        - quantify static + dynamic delta in bytes" >&2
  echo "                        - compare against ESP8266 budget (very tight, ~5KB headroom)" >&2
  echo "                        - if >5KB, refactor or wrap in #ifdef ESP32" >&2
fi

exit 0
