#!/usr/bin/env bash
# verify-ota.sh <version>   e.g.  verify-ota.sh v3.4.4-forceclose.95
#
# Gates an OTA release on the DEVICE-FACING state, not the GitHub release.
# The device's "Update from GitHub" button (src/www/functions.js ~1071-1268)
# does NOT download from the GitHub release assets — it:
#   1. queries the GitHub releases API for the latest non-prerelease,
#   2. downloads the firmware.bin + firmware.md5 from GitHub *Pages*:
#        https://haglerd.github.io/homekit-ratgdo32/firmware/<asset>
#      (release.yml commits the 4 bins to docs/firmware/ on main, then Pages
#       redeploys — which lags the release build by ~1-2 min).
#
# So a release can exist with all assets attached while the device still
# 404s on the Pages firmware/ + md5 URLs. During that window the device UI
# shows "no firmware binary attached yet" (bin 404) and
# "Firmware MD5 checksum file not found on GitHub. Continue anyway?" (md5 404)
# — i.e. "it isn't there" / "the md5 files are fucked".
#
# THIS SCRIPT IS THE CORRECT done-gate: it polls the Pages URLs the device
# actually uses until all are 200 with a matching md5, or times out. Only
# announce an OTA as ready after this exits 0.
set -uo pipefail

VER="${1:?usage: verify-ota.sh <version e.g. v3.4.4-forceclose.95>}"
USER_LC="haglerd"; REPO="homekit-ratgdo32"
PAGES="https://${USER_LC}.github.io/${REPO}"
FW="${PAGES}/firmware/${REPO}-${VER}"
API="https://api.github.com/repos/Haglerd/${REPO}/releases"
RETRIES="${RETRIES:-20}"; SLEEP="${SLEEP:-30}"   # ~10 min max wait for Pages

http() { curl -s -o /dev/null -w "%{http_code}" "$1"; }

check_once() {
  local ok=1
  # 1. all 4 device-facing Pages assets must be 200
  for f in firmware.bin firmware.md5 bootloader.bin partitions.bin; do
    local code; code=$(http "${FW}.${f}")
    if [ "$code" != "200" ]; then echo "  [pending] ${VER}.${f} -> HTTP $code"; ok=0; fi
  done
  [ "$ok" = 1 ] || return 1
  # 2. md5 file must be exactly 32 lowercase hex chars (no filename, no newline cruft)
  local md5file; md5file=$(curl -s "${FW}.firmware.md5" | tr -d '[:space:]')
  if ! printf '%s' "$md5file" | grep -qE '^[0-9a-f]{32}$'; then
    echo "  [bad md5] '${md5file}' is not 32 hex chars"; return 1
  fi
  # 3. recompute md5 of the Pages-served bin and compare
  local tmp; tmp=$(mktemp); curl -sL -o "$tmp" "${FW}.firmware.bin"
  local got; got=$(md5sum "$tmp" | cut -d' ' -f1); rm -f "$tmp"
  if [ "$got" != "$md5file" ]; then
    echo "  [mismatch] pages bin md5 $got != md5 file $md5file"; return 1
  fi
  # 4. releases API: VER must be the newest non-prerelease (what the UI picks)
  local latest; latest=$(curl -s "$API" \
    | python -c "import json,sys; r=json.load(sys.stdin); n=[x for x in sorted(r,key=lambda y:y['created_at'],reverse=True) if not x['prerelease']]; print(n[0]['tag_name'] if n else '')" 2>/dev/null)
  if [ "$latest" != "$VER" ]; then
    echo "  [api] latest non-prerelease is '${latest}', expected '${VER}'"; return 1
  fi
  # 5. Pages manifest.json (ESP Web Tools / browser installer) must match
  local mver; mver=$(curl -s "${PAGES}/manifest.json" | grep -oE '"version" *: *"[^"]+"' | sed -E 's/.*"([^"]+)"$/\1/')
  if [ "$mver" != "$VER" ]; then
    echo "  [manifest] Pages manifest.json version '${mver}' != '${VER}'"; return 1
  fi
  echo "  OK: 4 Pages assets 200, md5 ${md5file} verified vs bin, API latest=${VER}, manifest=${VER}"
  return 0
}

echo "Verifying device-facing OTA for ${VER} (Pages docs/firmware + releases API + manifest)..."
for i in $(seq 1 "$RETRIES"); do
  echo "attempt ${i}/${RETRIES}:"
  if check_once; then
    echo "READY: ${VER} is live on the device-facing OTA path. Safe to flash."
    exit 0
  fi
  [ "$i" -lt "$RETRIES" ] && { echo "  ...Pages not fully live yet, waiting ${SLEEP}s"; sleep "$SLEEP"; }
done
echo "TIMED OUT: ${VER} did not reach a fully-live device-facing state. Do NOT announce as ready."
exit 1
