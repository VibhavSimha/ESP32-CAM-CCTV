#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "camera_init.h"
#include "http_auth.h"
#include "crypto_auth.h"
#include "config.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// Minimum spacing between browser best-effort uploads (ms). See STORAGE_UPLOAD_MIN_GAP_MS
// in config.h; fall back to a safe default if not defined.
#ifndef STORAGE_UPLOAD_MIN_GAP_MS
#define STORAGE_UPLOAD_MIN_GAP_MS 250
#endif

httpd_handle_t camera_httpd = NULL;

// M4: touched from stream tasks (inc/dec) and read from the main loop's idle
// uploader. Marked volatile so the compiler always re-reads it and a decrement
// on any exit path is observed promptly (a missed dec would wedge idle uploads).
volatile int active_stream_clients = 0;

#define FLASH_GPIO 4

// -----------------------------------------------------------------------------
// Global, NVS-persisted flash (LED) state. This is a GLOBAL setting: it applies
// whether or not a client is connected and survives client disconnect + reboot.
// -----------------------------------------------------------------------------
static bool g_flash_on = false;

static void flashApply(bool on) {
    pinMode(FLASH_GPIO, OUTPUT);
    digitalWrite(FLASH_GPIO, on ? HIGH : LOW);
}

static void flashPersist(bool on) {
    Preferences p;
    p.begin("cctv", false);
    p.putBool("flash_on", on);
    p.end();
}

// Called from setup() so the LED reflects the saved global setting at boot,
// independent of any client.
void setupFlashState() {
    Preferences p;
    p.begin("cctv", false);
    g_flash_on = p.getBool("flash_on", false);
    p.end();
    flashApply(g_flash_on);
    Serial.printf("[/flash] Restored persisted global flash state: %d\n", g_flash_on);
}

static esp_err_t capture_handler(httpd_req_t *req) {
    Serial.printf("[/capture] Request from client. Heap: %u\n", ESP.getFreeHeap());
    if (!cryptoAuthRequire(req)) {
        Serial.println("[/capture] Auth FAILED");
        return ESP_FAIL;
    }

    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;

    fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("[/capture] ERROR: esp_camera_fb_get() returned NULL");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    Serial.printf("[/capture] Frame captured: %u bytes\n", fb->len);

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");

    size_t out_len = fb->len;
    res = httpd_resp_send(req, (const char *)fb->buf, out_len);
    if (res != ESP_OK) {
        Serial.printf("[/capture] ERROR: httpd_resp_send failed: %d\n", res);
    }
    esp_camera_fb_return(fb);
    return res;
}

