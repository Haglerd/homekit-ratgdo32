#!/usr/bin/env python3
#
# This script converts standard web content files (html, css, etc) into a C++ language
# header file that is included in the program body.  The files are compressed and use
# PROGMEM keyword to store in Flash to save RAM.
#
# With thanks to https://github.com/mitchjs for removal of dependencies on external gzip/sed/xxd 
# 
# Copyright (c) 2023 David Kerr, https://github.com/dkerr64
#
import os
import shutil
import base64
import zlib
import gzip
import subprocess
import tempfile

#platformio
Import("env")

# exit script if cleaning
if env.GetOption("clean"):
    Return()

# source files
sourcepath = "src/www"

# ---------------------------------------------------------------------------
# Build-time minification of hand-written web assets.
#
# The device is at ~96% flash and the UI is gzipped into flash. Minifying the
# source BEFORE the CRC is computed and BEFORE gzip shrinks the embedded bytes
# without touching the readable/commented source files on disk.
#
# CRITICAL ORDERING: every consumer below (CRC32 computation, the ?v= fixed-
# point substitution loop, and the final gzip emit) must read the SAME bytes.
# If CRC were computed on the raw source but gzip emitted minified bytes, the
# `?v=<crc>` cache-busting key would not match the served body and browsers
# would hold stale assets. We therefore minify once, cache the result, and
# route ALL reads through get_asset_bytes().
#
# FAIL-SAFE: if a minifier tool is missing (not installed in this environment),
# we must NEVER break the build. We probe each tool once; if absent we embed
# the raw source bytes (today's behavior) and print a clear WARNING. Local and
# CI builds only produce identical (minified) bins when BOTH have the Node
# tools installed -- see .github/workflows note in the handoff.
# ---------------------------------------------------------------------------

# Assets we minify, keyed by the tool family. Anything not listed here (images,
# svg, qrframe, site.webmanifest, wifinets, auth, etc.) is embedded verbatim.
MINIFY_JS = {"functions.js", "logs.js", "qrcode.js"}
MINIFY_HTML = {"index.html", "logs.html", "wifiap.html"}
MINIFY_CSS = {"style.css", "wifiap.css"}


