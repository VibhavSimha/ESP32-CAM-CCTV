#include "captive_portal.h"
#include "captive_portal_parse.h"
#include "config.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>

#include <string>

// -----------------------------------------------------------------------------
// Config defaults (override in config.h). The feature is opt-out: it stays
// completely silent on open networks and only surfaces UI when a captive portal
// is actually detected.
// -----------------------------------------------------------------------------
#ifndef ENABLE_CAPTIVE_PORTAL_LOGIN
#define ENABLE_CAPTIVE_PORTAL_LOGIN 1
#endif

#ifndef CAPTIVE_PROBE_URL
// A plain-HTTP 204 endpoint. A clean network returns "204 No Content" with an
// empty body; a captive portal intercepts it with a redirect or login page.
#define CAPTIVE_PROBE_URL "http://connectivitycheck.gstatic.com/generate_204"
#endif

#ifndef CAPTIVE_PROBE_TIMEOUT_MS
#define CAPTIVE_PROBE_TIMEOUT_MS 6000
#endif

#ifndef CAPTIVE_MAX_LOGIN_ATTEMPTS
#define CAPTIVE_MAX_LOGIN_ATTEMPTS 3
#endif

// Cap how much of a portal page we read into RAM (heap is precious here).
// Some modern captive portals ship heavy WordPress pages where the login form /
// JS config appears beyond the first 12 KB, so keep this overrideable.
#ifndef CAPTIVE_MAX_PAGE_BYTES
#define CAPTIVE_MAX_PAGE_BYTES 32768
#endif

// Some captive portals (notably MikroTik hotspots) do not serve the login form
// directly: the first page is an rlogin-style LANDING page that only redirects
// to the real login page via a <meta refresh>, a JavaScript location change or a
// "continue" link. The ESP32 HTTP client does not run JavaScript, so we follow
// up to this many such hops ourselves to reach the real login form (issue #44).
#ifndef CAPTIVE_MAX_REDIRECT_HOPS
#define CAPTIVE_MAX_REDIRECT_HOPS 3
#endif

// Minimum free heap (bytes) required before opening a TLS connection to fetch or
// submit an HTTPS captive-portal page. A mbedTLS handshake needs ~40 KB of heap
// transiently; attempting it when the heap is already tight (camera + tunnel
// running) can brown the board out, so below this watermark we skip the HTTPS hop
// and stay on the manual-browser fallback instead of crashing (issue #48). At
// boot — when the portal is first probed — the heap is ~140 KB, well above this.
#ifndef CAPTIVE_MIN_HEAP_FOR_TLS
#define CAPTIVE_MIN_HEAP_FOR_TLS 60000UL
#endif

// Dump the full fetched captive-portal login page to the serial console when a
// portal is detected. This is a diagnostic aid: many ISP portals (e.g. MikroTik
// CHAP) cannot be auto-detected, and seeing the exact HTML the ESP32 received is
// the fastest way to work out which fields to parse/submit. Set to 0 in config.h
// to silence it. It only ever prints when a captive portal is actually detected
// (rare), so it does not add noise on open networks.
#ifndef CAPTIVE_LOG_PORTAL_PAGE
#define CAPTIVE_LOG_PORTAL_PAGE 1
#endif

// -----------------------------------------------------------------------------
// Module state
// -----------------------------------------------------------------------------
static PortalState s_state = PORTAL_STATE_UNKNOWN;
static PortalForm  s_form;             // auto-detected login form
static String      s_portalUrl;        // URL of the portal login page
// URL of the page the current s_form was actually parsed from. This can differ
// from s_portalUrl when the real login form is reached by following one or more
// landing-page redirects: for a POST hop s_portalUrl is deliberately kept at the
// browser-facing URL, so the form action must be resolved against THIS page URL
// instead, or a relative/empty action would post to the wrong host (issue #48).
static String      s_formPageUrl;
// Best-effort portal session cookie (typically PHPSESSID) captured from portal
// responses and replayed on follow-up fetches + credential submit when present.
static String      s_portalCookie;
static String      s_lastMessage;      // human-readable status for the UI
static int         s_attempts = 0;
static bool        s_reprobePending = false;
static unsigned long s_reprobeAt = 0;
// Set by the /portal/reprobe HTTP handler ("Check again" button) so the actual
// blocking probe runs in the main-loop task instead of the httpd task. Doing the
// probe inline in the handler froze the whole web server for ~13 s while the
// socket timed out, so /portal itself became unresponsive (issue #46, confirmed
// by the reporter's HAR: GET /portal took 13,380 ms). We ACK immediately and let
// captivePortalLoop() do the work; the page's existing poll surfaces the result.
static bool        s_manualReprobePending = false;
// Set by the /portal/login handler when the operator submits credentials but no
// automatable login form has been detected yet (e.g. the boot-time HTTPS redirect
// fetch failed). The blocking re-detect + submit is deferred to captivePortalLoop()
// so the httpd task stays responsive (issue #46). The credentials are held only
// until that deferred submit runs, then wiped (issue #48).
static bool        s_redetectLoginPending = false;
static String      s_pendingUser;
static String      s_pendingPass;
// Periodic re-probe interval when in UNSUPPORTED/FAILED state so that we
// automatically detect when the user manually logs in via their browser.
#ifndef CAPTIVE_PERIODIC_REPROBE_MS
#define CAPTIVE_PERIODIC_REPROBE_MS 30000UL
#endif
static unsigned long s_periodicReprobeAt = 0;
// Heartbeat interval while the internet IS confirmed reachable. We keep probing
// (more gently than the offline re-probe) so a captive portal that re-appears
// mid-operation — e.g. a time-limited ISP session that expires — is detected and
// cloud uploads are paused again instead of silently failing (issue #40).
#ifndef CAPTIVE_ONLINE_HEARTBEAT_MS
#define CAPTIVE_ONLINE_HEARTBEAT_MS 60000UL
#endif
static unsigned long s_onlineHeartbeatAt = 0;
// Tracks the previous connectivity state so captivePortalLoop() can reset the
// heartbeat timers exactly once on an offline<->online transition, rather than
// on every loop tick (issue #40).
static bool s_prevOnline = false;

PortalState captivePortalGetState() { return s_state; }

bool captivePortalIsOnline() {
#if !ENABLE_CAPTIVE_PORTAL_LOGIN
    return true;
#else
    // Only OPEN (clean network) and SUCCESS (portal cleared) mean the internet
    // has been positively confirmed reachable by the connectivity heartbeat.
    return s_state == PORTAL_STATE_OPEN || s_state == PORTAL_STATE_SUCCESS;
#endif
}

static void setStatus(PortalState st, const String& msg) {
    s_state = st;
    s_lastMessage = msg;
}

// -----------------------------------------------------------------------------
// URL helpers
// -----------------------------------------------------------------------------

// Resolve a (possibly relative) form action against the portal page URL.
static String resolveActionUrl(const String& base, const String& action) {
    if (action.length() == 0) return base; // post back to the page itself
    if (action.startsWith("http://") || action.startsWith("https://")) {
        return action;
    }
    // Derive scheme://host[:port] from the base URL.
    int schemeEnd = base.indexOf("://");
    if (schemeEnd < 0) return action;
    int hostStart = schemeEnd + 3;
    int pathStart = base.indexOf('/', hostStart);
    String origin = (pathStart < 0) ? base : base.substring(0, pathStart);
    if (action.startsWith("/")) {
        return origin + action;
    }
    // Relative to the page's directory.
    String dir = (pathStart < 0) ? origin + "/" : base.substring(0, base.lastIndexOf('/') + 1);
    return dir + action;
}

// scheme://host[:port]
static String urlOrigin(const String& url) {
    int schemeEnd = url.indexOf("://");
    if (schemeEnd < 0) return "";
    int hostStart = schemeEnd + 3;
    int pathStart = url.indexOf('/', hostStart);
    return (pathStart < 0) ? url : url.substring(0, pathStart);
}

static void rememberSetCookie(const String& setCookie) {
    if (setCookie.length() == 0) return;
    int semi = setCookie.indexOf(';');
    String pair = (semi < 0) ? setCookie : setCookie.substring(0, semi);
    pair.trim();
    if (pair.length() == 0 || pair.indexOf('=') <= 0) return;
    s_portalCookie = pair;
    Serial.printf("[CaptivePortal] Captured portal session cookie: %s\n", s_portalCookie.c_str());
}

