/***********************************************************************
 * homekit-ratgdo logger web page javascript functions
 *
 * Copyright (c) 2024-25 David Kerr, https://github.com/dkerr64
 *
 */

// Global vars...
var evtSource = undefined;      // for Server Sent Events (SSE)
var msgJson = undefined;        // for status
// v27: persist the SSE client UUID in localStorage so a page reload
// reuses the same subscription slot instead of leaking the previous one.
// Pre-v27, every reload allocated a fresh UUID + slot; with the firmware
// orphan sweep on a 15s pre-handshake / 120s idle timeout, you could
// fill all 8 SSE_MAX_CHANNELS by reloading logs.html ~6 times in 15s.
// uuidv4 is hoisted (function declaration further down), so the IIFE
// can call it from this earlier line.
const clientUUID = (function () {
    try {
        const KEY = 'ratgdo-logs-uuid';
        let id = localStorage.getItem(KEY);
        if (!id) {
            id = uuidv4();
            localStorage.setItem(KEY, id);
        }
        return id;
    } catch (e) {
        // Storage blocked (private browsing on iOS Safari, restrictive
        // tracking-prevention modes). Fall back to a per-session UUID —
        // not ideal, but the firmware sweep still catches the leak.
        return uuidv4();
    }
})();
// v27: best-effort cleanup. Browser releases the SSE slot on page-unload
// without waiting for the firmware orphan sweep timeout. sendBeacon does
// not block navigation and survives backgrounding on most browsers; on
// the ones it doesn't (mobile Safari background tabs), the firmware
// sweep is the safety net. No CSRF token: sendBeacon can't set custom
// headers, and the endpoint is intentionally unauthenticated (worst
// case is closing your own session early).
window.addEventListener('beforeunload', () => {
    try {
        navigator.sendBeacon('rest/events/unsubscribe?id=' + clientUUID);
    } catch (e) {
        // sendBeacon unsupported — orphan sweep will catch it
    }
});
var sysLogLoaded = false;
var tmpLogMsgs = [];
// v51: lightweight /showlog poll as a fallback to SSE. Even when SSE is
// wedged on flow-control (browser tab can't drain TCP fast enough — common
// during homebridge poll storms or DOM-render-heavy moments), the page
// stays current via this poll. Compares the FULL buffer text to the
// last-seen content and only inserts the suffix that's actually new.
// 3-second interval is "feels live" without flooding the device with
// /showlog GETs (which are auth'd and serialize through the WebServer
// task). Stops when the page is hidden via Page Visibility API to avoid
// background-tab CPU/network burn.
const SHOWLOG_POLL_INTERVAL_MS = 3000;
var showlogPoller = undefined;
var lastShowlogContent = "";

function msToTime(duration) {
    let seconds = Math.floor((duration / 1000) % 60),
        minutes = Math.floor((duration / (1000 * 60)) % 60),
        hours = Math.floor((duration / (1000 * 60 * 60)) % 24),
        days = Math.floor((duration / (1000 * 60 * 60 * 24)));

    hours = (hours < 10) ? "0" + hours : hours;
    minutes = (minutes < 10) ? "0" + minutes : minutes;
    seconds = (seconds < 10) ? "0" + seconds : seconds;

    return days + " days " + hours + " hrs " + minutes + " mins " + seconds + " secs";
}