def _probe(cmd):
    """Return True if the given npx tool responds to --version."""
    try:
        r = subprocess.run(cmd + ["--version"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           timeout=20)  # --version only; keep short so a slow npx can't stall the build for minutes
        return r.returncode == 0
    except Exception:
        return False


# npx is shell-resolved on Windows, so invoke via shell=False with the resolved
# launcher. "npx --yes" auto-installs the tool from the package.json/registry if
# not cached, keeping CI hermetic once Node is set up.
_NPX = "npx.cmd" if os.name == "nt" else "npx"

# Probe each tool family exactly once.
_have_terser = _probe([_NPX, "--yes", "terser"])
_have_htmlmin = _probe([_NPX, "--yes", "html-minifier-terser"])
_have_cleancss = _probe([_NPX, "--yes", "clean-css-cli"])

if not _have_terser:
    print("WARNING: terser not available -- JS assets will be embedded UNMINIFIED")
if not _have_htmlmin:
    print("WARNING: html-minifier-terser not available -- HTML assets will be embedded UNMINIFIED")
if not _have_cleancss:
    print("WARNING: clean-css-cli not available -- CSS assets will be embedded UNMINIFIED")

# If node_modules/ is absent, `npx --yes` fetches whatever version is current in
# the registry instead of the pinned ones in package-lock.json -> minified bytes
# (and thus the ?v= CRCs) can diverge from a CI `npm ci` build. Warn loudly.
if (_have_terser or _have_htmlmin or _have_cleancss) and not os.path.isdir("node_modules"):
    print("WARNING: node_modules/ not found -- npx may use UNPINNED minifier "
          "versions, producing CRCs that differ from CI. Run `npm ci` for "
          "reproducible, CI-matching output.")

# Cache of minified (or raw, on fallback) bytes, keyed by filename.
_asset_cache = {}


def _run_minifier(make_argv, src_path, raw):
    """Run a minifier. `make_argv(src, out)` returns the full argv with input
    and output positioned correctly for that specific tool (terser requires the
    input file BEFORE its --compress/--mangle flags, otherwise the filename is
    swallowed as a flag value). Returns minified bytes, or raw on any failure
    (fail-safe)."""
    out_fd, out_path = tempfile.mkstemp(suffix=os.path.basename(src_path))
    os.close(out_fd)
    try:
        r = subprocess.run(make_argv(src_path, out_path),
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                           timeout=300)
        if r.returncode != 0 or not os.path.exists(out_path) or os.path.getsize(out_path) == 0:
            print("WARNING: minify failed for %s (%s) -- embedding raw"
                  % (src_path, (r.stderr or b"").decode(errors="replace")[:200]))
            return raw
        with open(out_path, "rb") as fo:
            return fo.read()
    except Exception as e:
        print("WARNING: minify error for %s: %s -- embedding raw" % (src_path, e))
        return raw
    finally:
        try:
            os.remove(out_path)
        except OSError:
            pass


def _node_check(data):
    """Verify JS still parses (guards against ASI breakage in terse/vendored
    code). Returns True if `node --check` passes, False otherwise."""
    fd, p = tempfile.mkstemp(suffix=".js")
    os.close(fd)
    try:
        with open(p, "wb") as f:
            f.write(data)
        node = "node.exe" if os.name == "nt" else "node"
        r = subprocess.run([node, "--check", p],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           timeout=120)
        return r.returncode == 0
    except Exception:
        return False  # if node itself is missing, be conservative
    finally:
        try:
            os.remove(p)
        except OSError:
            pass


def _sane_minify(out, raw, file):
    """Catch catastrophic minifier output (truncated-but-nonzero) that the
    zero-byte guard in _run_minifier misses. The JS path has node --check;
    HTML/CSS have no parse gate, so guard against an implausibly small result
    (which would silently embed a blank/broken page). <10% of source = bogus."""
    if out is not raw and len(out) < max(1, len(raw) // 10):
        print("WARNING: %s minified output implausibly small (%d << %d) -- embedding raw"
              % (file, len(out), len(raw)))
        return raw
    return out


def get_asset_bytes(file):
    """Return the bytes to embed for `file`: minified when a suitable tool is
    available and the result is safe, otherwise the raw source bytes.
    Result is cached so CRC / fixed-point loop / gzip all see identical bytes."""
    if file in _asset_cache:
        return _asset_cache[file]

    src_path = sourcepath + "/" + file
    with open(src_path, "rb") as f:
        raw = f.read()
    result = raw

    if file in MINIFY_JS and _have_terser:
        # Safe options only: no --compress 'unsafe*', no eval. mangle of local
        # names is semantics-preserving. Input file MUST precede the flags
        # (terser treats "--compress <next-token>" as the compress options value).
        out = _run_minifier(
            lambda s, o: [_NPX, "--yes", "terser", s, "--compress", "--mangle", "-o", o],
            src_path, raw)
        # Conservative gate for terse/vendored code (qrcode.js, inlined MD5):
        # only accept the minified output if it still parses.
        if out is not raw and _node_check(out):
            result = out
        elif out is not raw:
            print("WARNING: %s failed node --check after minify -- embedding raw" % file)
    elif file in MINIFY_HTML and _have_htmlmin:
        out = _run_minifier(
            lambda s, o: [_NPX, "--yes", "html-minifier-terser",
                          "--collapse-whitespace", "--remove-comments",
                          "--minify-css", "true", "--minify-js", "true", s, "-o", o],
            src_path, raw)
        result = _sane_minify(out, raw, file)
    elif file in MINIFY_CSS and _have_cleancss:
        out = _run_minifier(
            lambda s, o: [_NPX, "--yes", "clean-css-cli", "-O2", s, "-o", o],
            src_path, raw)
        result = _sane_minify(out, raw, file)

    if result is not raw:
        print("minified %s: %d -> %d bytes (pre-gzip)" % (file, len(raw), len(result)))
    _asset_cache[file] = result
    return result
# compress .gz files into build folder (build/{env name}/www)
targetpath = os.path.join(env["PROJECT_BUILD_DIR"], env["PIOENV"], "www")
# final built include file subfolder
includepath = os.path.join(targetpath, "include")
# relative path for build_flags (uses /)
cIncludePath = os.path.relpath(includepath, env["PROJECT_DIR"]).replace("\\", "/")
# add include file path to build_flags
env.Append(BUILD_FLAGS=[f"-I {cIncludePath}"])
#print(env.get("BUILD_FLAGS"))

filenames = next(os.walk(sourcepath), (None, None, []))[2]
print("Compressing and converting files from " + sourcepath + " into " + targetpath)

# Start by deleting the target directory, then creating empty one.
try:
    shutil.rmtree(targetpath)
except FileNotFoundError:
    pass
# make the full path folder stucture
os.makedirs(includepath)

# calculate a CRC32 for each file and base64 encode it, this will change if the
# file contents are changed.  We use this to control browser caching.
file_crc = {}
for file in filenames:
    # skip hidden files
    if file[0] == ".":
        continue

    # skip status.json
    if file == "status.json":
        continue

    # skip js.map files
    if file.endswith(".js.map"):
        continue

    # skip markdown docs (e.g. icons/README.md) - not served, saves flash
    if file.endswith(".md"):
        continue

    # Read the bytes we will actually embed (minified when available). The CRC
    # MUST be computed on these post-minify bytes so the ?v=<crc> cache key
    # matches the served body.
    data = get_asset_bytes(file)
    crc32 = (
        base64.urlsafe_b64encode(zlib.crc32(data).to_bytes(4, byteorder="big"))
        .decode()
        .replace("=", "")
    )
    file_crc[file] = crc32
    print("CRC: " + crc32 + " (" + file + ")")

# An HTML/JS file's URL hash (`?v=<crc>`) is the browser's cache key.
# Below we substitute every `<file>?v=CRC-32` placeholder with the real
# per-file CRC; that means the *served* body of an HTML file changes
# whenever any file it references changes — but its OWN crc, computed
# above from the unsubstituted source, doesn't. Browsers (with web.cpp
# CACHE_CONTROL = 30 days) hold onto the stale HTML and never re-fetch
# the new JS, so a JS-only fix appears unshipped. Recompute HTML/JS
# CRCs from their post-substitution bytes; iterate to a fixed point so
# index.html → logs.html → asset chains converge.
for _pass in range(8):
    changed = False
    for file in filenames:
        if file[0] == "." or file == "status.json" or file.endswith(".js.map") or file.endswith(".md"):
            continue
        t = file.rpartition(".")[-1]
        if t not in ("html", "htm", "js"):
            continue
        # Use the same (cached) minified bytes the CRC was computed from.
        data = get_asset_bytes(file)
        for f_name, c in file_crc.items():
            data = data.replace(
                bytes(f_name + "?v=CRC-32", "utf-8"),
                bytes(f_name + "?v=" + c, "utf-8"),
            )
        new_crc = (
            base64.urlsafe_b64encode(zlib.crc32(data).to_bytes(4, byteorder="big"))
            .decode()
            .replace("=", "")
        )
        if file_crc[file] != new_crc:
            print("CRC update: " + file_crc[file] + " -> " + new_crc + " (" + file + ")")
            file_crc[file] = new_crc
            changed = True
    if not changed:
        break

# Open webcontent file and write warning header...
wf = open(includepath  + "/webcontent.h", "w")
wf.write("/**************************************\n")
wf.write(" * Autogenerated DO NOT EDIT\n")
wf.write(" **************************************/\n")
wf.write("#include <unordered_map>\n")
wf.write("#include <string>\n")
wf.flush()

varnames = []
# now loop through each file...
for file in filenames:
    # skip hidden files
    if file[0] == ".":
        continue

    # skip status.json
    if file == "status.json":
        continue

    # skip js.map files
    if file.endswith(".js.map"):
        continue

    # skip markdown docs (e.g. icons/README.md) - not served, saves flash
    if file.endswith(".md"):
        continue

    # create gzip file name
    gzfile = targetpath + "/" + file + ".gz"
    # create variable names
    varname = "www_" + file + ".gz"
    varnames.append(("/" + file, varname.replace(".", "_").replace("/", "_").replace("-", "_"), file_crc[file]))
    # get file type
    t = file.rpartition(".")[-1]
    # if file matches, add true crc to ?v=CRC-32 marker and create the gzip
    if (t == "html") or (t == "htm") or (t == "js"):
        with gzip.open(gzfile, 'wb') as f_out:
            # use the same (cached) minified bytes the CRC was computed from
            data = get_asset_bytes(file)
            # loop through each file that could be referenced
            for f_name, crc32 in file_crc.items():
                # Replace the target string with real crc
                data = data.replace(bytes(f_name + "?v=CRC-32", 'utf-8'), bytes(f_name + "?v=" + crc32, 'utf-8'))
            f_out.write(data)
            f_out.close()
    else :
        # Non-html/js assets: still routed through get_asset_bytes so CSS gets
        # minified; images / svg / etc. fall through to raw bytes.
        with gzip.open(gzfile, 'wb') as f_out:
            f_out.write(get_asset_bytes(file))
            f_out.close()
   
    # create the 'c' code
    # const unsigned char src_www_build_apple_touch_icon_png_gz[] PROGMEM = {
    # const unsigned int src_www_build_apple_touch_icon_png_gz_len = 2721;
    wf.write("const unsigned char %s[] PROGMEM = {\n" % varname.replace(".", "_").replace("/", "_").replace("-", "_") )
    count = 0
    with open(gzfile, 'rb') as f:
        bytes_read = f.read(12)
        while bytes_read:
            count = count + len(bytes_read)
            wf.write('  ')
            for b in bytes_read:
                wf.write('0x%02X,' % b)
            wf.write('\n')
            bytes_read = f.read(12)
    
    wf.write('};\n')
    wf.write("const unsigned int %s_len = %d;\n\n" % (varname.replace(".", "_").replace("/", "_").replace("-", "_"), count) )

wf.flush()

# Add possible MIME types to the file...
wf.write(
    """
const char type_svg[]  PROGMEM = "image/svg+xml";
const char type_bmp[]  PROGMEM = "image/bmp";
const char type_gif[]  PROGMEM = "image/gif";
const char type_jpeg[] PROGMEM = "image/jpeg";
const char type_jpg[]  PROGMEM = "image/jpeg";
const char type_png[]  PROGMEM = "image/png";
const char type_tiff[] PROGMEM = "image/tiff";
const char type_tif[]  PROGMEM = "image/tiff";
const char type_ico[]  PROGMEM = "image/x-icon";
const char type_txt[]  PROGMEM = "text/plain";
const char type_[]     PROGMEM = "text/plain";
const char type_htm[]  PROGMEM = "text/html";
const char type_html[] PROGMEM = "text/html";
const char type_css[]  PROGMEM = "text/css";
const char type_js[]   PROGMEM = "text/javascript";
const char type_mjs[]  PROGMEM = "text/javascript";
const char type_json[] PROGMEM = "application/json";
const char type_webmanifest[] PROGMEM = "application/manifest+json";
// Must be at least one more than max string above...
#define MAX_MIME_TYPE_LEN 32
"""
)

# Use an unordered_map so we can lookup the data, length and type based on filename...
wf.write(
    """
struct pageContent
{
    const unsigned char *data;
    const unsigned int length;
    const char *type;
    const std::string crc32;
};

const std::unordered_map<std::string, pageContent> webcontent = {
    """
)
n = 0
for file, var, crc32 in varnames:
    t = ""
    if file.find(".") > 0:
        t = file.rpartition(".")[-1]
    # Need comma at end of every line except last one...
    if n > 0:
        wf.write(",")
    wf.write('\n  { "' + file + '", {' + var + ", " + var + "_len, type_" + t + ', "' + crc32 + '"' + "} }")
    n = n + 1

# All done, close the file...
wf.write("\n\n};\n")
wf.close()

print("processed " + str(len(varnames)) + " files")