static String urlDecode(const String& in) {
    String out;
    out.reserve(in.length());
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < in.length()) {
            auto hex = [](char h) -> int {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                return -1;
            };
            int hi = hex(in[i + 1]);
            int lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += (char)((hi << 4) | lo);
                i += 2;
            } else {
                out += c;
            }
        } else {
            out += c;
        }
    }
    return out;
}

// -----------------------------------------------------------------------------
// Persistence — Wi-Fi is already stored by WiFiManager; we only remember that a
// captive network was handled so we can annotate status. Portal credentials are
// NEVER persisted (issue #33 decision).
// -----------------------------------------------------------------------------
static void persistSeen(const String& ssid) {
    Preferences p;
    if (p.begin("portal", false)) {
        p.putString("ssid", ssid);
        p.putBool("done", true);
        p.end();
    }
}

// -----------------------------------------------------------------------------
// Operator guidance
// -----------------------------------------------------------------------------

// Best-effort URL a human can open to reach the ISP login page when we could not
// positively determine it (e.g. the connectivity probe was unreachable). Captive
// portals almost always intercept plain-HTTP traffic to the default gateway, so
// the gateway root is the safest thing to hand the operator. This guarantees the
// /portal helper's "Open the portal page" link never falls back to the device's
// own page (which rendered as http://<device-ip>/portal# and went nowhere —
// issue #46).
static String bestEffortPortalUrl() {
    IPAddress gw = WiFi.gatewayIP();
    if ((uint32_t)gw != 0) {
        return String("http://") + gw.toString() + "/";
    }
    return String(CAPTIVE_PROBE_URL);
}

// Print a clear, step-by-step banner telling the operator EXACTLY which URL to
// open (on any phone/laptop joined to the same Wi-Fi) to clear the captive
// portal. This replaces the previous terse one-line "manual browser fallback"
// note so the required action is unmistakable in the serial log, instead of the
// log filling up with repeated upload failures (issue #40). It also spells out
// the per-MAC "already logged in" gotcha so operators do not waste time logging
// in on the wrong device (issue #44).
static void printPortalInstructions() {
    String ip  = WiFi.localIP().toString();
    String mac = WiFi.macAddress();
    Serial.println();
    Serial.println("========= ACTION REQUIRED: Wi-Fi captive-portal login =========");
    Serial.println("[CaptivePortal] This network needs a one-time login before the camera");
    Serial.println("[CaptivePortal] can reach the internet. Cloud uploads and the remote");
    Serial.println("[CaptivePortal] tunnel are PAUSED until you complete it.");
    Serial.println("[CaptivePortal]");
    Serial.println("[CaptivePortal]   STEP 1  On a phone or laptop, join the SAME Wi-Fi network the");
    Serial.println("[CaptivePortal]           camera is on (the SSID shown as connected above).");
    Serial.println("[CaptivePortal]   STEP 2  In that device's browser, open the camera's helper page:");
    Serial.printf ("[CaptivePortal]               http://%s/portal\n", ip.c_str());
    if (s_state == PORTAL_STATE_CAPTIVE) {
        Serial.println("[CaptivePortal]   STEP 3  Type your ISP hotspot username & password into that");
        Serial.println("[CaptivePortal]           page and press Log in. The CAMERA then submits the");
        Serial.println("[CaptivePortal]           login itself (hashing the password locally if the");
        Serial.println("[CaptivePortal]           portal uses CHAP), so the camera's OWN connection is");
        Serial.println("[CaptivePortal]           authorised — not your phone's.");
    } else {
        Serial.println("[CaptivePortal]   STEP 3  This portal's login could not be automated. Tap the");
        Serial.println("[CaptivePortal]           portal link on that page, finish the login in your");
        Serial.println("[CaptivePortal]           browser, then tap 'Check again'.");
        if (s_portalUrl.length()) {
            Serial.printf("[CaptivePortal]           (ISP portal page: %s )\n", s_portalUrl.c_str());
        }
    }
    Serial.println("[CaptivePortal]   STEP 4  Leave the camera powered on. It re-checks connectivity");
    Serial.println("[CaptivePortal]           every 30s and resumes uploads automatically once online.");
    Serial.println("[CaptivePortal]");
    Serial.println("[CaptivePortal]   IMPORTANT — same-ISP / 'already logged in' gotcha:");
    Serial.println("[CaptivePortal]   Hotspots authorise each DEVICE separately (per MAC address).");
    Serial.println("[CaptivePortal]   Logging in from your phone/PC only grants THAT device internet");
    Serial.println("[CaptivePortal]   (it may even show \"you are already logged in\") and does NOT");
    Serial.println("[CaptivePortal]   help the camera, which has a different MAC. Always enter the");
    Serial.println("[CaptivePortal]   credentials on the camera's /portal page so the CAMERA logs");
    Serial.println("[CaptivePortal]   ITSELF in.");
    if (s_state != PORTAL_STATE_CAPTIVE) {
        Serial.println("[CaptivePortal]   If you must log in manually and the camera stays blocked,");
        Serial.println("[CaptivePortal]   ask your ISP to authorise the camera's MAC address:");
        Serial.printf ("[CaptivePortal]               %s\n", mac.c_str());
    }
    Serial.println("===============================================================");
    Serial.println();
}

// -----------------------------------------------------------------------------
// HTTP probe + fetch
// -----------------------------------------------------------------------------

// True when `url` uses the HTTPS scheme and therefore needs a TLS client. Many
// ISP portals redirect the plain-HTTP landing page to an https:// login page
// (e.g. Spectra/RADIUSdesk external portals — issue #48). A plain WiFiClient
// cannot speak TLS, so such a fetch fails instantly with HTTPC_ERROR_CONNECTION_
// LOST (-5), dead-ending the whole login flow. We transparently upgrade to a
// WiFiClientSecure for these URLs so the redirect chain can be followed.
static bool isHttpsUrl(const String& url) {
    return url.startsWith("https://") || url.startsWith("HTTPS://");
}

// Map a negative HTTPClient (ESP32) error code to a readable name so the serial
// log shows e.g. "CONNECTION_LOST (-5)" instead of a bare "-5" the operator has
// to look up (issue #48 — "add more log surface"). Positive values are real HTTP
// status codes and are printed as-is by the caller.
static const char* httpErrorName(int code) {
    switch (code) {
        case HTTPC_ERROR_CONNECTION_REFUSED:  return "CONNECTION_REFUSED";
        case HTTPC_ERROR_SEND_HEADER_FAILED:  return "SEND_HEADER_FAILED";
        case HTTPC_ERROR_SEND_PAYLOAD_FAILED: return "SEND_PAYLOAD_FAILED";
        case HTTPC_ERROR_NOT_CONNECTED:       return "NOT_CONNECTED";
        case HTTPC_ERROR_CONNECTION_LOST:     return "CONNECTION_LOST";
        case HTTPC_ERROR_NO_STREAM:           return "NO_STREAM";
        case HTTPC_ERROR_NO_HTTP_SERVER:      return "NO_HTTP_SERVER";
        case HTTPC_ERROR_TOO_LESS_RAM:        return "TOO_LESS_RAM";
        case HTTPC_ERROR_ENCODING:            return "ENCODING";
        case HTTPC_ERROR_STREAM_WRITE:        return "STREAM_WRITE";
        case HTTPC_ERROR_READ_TIMEOUT:        return "READ_TIMEOUT";
        case -1000:                           return "BEGIN_FAILED";
        case -1001:                           return "HEAP_TOO_LOW_FOR_TLS";
        default:                              return code < 0 ? "UNKNOWN_ERROR" : "HTTP";
    }
}

// Pick the transport for `url`: a TLS client (certificate check disabled — a
// captive portal presents an untrusted/again-self-signed cert and the board has
// no RTC time to validate one anyway) for https://, else a plain client. Both
// clients are declared by the caller so they outlive the HTTPClient that borrows
// one of them (see probeInternet for why the ordering matters).
static WiFiClient& selectPortalClient(const String& url, WiFiClient& plain,
                                      WiFiClientSecure& secure) {
    if (isHttpsUrl(url)) {
        secure.setInsecure();
        return secure;
    }
    return plain;
}