function openTab(evt, tabName) {
    var i, tabcontent, tablinks;
    // Get all elements with class="tabcontent" and hide them
    tabcontent = document.getElementsByClassName("tabcontent");
    for (i = 0; i < tabcontent.length; i++) {
        tabcontent[i].style.display = "none";
    }
    document.getElementById("clearLogBtn").style.display = "none";
    document.getElementById("reloadLogButton").style.display = "none";
    document.getElementById("clearBtn").style.display = "none";
    // Get all elements with class="tablinks" and remove the class "active"
    tablinks = document.getElementsByClassName("tablinks");
    for (i = 0; i < tablinks.length; i++) {
        tablinks[i].className = tablinks[i].className.replace(" active", "");
    }
    // Show the current tab, and add an "active" class to the button that opened the tab
    document.getElementById(tabName).style.display = "block";
    evt.currentTarget.className += " active";
    if (tabName === "logTab") {
        document.getElementById("clearLogBtn").style.display = "inline-block";
        document.getElementById("reloadLogButton").style.display = "inline-block";
    } else if (tabName === "crashTab") {
        if (msgJson?.crashCount != 0) {
            document.getElementById("clearBtn").style.display = "inline-block";
        }
    } else if (tabName === "statusTab") {
        // Refresh status from the server
        loaderElem.style.visibility = "visible";
        document.getElementById("statusjson").innerText = "";
        fetch("status.json")
            .then((response) => {
                if (!response.ok || response.status !== 200) {
                    reject(`Error requsting status.json, RC: ${response.status}`);
                } else {
                    return response.text();
                }
            })
            .then((text) => {
                msgJson = JSON.parse(text);
                document.getElementById("statusjson").innerText = text;
                loaderElem.style.visibility = "hidden";
            })
            .catch(error => console.warn(error));
    }
}

async function loadLogs() {
    sysLogLoaded = false;
    tmpLogMsgs.length = 0;
    loaderElem.style.visibility = "visible";
    // v52: pure polling design. No SSE setup, no /auth call.
    // /showlog is auth'd via standard browser Digest cache (no
    // per-IP allowlist needed for non-SSE GETs). Initial load
    // fetches /showlog + /showrebootlog + /crashlog + /status.json
    // in parallel, then startShowlogPoller takes over for live
    // updates every SHOWLOG_POLL_INTERVAL_MS.
    //
    // History note: v40-v51 used SSE for log streaming. The SSE
    // path had repeated reconnect-cascade issues (sweep reap →
    // close → re-subscribe → /showlog re-fetch → PREPEND duplicates
    // → polling next tick clears panes → flicker). Polling is
    // simpler, more reliable, "live enough" at 3s cadence. The
    // home page (functions.js) still uses SSE because it has a
    // local 1-Hz uptime ticker that masks any SSE wedge.
    //
    // Clear out v51's localStorage skip-auth key (no longer used).
    try { localStorage.removeItem('ratgdo-logs-last-auth-at'); } catch (e) { /* ignore */ }
    loadLogPages();
}

async function loadLogPages() {
    // Load the pages in background
    Promise.allSettled([

        fetch("showlog")
            .then((response) => {
                if (!response.ok || response.status !== 200) {
                    reject(`Error requesting logs, RC: ${response.status}`);
                } else {
                    return response.text();
                }
            })
            .then((text) => {
                sysLogLoaded = true;
                // reduce newlines down to single \n
                text = text.replaceAll('\r\n', '\n');
                while (line = tmpLogMsgs.pop()) {
                    console.log(`Remove dup: ${line}`);
                    text = text.replace(line + '\n', '');
                }
                // v22: split the buffered showlog into HomeKit lines vs
                // system lines and seed each tab with ONLY its own
                // content. Mirror behaviour of the live SSE handler so
                // homekit lines never appear in the system log.
                const lines = text.split('\n');
                const hkLines = lines.filter(isHomeKitLine);
                const sysLines = lines.filter(l => !isHomeKitLine(l));
                document.getElementById("showlog").insertAdjacentText('afterbegin', sysLines.join('\n'));
                let divElem = document.getElementById("logTab");
                divElem.scrollTop = divElem.scrollHeight;
                if (hkLines.length > 0) {
                    document.getElementById("homekitlog").insertAdjacentText('afterbegin', hkLines.join('\n') + '\n');
                    let hkPane = document.getElementById("homekitTab");
                    hkPane.scrollTop = hkPane.scrollHeight;
                }
            })
            .catch(error => console.warn(error)),

        fetch("status.json")
            .then((response) => {
                if (!response.ok || response.status !== 200) {
                    reject(`Error requesting status.json, RC: ${response.status}`);
                } else {
                    return response.text();
                }
            })
            .then((text) => {
                msgJson = JSON.parse(text);
                document.getElementById("deviceName").innerHTML = msgJson.deviceName;
                document.title = msgJson.deviceName;
                document.getElementById("statusjson").innerText = text;
            })
            .catch(error => console.warn(error)),

        fetch("showrebootlog")
            .then((response) => {
                if (!response.ok || response.status !== 200) {
                    reject(`Error requesting reboot logs, RC: ${response.status}`);
                } else {
                    return response.text();
                }
            })
            .then((text) => {
                document.getElementById("rebootlog").innerText = text;
            })
            .catch(error => console.warn(error)),

        fetch("crashlog")
            .then((response) => {
                if (!response.ok || response.status !== 200) {
                    reject(`Error requesting crash logs, RC: ${response.status}`);
                } else {
                    return response.text();
                }
            })
            .then((text) => {
                document.getElementById("crashlog").innerText = text;
            })
            .catch(error => console.warn(error)),
    ])
        .then((results) => {
            // Once all loaded reset the progress indicator
            loaderElem.style.visibility = "hidden";
            console.log("All logs loaded");
            //console.log(results);
            // v51: kick off the /showlog polling fallback after the
            // initial buffer is loaded. If SSE is delivering events
            // live, this is mostly idempotent (lastShowlogContent
            // already covers the SSE-appended lines). If SSE is
            // wedged, this is what makes the page feel live.
            startShowlogPoller();
        });
}

