#include "captive_portal.h"
#include "captive_portal_parse.h"
#include "config.h"

#include <Arduino.h>
#include <WiFi.h>
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
#define CAPTIVE_MAX_PAGE_BYTES 12288

// -----------------------------------------------------------------------------
// Module state
// -----------------------------------------------------------------------------
static PortalState s_state = PORTAL_STATE_UNKNOWN;
static PortalForm  s_form;             // auto-detected login form
static String      s_portalUrl;        // URL of the portal login page
static String      s_lastMessage;      // human-readable status for the UI
static int         s_attempts = 0;
static bool        s_reprobePending = false;
static unsigned long s_reprobeAt = 0;
// Periodic re-probe interval when in UNSUPPORTED/FAILED state so that we
// automatically detect when the user manually logs in via their browser.
#ifndef CAPTIVE_PERIODIC_REPROBE_MS
#define CAPTIVE_PERIODIC_REPROBE_MS 30000UL
#endif
static unsigned long s_periodicReprobeAt = 0;

PortalState captivePortalGetState() { return s_state; }

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
// HTTP probe + fetch
// -----------------------------------------------------------------------------

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
// and fills `html`.
static int fetchPortalPage(const String& url, String& html) {
    html = "";
    // WiFiClient must outlive the HTTPClient (see probeInternet for details).
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(CAPTIVE_PROBE_TIMEOUT_MS);
    http.setTimeout(CAPTIVE_PROBE_TIMEOUT_MS);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(client, url)) {
        return -1000;
    }
    int code = http.GET();
    if (code > 0) {
        html = http.getString();
        if (html.length() > CAPTIVE_MAX_PAGE_BYTES) {
            html = html.substring(0, CAPTIVE_MAX_PAGE_BYTES);
        }
    }
    http.end();
    return code;
}

// Parse the portal page and populate s_form / s_portalUrl / state.
static void analyzePortalPage(const String& html) {
    std::string h(html.c_str(), html.length());
    parseLoginForm(h, s_form);

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
              "Captive portal detected. Enter your ISP portal username and password.");
    Serial.printf("[CaptivePortal] Auto-detected fields — user:'%s' pass:'%s' action:'%s' method:'%s' hidden:%u\n",
                  s_form.userField.c_str(), s_form.passField.c_str(),
                  s_form.action.c_str(), s_form.method.c_str(),
                  (unsigned)s_form.hidden.size());
}