// Uniform "fetch result" log line: adds the URL scheme and, for a negative code,
// the decoded HTTPClient error name so failures are self-explanatory (issue #48).
static void logFetchResult(const char* what, const String& url, int code, size_t bytes) {
    if (code < 0) {
        Serial.printf("[CaptivePortal] %s [%s] -> HTTP %d (%s), %u bytes\n",
                      what, isHttpsUrl(url) ? "https" : "http", code,
                      httpErrorName(code), (unsigned)bytes);
    } else {
        Serial.printf("[CaptivePortal] %s [%s] -> HTTP %d, %u bytes\n",
                      what, isHttpsUrl(url) ? "https" : "http", code, (unsigned)bytes);
    }
}

// Perform the connectivity probe. Returns the HTTP status code (or a negative
// HTTPClient error), fills `body` and, on a redirect, `location`.
static int probeInternet(String& body, String& location) {
    body = "";
    location = "";
    // Declare the WiFiClient BEFORE the HTTPClient. Locals are destroyed in
    // reverse order, so this guarantees the HTTPClient (which holds a pointer to
    // the client via http.begin()) is torn down first, while the client is still
    // alive. The reverse order is a use-after-free that corrupts lwIP's pbuf
    // refcounts and later trips "assert failed: pbuf_free: p->ref > 0".
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(CAPTIVE_PROBE_TIMEOUT_MS);
    http.setTimeout(CAPTIVE_PROBE_TIMEOUT_MS);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if (!http.begin(client, CAPTIVE_PROBE_URL)) {
        return -1000;
    }
    const char* headerKeys[] = {"Location"};
    http.collectHeaders(headerKeys, 1);
    int code = http.GET();
    if (code > 0) {
        location = http.header("Location");
        // Only read the body for non-204 responses (portal pages).
        if (code != 204) {
            body = http.getString();
            if (body.length() > CAPTIVE_MAX_PAGE_BYTES) {
                body = body.substring(0, CAPTIVE_MAX_PAGE_BYTES);
            }
        }
    }
    http.end();
    return code;
}

// GET the portal login page at `url`, following redirects. Returns HTTP status
// and fills `html`. Transparently uses TLS for https:// targets (issue #48).
static int fetchPortalPage(const String& url, String& html) {
    html = "";
    if (isHttpsUrl(url) && ESP.getFreeHeap() < CAPTIVE_MIN_HEAP_FOR_TLS) {
        Serial.printf("[CaptivePortal] Skipping HTTPS GET of %s — free heap %u < %lu needed for TLS.\n",
                      url.c_str(), (unsigned)ESP.getFreeHeap(), (unsigned long)CAPTIVE_MIN_HEAP_FOR_TLS);
        return -1001;
    }
    // The transport client(s) must outlive the HTTPClient (see probeInternet).
    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    WiFiClient& client = selectPortalClient(url, plainClient, secureClient);
    HTTPClient http;
    http.setConnectTimeout(CAPTIVE_PROBE_TIMEOUT_MS);
    http.setTimeout(CAPTIVE_PROBE_TIMEOUT_MS);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(client, url)) {
        return -1000;
    }
    const char* headerKeys[] = {"Set-Cookie"};
    http.collectHeaders(headerKeys, 1);
    if (s_portalCookie.length()) {
        http.addHeader("Cookie", s_portalCookie);
    }
    int code = http.GET();
    if (code > 0) {
        rememberSetCookie(http.header("Set-Cookie"));
        html = http.getString();
        if (html.length() > CAPTIVE_MAX_PAGE_BYTES) {
            html = html.substring(0, CAPTIVE_MAX_PAGE_BYTES);
        }
    }
    http.end();
    return code;
}

// POST a MikroTik-style "redirect"/"continue" landing form (application/x-www-
// form-urlencoded `body`) and return the page the portal responds with. Some
// hotspots (notably RADIUSdesk external portals) serve an rlogin landing page
// whose ONLY route to the real login form is a <form name="redirect" method=
// "post"> that the browser auto-submits — a GET to its action does not reproduce
// what the browser does, so we replay it as a POST here (issue #46). The action
// is frequently an https:// external portal, so TLS is used transparently when
// needed (issue #48). Redirects are FORCE-followed so the 302 the portal issues
// after the POST is chased to the real login page (which is then re-parsed for an
// automatable form).
static int fetchPortalPagePost(const String& url, const String& body, String& html) {
    html = "";
    if (isHttpsUrl(url) && ESP.getFreeHeap() < CAPTIVE_MIN_HEAP_FOR_TLS) {
        Serial.printf("[CaptivePortal] Skipping HTTPS POST to %s — free heap %u < %lu needed for TLS.\n",
                      url.c_str(), (unsigned)ESP.getFreeHeap(), (unsigned long)CAPTIVE_MIN_HEAP_FOR_TLS);
        return -1001;
    }
    // The transport client(s) must outlive the HTTPClient (see probeInternet).
    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    WiFiClient& client = selectPortalClient(url, plainClient, secureClient);
    HTTPClient http;
    http.setConnectTimeout(CAPTIVE_PROBE_TIMEOUT_MS);
    http.setTimeout(CAPTIVE_PROBE_TIMEOUT_MS);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    if (!http.begin(client, url)) {
        return -1000;
    }
    const char* headerKeys[] = {"Set-Cookie"};
    http.collectHeaders(headerKeys, 1);
    if (s_portalCookie.length()) {
        http.addHeader("Cookie", s_portalCookie);
    }
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    int code = http.POST(body);
    if (code > 0) {
        rememberSetCookie(http.header("Set-Cookie"));
        html = http.getString();
        if (html.length() > CAPTIVE_MAX_PAGE_BYTES) {
            html = html.substring(0, CAPTIVE_MAX_PAGE_BYTES);
        }
    }
    http.end();
    return code;
}

// Dump the exact portal HTML we are about to parse to the serial console, in
// small chunks so a large page cannot overflow the UART TX buffer. This is what
// lets us see the real login form (field names, hidden CHAP tokens, JS) and
// decide precisely what to parse/submit instead of guessing (see the
// CAPTIVE_LOG_PORTAL_PAGE knob). Byte-accurate: we write the raw bytes, not an
// escaped/normalised copy, so what you see is exactly what the ESP32 received.
static void logPortalPage(const String& html) {
#if CAPTIVE_LOG_PORTAL_PAGE
    const char* p = html.c_str();
    const size_t n = html.length();
    Serial.printf("[CaptivePortal] ===== BEGIN portal page (%u bytes) =====\n", (unsigned)n);
    const size_t chunk = 256;
    for (size_t i = 0; i < n; i += chunk) {
        size_t len = (n - i < chunk) ? (n - i) : chunk;
        Serial.write((const uint8_t*)(p + i), len);
        Serial.flush();
    }
    if (n == 0 || p[n - 1] != '\n') Serial.println();
    Serial.println("[CaptivePortal] ===== END portal page =====");
#else
    (void)html;
#endif
}

