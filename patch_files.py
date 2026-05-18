#!/usr/bin/env python3
#
# This script runs patch command on specified files.
#
# fork patch invocations:
#   - hap_heap_gate.patch (log-audit-20260517-001) — HAP preflight gate
#     applied against the vendored HomeSpan TempBuffer ctor in HAP.cpp.
#     Without it, low-heap HomeKit requests crash on the TempBuffer ctor
#     (observed .84 crash 2026-05-17 08:20 CDT at free=324 B).
#
# Copyright (c) 2024-25 David Kerr, https://github.com/dkerr64
#
import os

Import("env")
#print(env['PROJECT_PACKAGES_DIR']);
#print(env['PROJECT_LIBDEPS_DIR']);

if os.name == "nt":
    # On Windows the dev environment doesn't ship the POSIX `patch` binary
    # by default; the build is exercised by CI on Linux. Local Windows
    # builds will use whatever was last patched in `.pio/libdeps/...`.
    # If patches need a refresh, run `patch -N -p0 < <patchfile>` manually
    # from a bash shell.
    pass
else:
    # Idempotent re-apply (-N skips already-applied hunks). -p0 because the
    # patch files use relative paths from the repo root.
    repo_root = env['PROJECT_DIR']
    libdeps = env['PROJECT_LIBDEPS_DIR']
    pio_env = env['PIOENV']  # e.g. "ratgdo_esp32dev"

    hap_path = os.path.join(libdeps, pio_env, "HomeSpan", "src", "HAP.cpp")
    hap_patch = os.path.join(repo_root, "hap_heap_gate.patch")
    if os.path.exists(hap_path) and os.path.exists(hap_patch):
        os.system("patch -N -p0 " + hap_path + " " + hap_patch)
