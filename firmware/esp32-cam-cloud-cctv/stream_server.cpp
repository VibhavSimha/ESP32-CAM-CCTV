#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "camera_init.h"
#include "http_auth.h"
#include "config.h"
#include <Arduino.h>
#include <ArduinoJson.h>

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t camera_httpd = NULL;

static esp_err_t capture_handler(httpd_req_t *req) {
    Serial.printf("[/capture] Request from client. Heap: %u\n", ESP.getFreeHeap());
    if (!httpCheckBasicAuth(req, CONFIG_HTTP_USER, CONFIG_HTTP_PASS)) {
        Serial.println("[/capture] Auth FAILED");
        return httpSendUnauthorized(req);
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
    if (!httpCheckBasicAuth(req, CONFIG_HTTP_USER, CONFIG_HTTP_PASS)) {
        Serial.println("[/stream] Auth FAILED — rejecting");
        return httpSendUnauthorized(req);
    }
    Serial.println("[/stream] Auth OK — starting MJPEG stream loop");

    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t * _jpg_buf = NULL;
    char part_buf[64];
    uint32_t frame_num = 0;

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) {
        Serial.printf("[/stream] ERROR: Failed to set content type: %d\n", res);
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
        if (frame_num % 10 == 0) { // Log every 10 frames to avoid completely flooding the console
            Serial.printf("[/stream] Frame %u sent (%u bytes). Net TX time: %lums\n", frame_num, _jpg_buf_len, frameTime);
        }

        frame_num++;
        // Log every 20 frames (~3s at 6.5fps)
        if (frame_num % 20 == 0) {
            Serial.printf("[/stream] Streaming... frame %u, last=%u bytes, heap=%u\n",
                frame_num, _jpg_buf_len, ESP.getFreeHeap());
        }

        // Target ~6.5 FPS (150ms total frame period).
        // Prevents flooding the BORE WAN TCP window and socket buffer overflow.
        int64_t fr_end = esp_timer_get_time();
        int64_t frame_time = (fr_end - fr_start) / 1000;
        if (frame_time < 150) {
            vTaskDelay((150 - frame_time) / portTICK_PERIOD_MS);
        } else {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
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

static esp_err_t view_handler(httpd_req_t *req) {
    if (!httpCheckBasicAuth(req, CONFIG_HTTP_USER, CONFIG_HTTP_PASS)) {
        return httpSendUnauthorized(req);
    }

    // MJPEG Viewer — uses native browser MJPEG support via <img src="/stream">.
    // This keeps ONE persistent BORE proxy connection open carrying all frames.
    // No per-frame HTTP overhead, no socket exhaustion, maximum possible FPS.
    // The Authorization header is embedded directly in the URL as Basic auth
    // so the browser's img element can authenticate /stream automatically.
    static const char html[] =
        "<!DOCTYPE html><html>"
        "<head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        // Suppress /favicon.ico request — browser would open a 2nd BORE proxy connection
        // which blocks the tunnel task and kills the active /stream connection
        "<link rel='icon' href='data:,'>"
        "<title>ESP32-CAM Live</title>"
        "<style>"
        "body{margin:0;background:#0d1117;display:flex;flex-direction:column;"
        "align-items:center;justify-content:center;min-height:100vh;"
        "font-family:-apple-system,sans-serif;color:#c9d1d9}"
        "img{max-width:100%;border:2px solid #30363d;border-radius:8px;"
        "box-shadow:0 8px 24px rgba(0,0,0,0.5)}"
        "#st{margin-top:12px;font-size:14px;font-family:monospace;color:#3fb950}"
        "</style></head><body>"
        "<img id='cam' width='640' height='480' "
        // /stream endpoint — same host, credentials already in browser from /view Basic Auth prompt
        "src='/stream' alt='Loading stream...' "
        "onerror=\"this.style.opacity='0.3';document.getElementById('st').textContent='Stream error — retrying...';setTimeout(function(){document.getElementById('cam').src='/stream?_='+Date.now();},3000);\""
        "onload=\"document.getElementById('st').textContent='MJPEG LIVE \\u25cf';\""
        ">"
        "<div id='st'>Connecting to stream...</div>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

void startCameraServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 5;
    config.max_open_sockets = 7;     // MJPEG uses 1 long-lived connection — keep all 7 sockets available
    config.recv_wait_timeout = 60;
    config.send_wait_timeout = 60;
    config.lru_purge_enable = true;  // Force-close oldest stalled socket when all 7 are full
    
    httpd_uri_t stream_uri = {
        .uri       = "/stream",
        .method    = HTTP_GET,
        .handler   = stream_handler,
        .user_ctx  = NULL
        // NOTE: do NOT set is_websocket here — that would make the ESP-IDF
        // HTTP server reject normal MJPEG GET requests as invalid WS upgrades.
    };

    httpd_uri_t capture_uri = {
        .uri       = "/capture",
        .method    = HTTP_GET,
        .handler   = capture_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t view_uri = {
        .uri       = "/view",
        .method    = HTTP_GET,
        .handler   = view_handler,
        .user_ctx  = NULL
    };
    
    httpd_uri_t health_uri = {
        .uri       = "/health",
        .method    = HTTP_GET,
        .handler   = health_handler,
        .user_ctx  = NULL
    };
    
    Serial.printf("Starting web server on port: '%d'\n", config.server_port);
    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(camera_httpd, &stream_uri);
        httpd_register_uri_handler(camera_httpd, &capture_uri);
        httpd_register_uri_handler(camera_httpd, &view_uri);
        httpd_register_uri_handler(camera_httpd, &health_uri);
    }
}