// Parse the portal page and populate s_form / s_portalUrl / state.
static void analyzePortalPage(const String& html) {
    std::string h(html.c_str(), html.length());
    parseLoginForm(h, s_form);

    // Adjacent debug: report exactly what the parser saw so a mis-detection can
    // be diagnosed from the serial log alone (issue #44).
    Serial.printf("[CaptivePortal] Parse result — formFound:%d valid:%d challenge:%d chap:%d redirect:'%s'\n",
                  s_form.formFound ? 1 : 0, s_form.valid ? 1 : 0,
                  s_form.challenge ? 1 : 0, s_form.chapLogin ? 1 : 0,
                  s_form.redirectUrl.c_str());

    if (!s_form.formFound) {
        setStatus(PORTAL_STATE_UNSUPPORTED,
                  "Captive portal detected, but no login form was found (it may "
                  "be JavaScript-driven). Open the portal in your browser to log in.");
        Serial.println("[CaptivePortal] No form found -> manual browser fallback.");
        return;
    }
    if (s_form.challenge) {
        setStatus(PORTAL_STATE_UNSUPPORTED,
                  "This portal uses a challenge/response login (e.g. CHAP) that "
                  "cannot be automated. Open the portal in your browser to log in.");
        Serial.println("[CaptivePortal] Challenge portal -> manual browser fallback.");
        return;
    }
    if (!s_form.valid) {
        setStatus(PORTAL_STATE_UNSUPPORTED,
                  "Captive portal login form could not be auto-detected. Open the "
                  "portal in your browser to log in.");
        Serial.println("[CaptivePortal] Form incomplete -> manual browser fallback.");
        return;
    }

    setStatus(PORTAL_STATE_CAPTIVE,
              s_form.chapLogin
                  ? "MikroTik captive portal detected. Enter your ISP portal username "
                    "and password — the camera hashes the password (CHAP) and logs in."
                  : "Captive portal detected. Enter your ISP portal username and password.");
    if (s_form.chapLogin) {
        Serial.println("[CaptivePortal] MikroTik CHAP portal — the camera will MD5-hash the "
                       "password locally (chap-id + password + chap-challenge) before submitting.");
    }
    Serial.printf("[CaptivePortal] Auto-detected fields — user:'%s' pass:'%s' action:'%s' method:'%s' hidden:%u chap:%s\n",
                  s_form.userField.c_str(), s_form.passField.c_str(),
                  s_form.action.c_str(), s_form.method.c_str(),
                  (unsigned)s_form.hidden.size(), s_form.chapLogin ? "yes" : "no");
}

// A captive portal has been confirmed reachable (the probe was redirected to a
// login page). Locate that page, auto-detect the login form, set the resulting
// state (CAPTIVE when the form can be automated, UNSUPPORTED otherwise) and print
// step-by-step login instructions. Shared by the boot-time probe
// (captivePortalBegin) and the online heartbeat's portal-reappeared path so both
// classify the portal identically instead of guessing (issue #40).
static void handleCaptiveDetected(const String& body, const String& location) {
    s_portalUrl = location.length() ? location : String(CAPTIVE_PROBE_URL);
    s_portalCookie = "";
    Serial.printf("[CaptivePortal] Captive portal detected. Portal URL: %s\n", s_portalUrl.c_str());

    String currentUrl = s_portalUrl;
    String html = body;
    if (location.length()) {
        String page;
        int pcode = fetchPortalPage(currentUrl, page);
        logFetchResult("Fetch portal page", currentUrl, pcode, page.length());
        if (page.length()) html = page;
    }

    // Follow up to CAPTIVE_MAX_REDIRECT_HOPS landing-page redirects to reach the
    // real login form. Many hotspots (MikroTik especially) serve an rlogin-style
    // page first that only points to login.html via a <meta refresh>, a JS
    // location change or a "continue" link. Because the ESP32 does not run
    // JavaScript, we walk those hops ourselves; without this the firmware stalls
    // on the landing page and reports the portal as "unsupported" (issue #44).
    for (int hop = 0; ; hop++) {
        // Dump the exact page we are about to parse so the real login form (field
        // names / hidden CHAP tokens / JS) is visible on the serial console.
        logPortalPage(html);
        analyzePortalPage(html);
        // Remember which page THIS form came from so a relative/empty form action
        // is later resolved against the correct base, even after a POST hop where
        // s_portalUrl is intentionally left at the browser-facing URL (issue #48).
        s_formPageUrl = currentUrl;

        if (s_state == PORTAL_STATE_CAPTIVE) break;   // reached an automatable form
        if (s_form.redirectUrl.empty()) break;        // no next hop to follow
        if (hop >= CAPTIVE_MAX_REDIRECT_HOPS) {
            Serial.printf("[CaptivePortal] Redirect hop limit (%d) reached — staying on manual fallback.\n",
                          CAPTIVE_MAX_REDIRECT_HOPS);
            break;
        }

        String nextUrl = resolveActionUrl(currentUrl, String(s_form.redirectUrl.c_str()));
        if (nextUrl == currentUrl) {
            Serial.println("[CaptivePortal] Redirect target is the current page — stopping to avoid a loop.");
            break;
        }

        // A MikroTik-style "redirect"/"continue" landing form is auto-submitted by
        // the browser; replay it with the SAME method (POST echoing its hidden
        // fields, else a plain GET) so we reach the real login page instead of
        // dead-ending on the landing page (issue #46).
        bool postHop = (s_form.redirectMethod == "post");
        String postBody;
        if (postHop) {
            PortalForm redirectForm;
            redirectForm.hidden = s_form.redirectFields;
            postBody = String(buildFormBody(redirectForm, std::string(), std::string()).c_str());
        }
        Serial.printf("[CaptivePortal] Landing page redirects (no login form here) — following hop %d via %s: %s\n",
                      hop + 1, postHop ? "POST" : "GET", nextUrl.c_str());

        String page;
        int pcode = postHop ? fetchPortalPagePost(nextUrl, postBody, page)
                            : fetchPortalPage(nextUrl, page);
        logFetchResult("Fetch redirect target", nextUrl, pcode, page.length());
        if (pcode <= 0 || page.length() == 0) {
            Serial.println("[CaptivePortal] Could not fetch the redirect target — staying on manual fallback.");
            if (isHttpsUrl(nextUrl) && pcode == HTTPC_ERROR_CONNECTION_LOST) {
                Serial.println("[CaptivePortal] (HTTPS target dropped the connection — the portal's TLS "
                               "may be incompatible with the ESP32; use the /portal form or a browser.)");
            }
            break;
        }
        currentUrl = nextUrl;
        // Point the manual-browser fallback at the real login page for a GET hop
        // (a URL a human can open directly). For a POST hop the target is a
        // browser-detection endpoint that only makes sense as a form submission, so
        // keep the original portal URL — that is what the operator's browser hits.
        if (!postHop) s_portalUrl = nextUrl;
        html = page;
    }

    // A captive portal is confirmed at this point — tell the operator, in clear
    // step-by-step form, exactly which URL to open to clear it.
    printPortalInstructions();
}

// -----------------------------------------------------------------------------
// Public: post-connect probe
// -----------------------------------------------------------------------------

// Log low-level network parameters so an operator can rule out signal / gateway /
// DNS problems as the cause of a stuck probe (issue #44). No secrets are printed
// — only the local network configuration any device on the LAN can already see.
static void logNetworkDiagnostics() {
    Serial.println("[CaptivePortal] Network diagnostics (rule out adjacent causes):");
    Serial.printf ("[CaptivePortal]   SSID:'%s'  RSSI:%d dBm  channel:%d\n",
                   WiFi.SSID().c_str(), (int)WiFi.RSSI(), WiFi.channel());
    Serial.printf ("[CaptivePortal]   IP:%s  gateway:%s  subnet:%s\n",
                   WiFi.localIP().toString().c_str(),
                   WiFi.gatewayIP().toString().c_str(),
                   WiFi.subnetMask().toString().c_str());
    Serial.printf ("[CaptivePortal]   DNS1:%s  DNS2:%s  MAC:%s\n",
                   WiFi.dnsIP(0).toString().c_str(),
                   WiFi.dnsIP(1).toString().c_str(),
                   WiFi.macAddress().c_str());
    // A gateway that is also the DNS server, or a private DNS, is typical of a
    // hotspot that intercepts DNS — a useful hint when the probe misbehaves.
    if (WiFi.dnsIP(0) == WiFi.gatewayIP()) {
        Serial.println("[CaptivePortal]   Note: DNS == gateway (hotspot likely intercepts DNS).");
    }
}