// v51: poll /showlog every SHOWLOG_POLL_INTERVAL_MS and append any
// content the user hasn't seen yet. Diffs the full buffer text against
// `lastShowlogContent` — when the device's ring buffer adds new lines,
// the suffix grows; when the buffer wraps, the diff falls back to a
// full replace (rare, only happens under sustained log volume).
//
// Pauses while the tab is hidden (Page Visibility API) — browsers
// throttle background tabs anyway, and this avoids burning the
// device's serialised WebServer task on /showlog GETs the user
// can't see. Resumes immediately on visibilitychange-to-visible.
function startShowlogPoller() {
    if (showlogPoller !== undefined) return; // already running
    // Seed lastShowlogContent with the initial /showlog text so the
    // first poll diff is a no-op (avoids replaying the same content
    // into the DOM). Read what's already in the showlog pre tag.
    lastShowlogContent = document.getElementById("showlog").innerText || "";
    const tick = async () => {
        if (document.visibilityState === "hidden") return;
        try {
            const resp = await fetch("showlog");
            if (!resp.ok || resp.status !== 200) return;
            let text = await resp.text();
            text = text.replaceAll('\r\n', '\n');
            if (text === lastShowlogContent) return; // nothing new
            // Find the suffix that's new since last poll. If the
            // device's buffer wrapped (rare), text won't start with
            // lastShowlogContent — fall back to full replace then.
            let newPart = "";
            if (lastShowlogContent && text.startsWith(lastShowlogContent)) {
                newPart = text.slice(lastShowlogContent.length);
            } else if (lastShowlogContent) {
                // Buffer wrapped — replace everything to stay consistent.
                document.getElementById("showlog").innerText = "";
                document.getElementById("homekitlog").innerText = "";
                newPart = text;
            } else {
                newPart = text;
            }
            lastShowlogContent = text;
            if (!newPart.trim()) return;
            // Same split-by-isHomeKitLine logic as the SSE path uses.
            const newLines = newPart.split('\n').filter(l => l.length > 0);
            const hkLines = newLines.filter(isHomeKitLine);
            const sysLines = newLines.filter(l => !isHomeKitLine(l));
            if (sysLines.length > 0) {
                const divElem = document.getElementById("logTab");
                const scroll = (divElem.scrollHeight - divElem.scrollTop - divElem.clientHeight) < 10;
                document.getElementById("showlog").insertAdjacentText('beforeend', sysLines.join('\n') + '\n');
                if (scroll) divElem.scrollTop = divElem.scrollHeight;
            }
            if (hkLines.length > 0) {
                const hkPane = document.getElementById("homekitTab");
                const hkScroll = (hkPane.scrollHeight - hkPane.scrollTop - hkPane.clientHeight) < 10;
                document.getElementById("homekitlog").insertAdjacentText('beforeend', hkLines.join('\n') + '\n');
                if (hkScroll) hkPane.scrollTop = hkPane.scrollHeight;
            }
        } catch (e) { /* network blip — try again next interval */ }
    };
    showlogPoller = setInterval(tick, SHOWLOG_POLL_INTERVAL_MS);
    // Tick immediately on visibilitychange-to-visible so the user
    // doesn't wait up to SHOWLOG_POLL_INTERVAL_MS after un-hiding.
    document.addEventListener("visibilitychange", () => {
        if (document.visibilityState === "visible") tick();
    });
}