static esp_err_t stream_handler(httpd_req_t *req) {
    Serial.printf("[/stream] Client connected. Heap: %u\n", ESP.getFreeHeap());
    if (!cryptoAuthRequire(req)) {
        Serial.println("[/stream] Auth FAILED — rejecting");
        return ESP_FAIL;
    }
    Serial.println("[/stream] Auth OK — starting MJPEG stream loop");

    // M4: single owned increment; guaranteed matching decrement on EVERY exit
    // path below (content-type failure + natural loop break) so the idle
    // uploader resumes correctly when this client goes away.
    active_stream_clients++;
    Serial.printf("[/stream] active_stream_clients=%d\n", active_stream_clients);

    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t * _jpg_buf = NULL;
    char part_buf[64];
    uint32_t frame_num = 0;

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) {
        Serial.printf("[/stream] ERROR: Failed to set content type: %d\n", res);
        active_stream_clients--;
        Serial.printf("[/stream] active_stream_clients=%d (early exit)\n", active_stream_clients);
        return res;
    }

    while (true) {
        int64_t fr_start = esp_timer_get_time();

        fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("[/stream] ERROR: esp_camera_fb_get() returned NULL");
            res = ESP_FAIL;
        } else if (fb->format != PIXFORMAT_JPEG) {
            Serial.printf("[/stream] ERROR: unexpected pixel format %d (expected JPEG)\n", fb->format);
            res = ESP_FAIL;
        } else {
            _jpg_buf_len = fb->len;
            _jpg_buf = fb->buf;
        }

        unsigned long frameStart = millis();

        if (res == ESP_OK) {
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
            if (res != ESP_OK) {
                Serial.printf("[/stream] ERROR: send_chunk(header) failed on frame %u: %d\n", frame_num, res);
            }
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
            if (res != ESP_OK) {
                Serial.printf("[/stream] ERROR: send_chunk(jpeg %u bytes) failed on frame %u: %d\n", _jpg_buf_len, frame_num, res);
            }
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
            if (res != ESP_OK) {
                Serial.printf("[/stream] ERROR: send_chunk(boundary) failed on frame %u: %d\n", frame_num, res);
            }
        }

        if (fb) {
            esp_camera_fb_return(fb);
            fb = NULL;
            _jpg_buf = NULL;
        } else if (_jpg_buf) {
            free(_jpg_buf);
            _jpg_buf = NULL;
        }

        if (res != ESP_OK) {
            if (res == HTTPD_SOCK_ERR_TIMEOUT) {
                Serial.printf("[/stream] Fatal: Network timeout (send_wait_timeout). Connection dropped due to extreme lag!\n");
            } else if (res == HTTPD_SOCK_ERR_FAIL) {
                Serial.printf("[/stream] Fatal: Socket closed by client or proxy.\n");
            }
            Serial.printf("[/stream] Stream ended after %u frames. Heap: %u\n", frame_num, ESP.getFreeHeap());
            break;
        }

        unsigned long frameTime = millis() - frameStart;
        if (frame_num % 10 == 0) {
            Serial.printf("[/stream] Frame %u sent (%u bytes). Net TX time: %lums\n", frame_num, _jpg_buf_len, frameTime);
        }

        frame_num++;
        if (frame_num % 20 == 0) {
            Serial.printf("[/stream] Streaming... frame %u, last=%u bytes, heap=%u\n",
                frame_num, _jpg_buf_len, ESP.getFreeHeap());
        }

        int64_t fr_end = esp_timer_get_time();
        int64_t frame_time = (fr_end - fr_start) / 1000;
        if (frame_time < 150) {
            vTaskDelay((150 - frame_time) / portTICK_PERIOD_MS);
        } else {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }

    active_stream_clients--;
    Serial.printf("[/stream] active_stream_clients=%d (stream ended)\n", active_stream_clients);
    return res;
}