void captivePortalBegin() {
#if !ENABLE_CAPTIVE_PORTAL_LOGIN
    return;
#else
    if (WiFi.status() != WL_CONNECTED) {
        return; // scope is strictly post-connect
    }

    logNetworkDiagnostics();

    String body, location;
    int code = probeInternet(body, location);
    if (code < 0) {
        Serial.printf("[CaptivePortal] Probe %s -> HTTP %d (%s)\n", CAPTIVE_PROBE_URL, code, httpErrorName(code));
    } else {
        Serial.printf("[CaptivePortal] Probe %s -> HTTP %d%s\n", CAPTIVE_PROBE_URL, code,
                      code >= 300 && code < 400 ? " (redirected — captive portal)" : "");
    }

    std::string b(body.c_str(), body.length());
    if (code > 0 && !looksLikeCaptivePortal(code, b)) {
        setStatus(PORTAL_STATE_OPEN, "Internet reachable — no captive portal.");
        Serial.println("[CaptivePortal] Open network — nothing to do.");
        return;
    }
    if (code <= 0) {
        // Could not reach the probe endpoint at all. Treat as recoverable: the
        // camera server still starts so the user stays in control. Cloud uploads
        // stay paused (captivePortalIsOnline() is false) until the heartbeat
        // confirms connectivity.
        setStatus(PORTAL_STATE_FAILED,
                  "Could not reach the internet probe. If this network has a "
                  "login page, open it in your browser.");
        // We do not know the exact ISP portal URL here, but a login page (if any)
        // is almost always at the gateway — hand the operator a working link so
        // the /portal helper never points back at the device itself (issue #46).
        if (s_portalUrl.length() == 0) s_portalUrl = bestEffortPortalUrl();
        Serial.println("[CaptivePortal] Probe failed (network/DNS). Staying recoverable.");
        Serial.printf("[CaptivePortal] Cloud uploads paused. Open http://%s/portal on another "
                      "device to check/retry.\n", WiFi.localIP().toString().c_str());
        return;
    }

    // Captive portal — locate the login page, auto-detect the form, and print
    // step-by-step login instructions (shared with the online heartbeat).
    handleCaptiveDetected(body, location);
#endif
}

// -----------------------------------------------------------------------------
// Portal login submission
// -----------------------------------------------------------------------------
static bool submitPortalLogin(const String& username, const String& password, String& outMsg) {
    if (!s_form.valid) {
        outMsg = "This portal cannot be automated. Please log in from your browser.";
        return false;
    }
    // Resolve the form action against the page the form was actually parsed from
    // (s_formPageUrl), not s_portalUrl — those differ after a POST redirect hop,
    // where s_portalUrl is kept at the browser-facing URL (issue #48). Fall back
    // to s_portalUrl when no distinct form-page URL was recorded.
    String base = s_formPageUrl.length() ? s_formPageUrl : s_portalUrl;
    String actionUrl = resolveActionUrl(base, String(s_form.action.c_str()));
    std::string bodyStd = buildFormBody(s_form,
                                        std::string(username.c_str(), username.length()),
                                        std::string(password.c_str(), password.length()));
    String body(bodyStd.c_str());

    Serial.printf("[CaptivePortal] Submitting login to %s (method %s, %s)\n",
                  actionUrl.c_str(), s_form.method.c_str(),
                  isHttpsUrl(actionUrl) ? "https" : "http");

    if (isHttpsUrl(actionUrl) && ESP.getFreeHeap() < CAPTIVE_MIN_HEAP_FOR_TLS) {
        Serial.printf("[CaptivePortal] Skipping HTTPS login submit — free heap %u < %lu needed for TLS.\n",
                      (unsigned)ESP.getFreeHeap(), (unsigned long)CAPTIVE_MIN_HEAP_FOR_TLS);
        outMsg = "Not enough memory to open a secure connection to the portal right "
                 "now. Please finish the login in your browser instead.";
        return false;
    }

    // The transport client(s) must outlive the HTTPClient (see probeInternet).
    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    WiFiClient& client = selectPortalClient(actionUrl, plainClient, secureClient);
    HTTPClient http;
    http.setConnectTimeout(CAPTIVE_PROBE_TIMEOUT_MS);
    http.setTimeout(CAPTIVE_PROBE_TIMEOUT_MS);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    int code;
    if (s_form.method == "get") {
        String url = actionUrl + (actionUrl.indexOf('?') >= 0 ? "&" : "?") + body;
        if (!http.begin(client, url)) { outMsg = "Could not connect to the portal."; return false; }
        const char* headerKeys[] = {"Set-Cookie"};
        http.collectHeaders(headerKeys, 1);
        if (s_portalCookie.length()) {
            http.addHeader("Cookie", s_portalCookie);
        }
        if (base.length()) {
            http.addHeader("Referer", base);
        }
        code = http.GET();
    } else {
        if (!http.begin(client, actionUrl)) { outMsg = "Could not connect to the portal."; return false; }
        const char* headerKeys[] = {"Set-Cookie"};
        http.collectHeaders(headerKeys, 1);
        if (s_portalCookie.length()) {
            http.addHeader("Cookie", s_portalCookie);
        }
        if (base.length()) {
            http.addHeader("Referer", base);
        }
        String origin = urlOrigin(actionUrl);
        if (origin.length()) {
            http.addHeader("Origin", origin);
        }
        http.addHeader("Content-Type", "application/x-www-form-urlencoded");
        if (actionUrl.indexOf("admin-ajax.php") >= 0) {
            http.addHeader("X-Requested-With", "XMLHttpRequest");
            http.addHeader("Accept", "application/json, text/javascript, */*; q=0.01");
        }
        code = http.POST(body);
    }
    if (code < 0) {
        Serial.printf("[CaptivePortal] Portal login response: HTTP %d (%s)\n", code, httpErrorName(code));
    } else {
        Serial.printf("[CaptivePortal] Portal login response: HTTP %d\n", code);
    }
    if (code > 0) {
        rememberSetCookie(http.header("Set-Cookie"));
    }
    http.end();

    if (code <= 0) {
        outMsg = "The portal did not respond. Please try again.";
        return false;
    }
    return true; // success verified by the re-probe below
}

// -----------------------------------------------------------------------------
// Local helper web endpoints
// -----------------------------------------------------------------------------
static const char* stateName(PortalState st) {
    switch (st) {
        case PORTAL_STATE_OPEN:          return "open";
        case PORTAL_STATE_CAPTIVE:       return "captive";
        case PORTAL_STATE_LOGIN_PENDING: return "login_pending";
        case PORTAL_STATE_SUBMITTED:     return "submitted";
        case PORTAL_STATE_SUCCESS:       return "success";
        case PORTAL_STATE_FAILED:        return "failed";
        case PORTAL_STATE_UNSUPPORTED:   return "unsupported";
        default:                         return "unknown";
    }
}

static void sendStatusJson(httpd_req_t* req) {
    // Small, hand-built JSON so we do not pull ArduinoJson into this module.
    String msg = s_lastMessage;
    msg.replace("\\", "\\\\");
    msg.replace("\"", "\\\"");
    String json = "{\"state\":\"";
    json += stateName(s_state);
    json += "\",\"message\":\"";
    json += msg;
    json += "\",\"automatable\":";
    json += (s_state == PORTAL_STATE_CAPTIVE || s_state == PORTAL_STATE_LOGIN_PENDING) ? "true" : "false";
    json += ",\"portalUrl\":\"";
    String pu = s_portalUrl; pu.replace("\"", "\\\"");
    json += pu;
    json += "\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, json.c_str(), HTTPD_RESP_USE_STRLEN);
}

static esp_err_t portal_status_handler(httpd_req_t* req) {
    sendStatusJson(req);
    return ESP_OK;
}