// -----------------------------------------------------------------------------
// Public: post-connect probe
// -----------------------------------------------------------------------------
void captivePortalBegin() {
#if !ENABLE_CAPTIVE_PORTAL_LOGIN
    return;
#else
    if (WiFi.status() != WL_CONNECTED) {
        return; // scope is strictly post-connect
    }

    String body, location;
    int code = probeInternet(body, location);
    Serial.printf("[CaptivePortal] Probe %s -> HTTP %d\n", CAPTIVE_PROBE_URL, code);

    std::string b(body.c_str(), body.length());
    if (code > 0 && !looksLikeCaptivePortal(code, b)) {
        setStatus(PORTAL_STATE_OPEN, "Internet reachable — no captive portal.");
        Serial.println("[CaptivePortal] Open network — nothing to do.");
        return;
    }
    if (code <= 0) {
        // Could not reach the probe endpoint at all. Treat as recoverable: the
        // camera server still starts so the user stays in control.
        setStatus(PORTAL_STATE_FAILED,
                  "Could not reach the internet probe. If this network has a "
                  "login page, open it in your browser.");
        Serial.println("[CaptivePortal] Probe failed (network/DNS). Staying recoverable.");
        return;
    }

    // Captive portal — locate the login page (redirect target or probe body).
    s_portalUrl = location.length() ? location : String(CAPTIVE_PROBE_URL);
    Serial.printf("[CaptivePortal] Captive portal detected. Portal URL: %s\n", s_portalUrl.c_str());

    String html = body;
    if (location.length()) {
        String page;
        int pcode = fetchPortalPage(s_portalUrl, page);
        Serial.printf("[CaptivePortal] Fetch portal page -> HTTP %d (%u bytes)\n",
                      pcode, (unsigned)page.length());
        if (page.length()) html = page;
    }
    analyzePortalPage(html);
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
    String actionUrl = resolveActionUrl(s_portalUrl, String(s_form.action.c_str()));
    std::string bodyStd = buildFormBody(s_form,
                                        std::string(username.c_str(), username.length()),
                                        std::string(password.c_str(), password.length()));
    String body(bodyStd.c_str());

    Serial.printf("[CaptivePortal] Submitting login to %s (method %s)\n",
                  actionUrl.c_str(), s_form.method.c_str());

    // WiFiClient must outlive the HTTPClient (see probeInternet for details).
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(CAPTIVE_PROBE_TIMEOUT_MS);
    http.setTimeout(CAPTIVE_PROBE_TIMEOUT_MS);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    int code;
    if (s_form.method == "get") {
        String url = actionUrl + (actionUrl.indexOf('?') >= 0 ? "&" : "?") + body;
        if (!http.begin(client, url)) { outMsg = "Could not connect to the portal."; return false; }
        code = http.GET();
    } else {
        if (!http.begin(client, actionUrl)) { outMsg = "Could not connect to the portal."; return false; }
        http.addHeader("Content-Type", "application/x-www-form-urlencoded");
        code = http.POST(body);
    }
    Serial.printf("[CaptivePortal] Portal login response: HTTP %d\n", code);
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

// Immediate re-probe triggered by the user clicking "Check again" after they
// have manually logged into a challenge/CHAP portal via their browser.
static void doReprobe() {
    String body, location;
    int code = probeInternet(body, location);
    Serial.printf("[CaptivePortal] Re-probe %s -> HTTP %d\n", CAPTIVE_PROBE_URL, code);
    std::string b(body.c_str(), body.length());
    if (code > 0 && !looksLikeCaptivePortal(code, b)) {
        setStatus(PORTAL_STATE_SUCCESS, "Portal login succeeded — you are online.");
        persistSeen(WiFi.SSID());
        Serial.println("[CaptivePortal] Re-probe: internet reachable — portal unlocked.");
    } else if (code <= 0) {
        setStatus(s_state, "Could not reach the internet. If you have logged in via your browser, try again in a moment.");
    } else {
        setStatus(s_state, "Still behind the captive portal. Please complete the login in your browser, then tap Check again.");
    }
    // Schedule the next periodic re-probe from now.
    s_periodicReprobeAt = millis() + CAPTIVE_PERIODIC_REPROBE_MS;
}

static esp_err_t portal_reprobe_handler(httpd_req_t* req) {
    doReprobe();
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

    s_attempts++;
    setStatus(PORTAL_STATE_SUBMITTED, "Submitting your credentials to the portal…");
    String msg;
    bool sent = submitPortalLogin(username, password, msg);

    // Wipe the credentials from RAM as soon as they are used — we do not keep or
    // persist portal credentials.
    for (size_t i = 0; i < username.length(); i++) username[i] = 0;
    for (size_t i = 0; i < password.length(); i++) password[i] = 0;
    username = ""; password = "";

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
        "a{color:#6cf}.hide{display:none}</style></head><body><div class='card'>"
        "<h1>Wi-Fi Portal Login</h1>"
        "<div id='msg'>Checking network…</div>"
        "<form id='f' class='hide'>"
        "<label>Portal username</label><input id='u' autocomplete='username' autocapitalize='none'>"
        "<label>Portal password</label><input id='p' type='password' autocomplete='current-password'>"
        "<button type='submit'>Log in</button></form>"
        "<p id='manual' class='hide'>This portal can't be logged in automatically. "
        "<a id='plink' href='#' target='_blank' rel='noopener'>Open the portal page</a> and finish there, "
        "then tap <strong>Check again</strong> below.</p>"
        "<button id='rprobe' class='hide' type='button'>Check again</button>"
        "</div><script>(function(){"
        "var msg=document.getElementById('msg'),f=document.getElementById('f'),"
        "manual=document.getElementById('manual'),plink=document.getElementById('plink'),"
        "rprobe=document.getElementById('rprobe');"
        "function render(s){msg.textContent=s.message||'';"
        "if(s.automatable){f.classList.remove('hide');manual.classList.add('hide');rprobe.classList.add('hide');}"
        "else{f.classList.add('hide');}"
        "if(s.state==='unsupported'||s.state==='failed'){if(s.portalUrl){plink.href=s.portalUrl;}manual.classList.remove('hide');rprobe.classList.remove('hide');}"
        "else{manual.classList.add('hide');rprobe.classList.add('hide');}}"
        "function poll(){fetch('/portal/status').then(r=>r.json()).then(render).catch(()=>{});}"
        "rprobe.addEventListener('click',function(){msg.textContent='Checking…';"
        "fetch('/portal/reprobe',{method:'POST'}).then(r=>r.json()).then(render).catch(()=>{msg.textContent='Network error.';});});"
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
    httpd_register_uri_handler(server, &portal_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &login_uri);
    httpd_register_uri_handler(server, &reprobe_uri);
    Serial.println("[CaptivePortal] Registered /portal, /portal/status, /portal/login, /portal/reprobe");
#else
    (void)server;
#endif
}

// -----------------------------------------------------------------------------
// Loop tick: post-submit verification + recovery
// -----------------------------------------------------------------------------
void captivePortalLoop() {
#if ENABLE_CAPTIVE_PORTAL_LOGIN
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

    // Periodic re-probe when behind an unsupported/failed portal so the device
    // detects when the user has manually logged in via their browser.
    if (s_state == PORTAL_STATE_UNSUPPORTED || s_state == PORTAL_STATE_FAILED) {
        if (s_periodicReprobeAt == 0) {
            s_periodicReprobeAt = millis() + CAPTIVE_PERIODIC_REPROBE_MS;
        }
        if ((long)(millis() - s_periodicReprobeAt) >= 0) {
            Serial.println("[CaptivePortal] Periodic re-probe (waiting for manual browser login).");
            doReprobe();
        }
    }
#endif
}