static esp_err_t health_handler(httpd_req_t *req) {
    uint32_t heap = ESP.getFreeHeap();
    uint32_t uptime = millis() / 1000;
    Serial.printf("[/health] Responding — heap: %u, uptime: %us\n", heap, uptime);

    StaticJsonDocument<128> doc;
    doc["ok"] = true;
    doc["heap"] = heap;
    doc["uptime"] = uptime;

    char buf[128];
    serializeJson(doc, buf);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

#ifndef STR
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#endif

// /flash — GLOBAL, persisted flash toggle.
//   GET /flash          -> returns current state as JSON {"flash":0|1} (reader)
//   GET /flash?s=1|0    -> sets, persists to NVS, and applies GPIO (setter)
static esp_err_t flash_handler(httpd_req_t *req) {
    if (!cryptoAuthRequire(req)) {
        return ESP_FAIL;
    }

    char param[32];
    if (httpd_req_get_url_query_str(req, param, sizeof(param)) == ESP_OK) {
        char value[8];
        if (httpd_query_key_value(param, "s", value, sizeof(value)) == ESP_OK) {
            bool on = atoi(value) != 0;
            g_flash_on = on;
            flashApply(on);
            flashPersist(on);
            Serial.printf("[/flash] Global flash set to %d (persisted)\n", on);
        }
    }

    // Always return the (possibly just-updated) current global state so the UI
    // can sync its toggle. This makes GET /flash a reader when no ?s is given.
    StaticJsonDocument<32> doc;
    doc["flash"] = g_flash_on ? 1 : 0;
    char out[24];
    serializeJson(doc, out);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t view_handler(httpd_req_t *req) {
    // /view itself is served without a session so the login form can load; all
    // sensitive endpoints (/stream, /capture, /flash) enforce the session that
    // the inline login flow establishes via ECDH-encrypted /login.

    // MJPEG Viewer + inline ECDH login + stateful flash toggle + best-effort
    // per-frame Supabase upload.
    //
    // SECURITY NOTE: window.crypto.subtle is only guaranteed on secure contexts
    // (HTTPS or http://localhost). Over plain http://bore.pub:<port> some
    // browsers disable it. For a fully-encrypted channel use the HTTPS SELFHOST
    // tunnel. See docs/SECURITY.md. This flow protects the LOGIN ONLY; the MJPEG
    // stream is still plaintext over BORE and the pubkey exchange is MITM-able.
    static const char html[] =
        "<!DOCTYPE html><html>"
        "<head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<link rel='icon' href='data:,'>"
        "<title>ESP32-CAM Live</title>"
        "<style>"
        "body{margin:0;background:#0d1117;display:flex;flex-direction:column;"
        "align-items:center;justify-content:center;min-height:100vh;"
        "font-family:-apple-system,sans-serif;color:#c9d1d9}"
        "img{max-width:100%;border:2px solid #30363d;border-radius:8px;"
        "box-shadow:0 8px 24px rgba(0,0,0,0.5)}"
        "#st{margin-top:12px;font-size:14px;font-family:monospace;color:#3fb950}"
        "button{margin-top:16px;padding:10px 20px;color:white;border:none;border-radius:6px;cursor:pointer;font-weight:bold;font-size:14px;transition:0.2s}"
        "#flashBtn.on{background:#d29922}#flashBtn.off{background:#30363d}"
        "#loginBox{background:#161b22;padding:24px;border-radius:8px;border:1px solid #30363d;display:flex;flex-direction:column;gap:10px}"
        "#loginBox input{padding:8px;border-radius:6px;border:1px solid #30363d;background:#0d1117;color:#c9d1d9}"
        "#loginBtn{background:#238636}#app{display:none;flex-direction:column;align-items:center}"
        "</style></head><body>"
        "<div id='loginBox'>"
        "<h3>ESP32-CAM Login</h3>"
        "<input id='u' placeholder='username' autocomplete='username'>"
        "<input id='p' type='password' placeholder='password' autocomplete='current-password'>"
        "<button id='loginBtn'>Login (encrypted)</button>"
        "<div id='lerr' style='color:#f85149;font-size:12px'></div>"
        "</div>"
        "<div id='app'>"
        "<img id='cam' width='640' height='480' alt='Loading stream...'>"
        "<div id='st'>Connecting to stream...</div>"
        "<button id='flashBtn' class='off'>\u26A1 Flash: OFF</button>"
        "</div>"
        "<script src='https://cdn.jsdelivr.net/npm/@supabase/supabase-js@2'></script>"
        "<script>"
        "let SID=null;"
        "const b64=b=>btoa(String.fromCharCode(...new Uint8Array(b)));"
        "const ub64=s=>Uint8Array.from(atob(s),c=>c.charCodeAt(0));"
        "async function doLogin(){"
        "const le=document.getElementById('lerr');le.textContent='';"
        "try{"
        "if(!window.crypto||!crypto.subtle){le.textContent='crypto.subtle unavailable over plain HTTP — use the HTTPS tunnel.';return;}"
        "const pk=await (await fetch('/pubkey')).json();"
        "const devPub=ub64(pk.pubkey);"
        "const devKey=await crypto.subtle.importKey('raw',devPub,{name:'X25519'},false,[]);"
        "const eph=await crypto.subtle.generateKey({name:'X25519'},false,['deriveBits']);"
        "const ephPubRaw=await crypto.subtle.exportKey('raw',eph.publicKey);"
        "const shared=await crypto.subtle.deriveBits({name:'X25519',public:devKey},eph.privateKey,256);"
        "const hk=await crypto.subtle.importKey('raw',shared,'HKDF',false,['deriveKey']);"
        "const enc=new TextEncoder();"
        "const aesKey=await crypto.subtle.deriveKey({name:'HKDF',hash:'SHA-256',salt:new Uint8Array(0),info:enc.encode('esp32cam-ecdh-aes256gcm')},hk,{name:'AES-GCM',length:256},false,['encrypt']);"
        "const iv=crypto.getRandomValues(new Uint8Array(12));"
        "const pt=enc.encode(JSON.stringify({user:document.getElementById('u').value,pass:document.getElementById('p').value}));"
        "const ctFull=new Uint8Array(await crypto.subtle.encrypt({name:'AES-GCM',iv},aesKey,pt));"
        "const tag=ctFull.slice(ctFull.length-16);const ct=ctFull.slice(0,ctFull.length-16);"
        "const r=await fetch('/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({epk:b64(ephPubRaw),iv:b64(iv),ct:b64(ct),tag:b64(tag)})});"
        "if(!r.ok){le.textContent='Login failed ('+r.status+')';return;}"
        "SID=(await r.json()).token;"
        "startApp();"
        "}catch(e){le.textContent='Login error: '+e.message;console.error(e);}"
        "}"
        "function auth(u){return fetch(u,{headers:{'X-Session':SID}});}"
        "function startApp(){"
        "document.getElementById('loginBox').style.display='none';"
        "document.getElementById('app').style.display='flex';"
        "const cam=document.getElementById('cam');"
        // MJPEG <img> can't send headers; the sid cookie set by /login authorizes /stream.
        "cam.src='/stream';"
        "cam.onload=()=>{document.getElementById('st').textContent='MJPEG LIVE \\u25cf';};"
        "cam.onerror=()=>{document.getElementById('st').textContent='Stream error — retrying...';setTimeout(()=>cam.src='/stream?_='+Date.now(),1500);};"
        "initFlash();initUpload();"
        "}"
        "async function initFlash(){"
        "const btn=document.getElementById('flashBtn');"
        "async function sync(state){btn.className=state?'on':'off';btn.textContent=(state?'\\u26A1 Flash: ON':'\\u26A1 Flash: OFF');btn.dataset.s=state?1:0;}"
        // Read current global flash state from firmware on load.
        "try{const j=await (await auth('/flash')).json();sync(!!j.flash);}catch(e){}"
        "btn.onclick=async()=>{const ns=(btn.dataset.s==='1')?0:1;try{const j=await (await auth('/flash?s='+ns)).json();sync(!!j.flash);}catch(e){console.error(e);}};"
        "}"
        "function initUpload(){"
        "const sb=supabase.createClient(" SUPABASE_URL "," SUPABASE_ANON_KEY ");"
        "const bkt=" SUPABASE_BUCKET ";const lim=" STR(STORAGE_FRAME_LIMIT) ";const minGap=" STR(STORAGE_UPLOAD_MIN_GAP_MS) ";"
        "let idx=0;let inflight=false;let lastDone=0;"
        "const cam=document.getElementById('cam');"
        // Best-effort: attempt to upload received frames. If an upload is still
        // in flight, DROP this frame (do not queue) to avoid backlog. Also honor
        // a minimum gap since the last completed upload so we never hammer
        // Supabase on a fast machine/network.
        "async function tick(){"
        "const now=performance.now();"
        "if(cam.complete&&cam.naturalWidth&&!inflight&&(now-lastDone)>=minGap){"
        "inflight=true;"
        "try{"
        "const c=document.createElement('canvas');c.width=cam.naturalWidth;c.height=cam.naturalHeight;"
        "c.getContext('2d').drawImage(cam,0,0);"
        "const blob=await new Promise(r=>c.toBlob(r,'image/jpeg'));"
        "idx=(idx%lim)+1;const n='frame_'+idx+'.jpg';"
        "await sb.storage.from(bkt).upload(n,blob,{upsert:true});"
        "}catch(e){/*best effort*/}"
        "lastDone=performance.now();"
        "inflight=false;"
        "}"
        "requestAnimationFrame(tick);"
        "}"
        "requestAnimationFrame(tick);"
        "}"
        "document.getElementById('loginBtn').onclick=doLogin;"
        "</script>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

void startCameraServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 8;
    config.max_open_sockets = 7;
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 10;
    config.lru_purge_enable = true;

    httpd_uri_t stream_uri  = { .uri = "/stream",  .method = HTTP_GET, .handler = stream_handler,  .user_ctx = NULL };
    httpd_uri_t capture_uri = { .uri = "/capture", .method = HTTP_GET, .handler = capture_handler, .user_ctx = NULL };
    httpd_uri_t view_uri    = { .uri = "/view",    .method = HTTP_GET, .handler = view_handler,    .user_ctx = NULL };
    httpd_uri_t health_uri  = { .uri = "/health",  .method = HTTP_GET, .handler = health_handler,  .user_ctx = NULL };
    httpd_uri_t flash_uri   = { .uri = "/flash",   .method = HTTP_GET, .handler = flash_handler,   .user_ctx = NULL };

    Serial.printf("Starting web server on port: '%d'\n", config.server_port);
    Serial.printf("[/stream] HTTPD timeouts: recv=%ds send=%ds, max_open_sockets=%d, lru_purge=%d\n",
        config.recv_wait_timeout, config.send_wait_timeout, config.max_open_sockets, config.lru_purge_enable);
    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(camera_httpd, &stream_uri);
        httpd_register_uri_handler(camera_httpd, &capture_uri);
        httpd_register_uri_handler(camera_httpd, &view_uri);
        httpd_register_uri_handler(camera_httpd, &health_uri);
        httpd_register_uri_handler(camera_httpd, &flash_uri);
        // Register the unauthenticated /pubkey (GET) and /login (POST) handlers.
        registerCryptoAuthHandlers(camera_httpd);
    }
}