// Build a human-readable, secrets-free diagnostics dump: everything an operator
// needs to understand why the captive-portal login is (or is not) working, in one
// place. Shared by the /portal/diag web endpoint and the serial banner
// (captivePortalPrintDiagnostics) so both always show identical information
// (issue #48 — "add all possible diagnostic helpers ... print that during
// startup").
static void buildPortalDiagnostics(String& d) {
    d = "";
    d.reserve(1024);
    d += "ESP32-CAM captive-portal diagnostics\n";
    d += "state           : "; d += stateName(s_state); d += "\n";
    d += "online          : "; d += (captivePortalIsOnline() ? "yes" : "no"); d += "\n";
    d += "message         : "; d += s_lastMessage; d += "\n";
    d += "attempts        : "; d += s_attempts; d += "\n";
    d += "portalUrl       : "; d += (s_portalUrl.length() ? s_portalUrl : String("(none)")); d += "\n";
    d += "portalUrlScheme : "; d += (s_portalUrl.length() ? (isHttpsUrl(s_portalUrl) ? "https" : "http") : "(none)"); d += "\n";
    d += "formPageUrl     : "; d += (s_formPageUrl.length() ? s_formPageUrl : String("(none)")); d += "\n";
    d += "formFound       : "; d += (s_form.formFound ? "yes" : "no"); d += "\n";
    d += "formValid       : "; d += (s_form.valid ? "yes" : "no"); d += "\n";
    d += "challenge       : "; d += (s_form.challenge ? "yes" : "no"); d += "\n";
    d += "chapLogin       : "; d += (s_form.chapLogin ? "yes" : "no"); d += "\n";
    d += "userField       : "; d += (s_form.userField.size() ? s_form.userField.c_str() : "(none)"); d += "\n";
    d += "passField       : "; d += (s_form.passField.size() ? s_form.passField.c_str() : "(none)"); d += "\n";
    d += "formAction      : "; d += (s_form.action.size() ? s_form.action.c_str() : "(none)"); d += "\n";
    d += "formMethod      : "; d += (s_form.method.size() ? s_form.method.c_str() : "(none)"); d += "\n";
    d += "hiddenFields    : "; d += (unsigned)s_form.hidden.size(); d += "\n";
    d += "-- network --\n";
    d += "wifiConnected   : "; d += (WiFi.status() == WL_CONNECTED ? "yes" : "no"); d += "\n";
    d += "ssid            : "; d += WiFi.SSID(); d += "\n";
    d += "rssi_dBm        : "; d += WiFi.RSSI(); d += "\n";
    d += "channel         : "; d += WiFi.channel(); d += "\n";
    d += "ip              : "; d += WiFi.localIP().toString(); d += "\n";
    d += "gateway         : "; d += WiFi.gatewayIP().toString(); d += "\n";
    d += "subnet          : "; d += WiFi.subnetMask().toString(); d += "\n";
    d += "dns0            : "; d += WiFi.dnsIP(0).toString(); d += "\n";
    d += "dns1            : "; d += WiFi.dnsIP(1).toString(); d += "\n";
    d += "mac             : "; d += WiFi.macAddress(); d += "\n";
    d += "-- runtime --\n";
    d += "freeHeap        : "; d += (unsigned)ESP.getFreeHeap(); d += "\n";
    d += "minTlsHeap      : "; d += (unsigned long)CAPTIVE_MIN_HEAP_FOR_TLS; d += "\n";
    d += "uptime_ms       : "; d += (unsigned long)millis(); d += "\n";
    d += "-- config (captive portal) --\n";
    d += "probeUrl        : "; d += CAPTIVE_PROBE_URL; d += "\n";
    d += "probeTimeout_ms : "; d += (int)CAPTIVE_PROBE_TIMEOUT_MS; d += "\n";
    d += "maxLoginAttempts: "; d += (int)CAPTIVE_MAX_LOGIN_ATTEMPTS; d += "\n";
    d += "maxRedirectHops : "; d += (int)CAPTIVE_MAX_REDIRECT_HOPS; d += "\n";
    d += "logPortalPage   : "; d += (int)CAPTIVE_LOG_PORTAL_PAGE; d += "\n";
}

