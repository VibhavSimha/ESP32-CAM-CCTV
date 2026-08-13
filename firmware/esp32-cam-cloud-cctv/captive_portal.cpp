#include "captive_portal.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

namespace {

static const char* kProbeUrl = "http://connectivitycheck.gstatic.com/generate_204";
static const char* kDefaultUserField = "username";
static const char* kDefaultPassField = "password";

static bool s_captiveDetected = false;
static String s_detectedPortalUrl;
static String s_probeStatus;

static String urlDecode(const String& value) {
    String out;
    out.reserve(value.length());
    for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < value.length()) {
            char hi = value[i + 1];
            char lo = value[i + 2];
            auto hex = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
                if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
                return -1;
            };
            int h = hex(hi);
            int l = hex(lo);
            if (h >= 0 && l >= 0) {
                out += static_cast<char>((h << 4) | l);
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

static bool getFormField(const String& body, const char* key, String& valueOut) {
    int start = 0;
    String needle = String(key) + "=";
    while (start < static_cast<int>(body.length())) {
        int end = body.indexOf('&', start);
        if (end < 0) end = body.length();
        String pair = body.substring(start, end);
        if (pair.startsWith(needle)) {
            valueOut = urlDecode(pair.substring(needle.length()));
            return true;
        }
        start = end + 1;
    }
    return false;
}

static String readBody(httpd_req_t* req) {
    String body;
    body.reserve(req->content_len + 16);

    int remaining = req->content_len;
    while (remaining > 0) {
        char chunk[128];
        int toRead = remaining < static_cast<int>(sizeof(chunk)) ? remaining : static_cast<int>(sizeof(chunk));
        int received = httpd_req_recv(req, chunk, toRead);
        if (received <= 0) {
            break;
        }
        body.concat(chunk, received);
        remaining -= received;
    }
    return body;
}

static String urlEncode(const String& input) {
    String out;
    out.reserve(input.length() * 3);
    auto hexDigit = [](uint8_t v) -> char {
        return v < 10 ? static_cast<char>('0' + v) : static_cast<char>('A' + (v - 10));
    };
    for (size_t i = 0; i < input.length(); ++i) {
        uint8_t c = static_cast<uint8_t>(input[i]);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else if (c == ' ') {
            out += '+';
        } else {
            out += '%';
            out += hexDigit((c >> 4) & 0x0F);
            out += hexDigit(c & 0x0F);
        }
    }
    return out;
}

static bool submitPortalLogin(const String& loginUrl,
                              const String& user,
                              const String& pass,
                              const String& userField,
                              const String& passField,
                              String& result) {
    if (loginUrl.isEmpty()) {
        result = "Portal URL is empty.";
        return false;
    }

    String body = String(userField) + '=' + urlEncode(user) + '&' +
                  String(passField) + '=' + urlEncode(pass);

    HTTPClient http;
    int code = -1;

    if (loginUrl.startsWith("https://")) {
        WiFiClientSecure client;
        client.setInsecure();
        if (!http.begin(client, loginUrl)) {
            result = "Failed to start HTTPS request.";
            return false;
        }
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        http.addHeader("Content-Type", "application/x-www-form-urlencoded");
        code = http.POST(body);
    } else {
        WiFiClient client;
        if (!http.begin(client, loginUrl)) {
            result = "Failed to start HTTP request.";
            return false;
        }
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        http.addHeader("Content-Type", "application/x-www-form-urlencoded");
        code = http.POST(body);
    }

    String response = http.getString();
    String location = http.header("Location");
    http.end();

    if (code >= 200 && code < 400) {
        if (location.length() > 0) {
            s_detectedPortalUrl = location;
        }
        result = "Portal login submitted successfully.";
        return true;
    }

    result = String("Portal login failed: HTTP ") + code;
    if (response.length() > 0) {
        result += "; response received";
    }
    return false;
}

static void probeCaptivePortal() {
    s_captiveDetected = false;
    s_detectedPortalUrl = "";
    s_probeStatus = "Probing network connectivity...";

    HTTPClient http;
    WiFiClient client;
    if (!http.begin(client, kProbeUrl)) {
        s_probeStatus = "Connectivity probe failed to start.";
        return;
    }
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    int code = http.GET();
    String location = http.header("Location");
    String body = http.getString();
    http.end();

    if (code == 204) {
        s_probeStatus = "No captive portal detected.";
        return;
    }

    s_captiveDetected = true;
    if (location.length() > 0) {
        s_detectedPortalUrl = location;
    } else {
        s_detectedPortalUrl = kProbeUrl;
    }

    s_probeStatus = String("Captive portal likely detected. HTTP ") + code;
    if (body.length() > 0) {
        s_probeStatus += "; login page content received";
    }
}

static String buildPortalPage(const String& message) {
    String html;
    html.reserve(4096);
    html += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<title>ISP Portal Login</title>";
    html += "<style>body{font-family:system-ui,sans-serif;background:#101418;color:#e5e7eb;margin:0;padding:24px;max-width:760px}";
    html += "h1{font-size:24px;margin:0 0 12px}p,li{line-height:1.5} .card{background:#1a202c;border:1px solid #2d3748;border-radius:12px;padding:16px;margin:16px 0}";
    html += "label{display:block;margin:10px 0 4px}input{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #4a5568;background:#0f172a;color:#f8fafc}";
    html += "button{margin-top:14px;padding:10px 16px;border:0;border-radius:8px;background:#2563eb;color:white;font-weight:700;cursor:pointer}small{color:#94a3b8}code{word-break:break-all}";
    html += ".ok{color:#86efac}.warn{color:#fbbf24}.err{color:#fca5a5}</style></head><body>";
    html += "<h1>ISP captive portal helper</h1>";
    html += "<div class='card'><p>" + s_probeStatus + "</p>";
    if (s_detectedPortalUrl.length() > 0) {
        html += "<p>Detected portal URL: <code>" + s_detectedPortalUrl + "</code></p>";
    }
    if (message.length() > 0) {
        html += "<p class='ok'>" + message + "</p>";
    }
    html += "</div>";
    html += "<div class='card'><form method='POST' action='/portal'>";
    html += "<label>Portal URL</label><input name='portal_url' placeholder='http://portal.example/login' value='" + s_detectedPortalUrl + "'>";
    html += "<label>Username</label><input name='username' autocomplete='username'>";
    html += "<label>Password</label><input name='password' type='password' autocomplete='current-password'>";
    html += "<label>Username field name</label><input name='user_field' value='" + String(kDefaultUserField) + "'>";
    html += "<label>Password field name</label><input name='pass_field' value='" + String(kDefaultPassField) + "'>";
    html += "<button type='submit'>Submit portal login</button></form>";
    html += "<p><small>If the ISP portal is a simple HTML form, the ESP32 can submit it automatically. If it relies on JavaScript, tokens, or device binding, use the browser manually.</small></p></div>";
    html += "</body></html>";
    return html;
}

static esp_err_t portalGetHandler(httpd_req_t* req) {
    String html = buildPortalPage("");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    return httpd_resp_send(req, html.c_str(), html.length());
}

static esp_err_t portalPostHandler(httpd_req_t* req) {
    String body = readBody(req);

    String loginUrl;
    String user;
    String pass;
    String userField = kDefaultUserField;
    String passField = kDefaultPassField;

    getFormField(body, "portal_url", loginUrl);
    getFormField(body, "username", user);
    getFormField(body, "password", pass);
    getFormField(body, "user_field", userField);
    getFormField(body, "pass_field", passField);

    if (loginUrl.isEmpty()) {
        loginUrl = s_detectedPortalUrl;
    }

    String result;
    bool ok = submitPortalLogin(loginUrl, user, pass, userField, passField, result);
    if (ok) {
        probeCaptivePortal();
        if (s_captiveDetected) {
            s_probeStatus = "Portal login submitted; connectivity still looks captive.";
        } else {
            s_probeStatus = "Portal login submitted; internet access restored.";
        }
    } else {
        s_probeStatus = result;
    }

    String html = buildPortalPage(result);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    return httpd_resp_send(req, html.c_str(), html.length());
}

} // namespace

void setupCaptivePortal() {
    probeCaptivePortal();
    Serial.printf("[CaptivePortal] %s\n", s_probeStatus.c_str());
    if (s_captiveDetected) {
        Serial.printf("[CaptivePortal] Portal URL: %s\n", s_detectedPortalUrl.c_str());
    }
}

void registerCaptivePortalHandlers(httpd_handle_t server) {
    httpd_uri_t portal_get = { .uri = "/portal", .method = HTTP_GET, .handler = portalGetHandler, .user_ctx = NULL };
    httpd_uri_t portal_post = { .uri = "/portal", .method = HTTP_POST, .handler = portalPostHandler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &portal_get);
    httpd_register_uri_handler(server, &portal_post);
}