async function clearLog(reload) {
    // Erase current content
    document.getElementById("showlog").innerText = "";
    document.getElementById("showLogHeader").innerHTML = "";
    // Load logs
    if (reload) loadLogs();
}

async function clearCrashLog() {
    loaderElem.style.visibility = "visible";
    await fetch('clearcrashlog');
    document.getElementById("clearBtn").style.display = "none";
    if (msgJson) msgJson.crashCount = 0;
    document.getElementById("crashlog").innerText = "No crashes saved";
    loaderElem.style.visibility = "hidden";
}

// Match HomeKit / WiFi / HomeSpan-related lines from the system log
// stream. Used by both the live SSE subscription and the initial fetch
// of /showlog when the HomeKit tab opens.
function isHomeKitLine(line) {
    // v43 (audit W28): collapsed `WiFi |Wifi |wifi ` (redundant under /i),
    // dropped `HomeKit reconnect` (subsumed by `HomeKit `), removed
    // `force-close to clear` (no firmware ESP_LOG emits that string).
    return /ratgdo-homekit|HomeKit |HomeSpan|WiFi /i.test(line);
}

async function reconnectHomeKit() {
    const status = document.getElementById("reconnectStatus");
    if (!confirm("Reconnect HomeKit? This cycles WiFi briefly (no reboot).")) return;
    status.textContent = "Sending…";
    try {
        const res = await fetch("reconnectHomeKit", { method: "POST" });
        if (res.ok) {
            status.textContent = "OK — WiFi cycling. Watch the log below.";
            status.style.color = "#3a7a3a";
        } else {
            status.textContent = `Failed (HTTP ${res.status})`;
            status.style.color = "#a33";
        }
    } catch (e) {
        // The cycle will briefly drop our HTTP — the request may error;
        // that's expected. Don't treat a network drop as failure.
        status.textContent = "Sent (HTTP dropped during reconnect — expected).";
        status.style.color = "#3a7a3a";
    }
    setTimeout(() => { status.textContent = ""; status.style.color = ""; }, 8000);
}

async function refreshHomeKitMDNS() {
    const status = document.getElementById("reconnectStatus");
    status.textContent = "Sending…";
    try {
        const res = await fetch("refreshHomeKitMDNS", { method: "POST" });
        if (res.ok) {
            status.textContent = "OK — mDNS re-advertised. Check Home app in a few seconds.";
            status.style.color = "#3a7a3a";
        } else {
            status.textContent = `Failed (HTTP ${res.status})`;
            status.style.color = "#a33";
        }
    } catch (e) {
        status.textContent = `Failed (${e.message || "network error"})`;
        status.style.color = "#a33";
    }
    setTimeout(() => { status.textContent = ""; status.style.color = ""; }, 8000);
}

async function dumpHomeKitState() {
    const status = document.getElementById("reconnectStatus");
    status.textContent = "Sending…";
    try {
        const res = await fetch("dumpHomeKitState", { method: "POST" });
        if (res.ok) {
            status.textContent = "OK — state dump in the log below (HomeSpan output is multi-line).";
            status.style.color = "#3a7a3a";
        } else {
            status.textContent = `Failed (HTTP ${res.status})`;
            status.style.color = "#a33";
        }
    } catch (e) {
        status.textContent = `Failed (${e.message || "network error"})`;
        status.style.color = "#a33";
    }
    setTimeout(() => { status.textContent = ""; status.style.color = ""; }, 8000);
}
// Generate a UUID.  Cannot use crypto.randomUUID() because that will only run
// in a secure environment, which is not possible with ratgdo.
function uuidv4() {
    return "10000000-1000-4000-8000-100000000000".replace(/[018]/g, c =>
        (+c ^ crypto.getRandomValues(new Uint8Array(1))[0] & 15 >> +c / 4).toString(16)
    );
}