static esp_err_t portal_diag_handler(httpd_req_t* req) {
    String d;
    buildPortalDiagnostics(d);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, d.c_str(), HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Public: dump the same diagnostics block to the serial monitor. Called at
// startup (from the .ino, after Wi-Fi connects) so a fresh serial capture always
// contains the full portal/network/heap/config picture even if the earlier boot
// banner scrolled past or was corrupted by the flash->run reset (issue #48).
void captivePortalPrintDiagnostics() {
    String d;
    buildPortalDiagnostics(d);
    Serial.println("[CaptivePortal] ===== BEGIN diagnostics =====");
    Serial.print(d);
    Serial.println("[CaptivePortal] ===== END diagnostics =====");
    Serial.println("[CaptivePortal] Tip: GET http://<device-ip>/portal/diag for this block over HTTP,");
    Serial.println("[CaptivePortal]      and open http://<device-ip>/portal to enter portal credentials.");
}

// Immediate re-probe triggered by the user clicking "Check again" after they
// have manually logged into a challenge/CHAP portal via their browser, and also
// the periodic connectivity heartbeat driven from captivePortalLoop().
static void doReprobe() {
    String body, location;
    int code = probeInternet(body, location);
    if (code < 0) {
        Serial.printf("[CaptivePortal] Heartbeat re-probe %s -> HTTP %d (%s)\n",
                      CAPTIVE_PROBE_URL, code, httpErrorName(code));
    } else {
        Serial.printf("[CaptivePortal] Heartbeat re-probe %s -> HTTP %d\n", CAPTIVE_PROBE_URL, code);
    }
    std::string b(body.c_str(), body.length());
    if (code > 0 && !looksLikeCaptivePortal(code, b)) {
        setStatus(PORTAL_STATE_SUCCESS, "Portal login succeeded — you are online.");
        persistSeen(WiFi.SSID());
        Serial.println("[CaptivePortal] Internet reachable — portal cleared. Cloud uploads resume.");
    } else if (code <= 0) {
        setStatus(s_state, "Could not reach the internet. If you have logged in via your browser, try again in a moment.");
    } else {
        setStatus(s_state, "Still behind the captive portal. Please complete the login in your browser, then tap Check again.");
    }
    // Schedule the next periodic re-probe from now.
    s_periodicReprobeAt = millis() + CAPTIVE_PERIODIC_REPROBE_MS;
}

// Overwrite a String's backing buffer, then release it. `&s[0]` uses the mutable
// `char& String::operator[]` overload, so it is a legitimately writable pointer
// into the String's own buffer (no `const` is cast away, unlike writing through
// c_str()). Writing through a VOLATILE view of it prevents the compiler from
// eliminating the zeroing as a dead store just because the buffer is freed
// immediately afterwards (a plain `for (...) s[i]=0; s="";` can be optimised
// away). Used to scrub transient ISP portal credentials from RAM as soon as they
// have been used (issue #48). They are never persisted.
static void secureZeroString(String& s) {
    const size_t n = s.length();
    if (n) {
        volatile char* p = (volatile char*)&s[0];
        for (size_t i = 0; i < n; i++) p[i] = 0;
    }
    s = "";
}

// Overwrite and release the transiently-held ISP portal credentials. They are
// kept in RAM only between the /portal/login handler queuing a deferred submit
// and captivePortalLoop() running it; they are NEVER persisted (issue #48).
static void wipePendingCredentials() {
    secureZeroString(s_pendingUser);
    secureZeroString(s_pendingPass);
}

// Deferred handler for a /portal/login submit made while no automatable form was
// known yet (s_form.valid == false). Runs in the main-loop task, so the blocking
// probe + TLS handshake never freeze the web server (issue #46). It re-probes and
// re-detects the portal — now able to follow HTTPS redirects to the real login
// page (issue #48) — and, if a usable form emerges, submits the operator's
// credentials to it. Either way the stashed credentials are wiped before return.
static void doRedetectAndSubmit() {
    String body, location;
    int code = probeInternet(body, location);
    if (code < 0) {
        Serial.printf("[CaptivePortal] Manual submit: re-probe %s -> HTTP %d (%s)\n",
                      CAPTIVE_PROBE_URL, code, httpErrorName(code));
    } else {
        Serial.printf("[CaptivePortal] Manual submit: re-probe %s -> HTTP %d\n", CAPTIVE_PROBE_URL, code);
    }
    std::string b(body.c_str(), body.length());

    if (code > 0 && !looksLikeCaptivePortal(code, b)) {
        // Already online — nothing to submit.
        setStatus(PORTAL_STATE_SUCCESS, "You are already online — no portal login was needed.");
        persistSeen(WiFi.SSID());
    } else if (code > 0) {
        // Portal present: re-detect (follows HTTPS redirects) and submit if we now
        // have an automatable form.
        handleCaptiveDetected(body, location);
        if (s_state == PORTAL_STATE_CAPTIVE && s_form.valid) {
            setStatus(PORTAL_STATE_SUBMITTED, "Login form found — submitting your credentials…");
            String msg;
            bool sent = submitPortalLogin(s_pendingUser, s_pendingPass, msg);
            if (sent) {
                s_reprobePending = true;
                s_reprobeAt = millis() + 1500;
                setStatus(PORTAL_STATE_SUBMITTED, "Credentials submitted — verifying connectivity…");
            } else {
                setStatus(PORTAL_STATE_FAILED, msg);
            }
        } else {
            setStatus(PORTAL_STATE_UNSUPPORTED,
                      "Could not find an automatable login form on this portal. Please "
                      "finish the login in your browser, then tap Check again.");
        }
    } else {
        setStatus(PORTAL_STATE_FAILED,
                  "Could not reach the portal to submit your login. Please try again in a moment.");
    }

    wipePendingCredentials();
    s_periodicReprobeAt = millis() + CAPTIVE_PERIODIC_REPROBE_MS;
}

// Online heartbeat: called periodically while connectivity is CONFIRMED. It
// re-verifies the internet is still reachable so that a captive portal which
// re-appears mid-operation (e.g. an ISP session that times out) is detected and
// cloud uploads are paused again — keeping the "always heartbeat before Supabase"
// guarantee true over the whole uptime, not just at boot (issue #40).
static void onlineHeartbeat() {
    String body, location;
    int code = probeInternet(body, location);
    std::string b(body.c_str(), body.length());
    if (code > 0 && !looksLikeCaptivePortal(code, b)) {
        return; // still online — stay quiet to avoid log noise
    }
    // Connectivity lost. Pause cloud uploads (captivePortalIsOnline() flips to
    // false) and steer the operator back to /portal. The offline heartbeat in
    // captivePortalLoop() then re-probes until access is restored, at which point
    // uploads resume automatically.
    Serial.printf("[CaptivePortal] Online heartbeat lost connectivity (HTTP %d) — pausing cloud uploads.\n", code);
    if (code > 0) {
        // A captive portal re-appeared and was reachable (redirected us to a
        // login page). Re-detect it exactly like the boot probe so /portal offers
        // the right flow and the state (CAPTIVE / UNSUPPORTED) — and therefore the
        // printed STEP 3 instructions — match the portal that is actually present.
        handleCaptiveDetected(body, location);
    } else {
        // The probe endpoint was unreachable (network/DNS). We cannot tell whether
        // a captive portal is present, so don't claim one (that would print a stale
        // portal URL). Report the drop concisely and point the operator at /portal,
        // matching captivePortalBegin()'s unreachable path.
        setStatus(PORTAL_STATE_FAILED,
                  "Lost internet connectivity — cloud uploads paused. If a login "
                  "page appears, open it in your browser.");
        // Give the manual fallback a working link (gateway root) rather than a
        // possibly-stale/empty URL (issue #46).
        if (s_portalUrl.length() == 0) s_portalUrl = bestEffortPortalUrl();
        Serial.printf("[CaptivePortal] Cloud uploads paused. Open http://%s/portal on "
                      "another device to check/retry.\n", WiFi.localIP().toString().c_str());
    }
}

static esp_err_t portal_reprobe_handler(httpd_req_t* req) {
    // Do NOT probe inline here: probeInternet() can block for many seconds while
    // a socket times out, and this handler runs in the single httpd task, which
    // would freeze the whole web server (issue #46). Instead, flag the request
    // and let captivePortalLoop() run the probe in the main-loop task, then ACK
    // immediately. The page keeps polling /portal/status for the outcome.
    s_manualReprobePending = true;
    setStatus(s_state, "Re-checking the connection…");
    sendStatusJson(req);
    return ESP_OK;
}

static esp_err_t portal_login_handler(httpd_req_t* req) {
    int total = req->content_len;
    if (total <= 0 || total > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_FAIL;
    }
    String raw;
    raw.reserve(total);
    char buf[256];
    int received = 0;
    while (received < total) {
        int want = (int)sizeof(buf);
        if (want > total - received) want = total - received;
        int r = httpd_req_recv(req, buf, want);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv error");
            return ESP_FAIL;
        }
        raw.concat(buf, r);
        received += r;
    }

    // Parse the two fields from the application/x-www-form-urlencoded body.
    String username, password;
    int start = 0;
    while (start < (int)raw.length()) {
        int amp = raw.indexOf('&', start);
        if (amp < 0) amp = raw.length();
        String pair = raw.substring(start, amp);
        int eq = pair.indexOf('=');
        if (eq >= 0) {
            String k = urlDecode(pair.substring(0, eq));
            String v = urlDecode(pair.substring(eq + 1));
            if (k == "user") username = v;
            else if (k == "pass") password = v;
        }
        start = amp + 1;
    }

    if (username.length() == 0 || password.length() == 0) {
        setStatus(s_state, "Username and password are required.");
        sendStatusJson(req);
        return ESP_OK;
    }

    // If we do NOT yet have an automatable login form (e.g. the boot-time HTTPS
    // redirect fetch failed, or the operator opened /portal on an "unsupported"
    // portal), defer a fresh re-detect + submit to captivePortalLoop() instead of
    // giving up. This is what lets the user "just enter a username/password" and
    // have the camera try, even when the exact fields were not detected at boot
    // (issue #48). The blocking work is deferred so the httpd task is not frozen
    // (issue #46). Credentials are stashed briefly, then wiped after the submit.
    if (!s_form.valid) {
        s_pendingUser = username;
        s_pendingPass = password;
        s_redetectLoginPending = true;
        s_attempts++;
        setStatus(PORTAL_STATE_SUBMITTED,
                  "Scanning the portal for a login form and submitting your credentials…");
        // s_pendingUser/Pass now hold a COPY of the credentials (String assignment
        // copies the buffer) and are wiped after the deferred submit runs
        // (wipePendingCredentials in doRedetectAndSubmit). Scrub the local copies
        // here too so no plaintext lingers in this handler's freed buffers.
        secureZeroString(username);
        secureZeroString(password);
        sendStatusJson(req);
        return ESP_OK;
    }

    s_attempts++;
    setStatus(PORTAL_STATE_SUBMITTED, "Submitting your credentials to the portal…");
    String msg;
    bool sent = submitPortalLogin(username, password, msg);

    // Wipe the credentials from RAM as soon as they are used — we do not keep or
    // persist portal credentials.
    secureZeroString(username);
    secureZeroString(password);

    if (!sent) {
        if (s_attempts >= CAPTIVE_MAX_LOGIN_ATTEMPTS) {
            setStatus(PORTAL_STATE_FAILED, msg + " (max attempts reached — log in from your browser instead).");
        } else {
            setStatus(PORTAL_STATE_FAILED, msg);
        }
        sendStatusJson(req);
        return ESP_OK;
    }

    // Verify by re-probing after a short settle delay (handled in the loop).
    s_reprobePending = true;
    s_reprobeAt = millis() + 1500;
    sendStatusJson(req);
    return ESP_OK;
}

static esp_err_t portal_page_handler(httpd_req_t* req) {
    static const char html[] =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<link rel='icon' href='data:,'><title>ESP32-CAM Portal Login</title>"
        "<style>body{font-family:system-ui,Arial,sans-serif;background:#111;color:#eee;margin:0;padding:1.2rem}"
        ".card{max-width:420px;margin:0 auto;background:#1c1c1c;border:1px solid #333;border-radius:12px;padding:1.2rem}"
        "h1{font-size:1.2rem;margin:.2rem 0 1rem}label{display:block;margin:.6rem 0 .2rem;font-size:.9rem}"
        "input{width:100%;box-sizing:border-box;padding:.6rem;border-radius:8px;border:1px solid #444;background:#000;color:#fff}"
        "button{margin-top:1rem;width:100%;padding:.7rem;border:0;border-radius:8px;background:#2d7;color:#000;font-weight:600;cursor:pointer}"
        "#msg{margin-top:1rem;padding:.6rem;border-radius:8px;background:#222;font-size:.9rem;white-space:pre-wrap}"
        ".hint{font-size:.82rem;color:#bbb;margin:.7rem 0 0}"
        ".foot{font-size:.8rem;color:#999;margin-top:1.2rem;border-top:1px solid #333;padding-top:.7rem}"
        "a{color:#6cf}.hide{display:none}</style></head><body><div class='card'>"
        "<h1>Wi-Fi Portal Login</h1>"
        "<div id='msg'>Checking network…</div>"
        "<form id='f' class='hide'>"
        "<label>Portal username</label><input id='u' autocomplete='username' autocapitalize='none'>"
        "<label>Portal password</label><input id='p' type='password' autocomplete='current-password'>"
        "<button type='submit'>Log in</button></form>"
        "<p id='hint' class='hint hide'>Enter the username and password your ISP hotspot login page asks for. "
        "The <strong>camera</strong> submits them over its own connection, so the camera's own MAC address is the one authorised.</p>"
        "<p id='manual' class='hide'>If the automatic login doesn't work, "
        "<span id='plinkwrap'><a id='plink' href='#' target='_blank' rel='noopener'>open the portal page</a> and finish there, </span>"
        "then tap <strong>Check again</strong> below.</p>"
        "<button id='rprobe' class='hide' type='button'>Check again</button>"
        "<p class='foot'>Trouble? <a id='diag' href='/portal/diag' target='_blank' rel='noopener'>View diagnostics</a>"
        " &middot; watch the Serial Monitor for detailed logs.</p>"
        "</div><script>(function(){"
        "var msg=document.getElementById('msg'),f=document.getElementById('f'),"
        "hint=document.getElementById('hint'),"
        "manual=document.getElementById('manual'),plink=document.getElementById('plink'),"
        "plinkwrap=document.getElementById('plinkwrap'),"
        "rprobe=document.getElementById('rprobe');"
        "function render(s){msg.textContent=s.message||'';"
        "var online=(s.state==='open'||s.state==='success');"
        // Always offer the credential form (and the how-to hint) unless we're
        // already online — so the user can attempt a login even when the exact
        // fields were not auto-detected (issue #48).
        "if(online){f.classList.add('hide');hint.classList.add('hide');rprobe.classList.add('hide');}"
        "else{f.classList.remove('hide');hint.classList.remove('hide');rprobe.classList.remove('hide');}"
        "if(s.state==='unsupported'||s.state==='failed'){"
        "if(s.portalUrl){plink.href=s.portalUrl;plinkwrap.classList.remove('hide');}else{plinkwrap.classList.add('hide');}"
        "manual.classList.remove('hide');}"
        "else{manual.classList.add('hide');}}"
        "function poll(){fetch('/portal/status').then(r=>r.json()).then(render).catch(()=>{});}"
        "rprobe.addEventListener('click',function(){msg.textContent='Re-checking the connection…';"
        "fetch('/portal/reprobe',{method:'POST'}).then(r=>r.json()).then(function(s){render(s);setTimeout(poll,1500);}).catch(()=>{msg.textContent='Network error.';});});"
        "f.addEventListener('submit',function(e){e.preventDefault();"
        "var b='user='+encodeURIComponent(document.getElementById('u').value)+"
        "'&pass='+encodeURIComponent(document.getElementById('p').value);"
        "msg.textContent='Submitting…';"
        "fetch('/portal/login',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})"
        ".then(r=>r.json()).then(function(s){render(s);setTimeout(poll,2000);}).catch(()=>{msg.textContent='Network error.';});});"
        "poll();setInterval(poll,4000);"
        "})();</script></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

void registerCaptivePortalHandlers(httpd_handle_t server) {
#if ENABLE_CAPTIVE_PORTAL_LOGIN
    if (!server) return;
    httpd_uri_t portal_uri  = { .uri = "/portal",        .method = HTTP_GET,  .handler = portal_page_handler,   .user_ctx = NULL };
    httpd_uri_t status_uri  = { .uri = "/portal/status", .method = HTTP_GET,  .handler = portal_status_handler, .user_ctx = NULL };
    httpd_uri_t login_uri   = { .uri = "/portal/login",  .method = HTTP_POST, .handler = portal_login_handler,  .user_ctx = NULL };
    httpd_uri_t reprobe_uri = { .uri = "/portal/reprobe", .method = HTTP_POST, .handler = portal_reprobe_handler, .user_ctx = NULL };
    httpd_uri_t diag_uri    = { .uri = "/portal/diag",   .method = HTTP_GET,  .handler = portal_diag_handler,   .user_ctx = NULL };
    httpd_register_uri_handler(server, &portal_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &login_uri);
    httpd_register_uri_handler(server, &reprobe_uri);
    httpd_register_uri_handler(server, &diag_uri);
    Serial.println("[CaptivePortal] Registered /portal, /portal/status, /portal/login, /portal/reprobe, /portal/diag");
#else
    (void)server;
#endif
}

// -----------------------------------------------------------------------------
// Loop tick: post-submit verification + recovery
// -----------------------------------------------------------------------------
void captivePortalLoop() {
#if ENABLE_CAPTIVE_PORTAL_LOGIN
    // Operator tapped "Check again" on /portal. The HTTP handler deferred the
    // (potentially multi-second, blocking) probe to us so the httpd task — and
    // therefore the whole web server — stayed responsive (issue #46). Run it now
    // in the main-loop task; the page's /portal/status poll surfaces the result.
    if (s_manualReprobePending) {
        s_manualReprobePending = false;
        doReprobe();
        return;
    }

    // Operator submitted credentials on /portal but no automatable form was known
    // yet. Re-detect the portal (now able to follow HTTPS redirects — issue #48)
    // and submit, deferred here so the httpd task stays responsive (issue #46).
    if (s_redetectLoginPending) {
        s_redetectLoginPending = false;
        doRedetectAndSubmit();
        return;
    }

    // Post-submit re-probe (after automated credential submission).
    if (s_reprobePending) {
        if ((long)(millis() - s_reprobeAt) >= 0) {
            s_reprobePending = false;

            String body, location;
            int code = probeInternet(body, location);
            std::string b(body.c_str(), body.length());
            if (code > 0 && !looksLikeCaptivePortal(code, b)) {
                setStatus(PORTAL_STATE_SUCCESS, "Portal login succeeded — you are online.");
                persistSeen(WiFi.SSID());
                Serial.println("[CaptivePortal] Login verified — internet reachable.");
            } else {
                if (s_attempts >= CAPTIVE_MAX_LOGIN_ATTEMPTS) {
                    setStatus(PORTAL_STATE_FAILED,
                              "Login did not unlock the internet. Please log in from your browser.");
                } else {
                    setStatus(PORTAL_STATE_FAILED,
                              "Login did not unlock the internet — check your credentials and try again.");
                }
                Serial.println("[CaptivePortal] Re-probe still captive — login failed.");
            }
            // Schedule first periodic re-probe from now.
            s_periodicReprobeAt = millis() + CAPTIVE_PERIODIC_REPROBE_MS;
        }
        return;
    }

    // Periodic connectivity heartbeat while the internet is NOT confirmed
    // reachable (any captive / failed / login-pending state). This lets the
    // device automatically detect when the user has cleared the portal — via the
    // /portal helper OR a manual browser login on any device — and resume cloud
    // uploads without a reboot. Cloud uploads stay paused via captivePortalIsOnline()
    // until this heartbeat confirms connectivity (issue #40).
    bool online = captivePortalIsOnline();
    if (online != s_prevOnline) {
        // Connectivity flipped: start the newly-active mode's heartbeat fresh so
        // each mode's timer is initialised once per transition, not every tick.
        s_periodicReprobeAt = 0;
        s_onlineHeartbeatAt = 0;
        s_prevOnline = online;
    }
    if (!online && WiFi.status() == WL_CONNECTED) {
        if (s_periodicReprobeAt == 0) {
            s_periodicReprobeAt = millis() + CAPTIVE_PERIODIC_REPROBE_MS;
        }
        if ((long)(millis() - s_periodicReprobeAt) >= 0) {
            Serial.println("[CaptivePortal] Connectivity heartbeat (waiting for portal login)...");
            doReprobe();
            if (!captivePortalIsOnline()) {
                // Still offline — remind the operator, with the exact URL, how to
                // clear the portal from another device.
                Serial.printf("[CaptivePortal] Still offline. Open http://%s/portal on a "
                              "phone/laptop on this Wi-Fi to log in.\n",
                              WiFi.localIP().toString().c_str());
            }
        }
    } else if (online && WiFi.status() == WL_CONNECTED) {
        // Online: keep a gentle heartbeat so a captive portal that re-appears
        // mid-operation is caught and uploads are paused again (issue #40).
        if (s_onlineHeartbeatAt == 0) {
            s_onlineHeartbeatAt = millis() + CAPTIVE_ONLINE_HEARTBEAT_MS;
        }
        if ((long)(millis() - s_onlineHeartbeatAt) >= 0) {
            s_onlineHeartbeatAt = millis() + CAPTIVE_ONLINE_HEARTBEAT_MS;
            onlineHeartbeat();
        }
    }
#endif
}
