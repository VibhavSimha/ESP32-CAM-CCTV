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
    if (!httpCheckBasicAuth(req, CONFIG_HTTP_USER, CONFIG_HTTP_PASS)) {
        return httpSendUnauthorized(req);
    }
    
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;

    fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");

    size_t out_len = fb->len;
    res = httpd_resp_send(req, (const char *)fb->buf, out_len);
    
    esp_camera_fb_return(fb);
    return res;
}

static esp_err_t stream_handler(httpd_req_t *req) {
    if (!httpCheckBasicAuth(req, CONFIG_HTTP_USER, CONFIG_HTTP_PASS)) {
        return httpSendUnauthorized(req);
    }

    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t * _jpg_buf = NULL;
    char part_buf[64];

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if(res != ESP_OK){
        return res;
    }

    while(true){
        fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("Camera capture failed");
            res = ESP_FAIL;
        } else {
            if(fb->format != PIXFORMAT_JPEG){
                // In our setup, it should always be JPEG.
                res = ESP_FAIL;
            } else {
                _jpg_buf_len = fb->len;
                _jpg_buf = fb->buf;
            }
        }
        
        if(res == ESP_OK){
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        
        if(fb){
            esp_camera_fb_return(fb);
            fb = NULL;
            _jpg_buf = NULL;
        } else if(_jpg_buf){
            free(_jpg_buf);
            _jpg_buf = NULL;
        }
        if(res != ESP_OK){
            break;
        }
        // Small delay to allow yielding
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    return res;
}

static esp_err_t health_handler(httpd_req_t *req) {
    StaticJsonDocument<128> doc;
    doc["ok"] = true;
    doc["heap"] = ESP.getFreeHeap();
    doc["uptime"] = millis() / 1000;
    
    char buf[128];
    serializeJson(doc, buf);
    
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t view_handler(httpd_req_t *req) {
    if (!httpCheckBasicAuth(req, CONFIG_HTTP_USER, CONFIG_HTTP_PASS)) {
        return httpSendUnauthorized(req);
    }

    // Inline viewer using fetch()+AbortController.
    // new Image() has no timeout — if the BORE tunnel takes too long to relay
    // the /capture response, the load hangs forever with no onerror fired.
    // fetch() lets us abort after a deadline and always resolve the promise.
    static const char html[] =
        "<!DOCTYPE html><html>"
        "<head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ESP32-CAM Live</title>"
        "<style>"
        "body{margin:0;background:#111;display:flex;flex-direction:column;"
        "align-items:center;justify-content:center;min-height:100vh;"
        "font-family:sans-serif;color:#eee}"
        "img{max-width:100%;border:2px solid #333;border-radius:6px}"
        "#st{margin-top:10px;font-size:13px;color:#888}"
        ".ok{color:#4caf50}.er{color:#f44336}"
        "</style></head><body>"
        "<img id='cam' alt='Waiting for first frame...' width='640' height='480'>"
        "<div id='st'>Connecting to camera...</div>"
        "<script>"
        "var img=document.getElementById('cam'),"
        "st=document.getElementById('st'),"
        "n=0,e=0,prev=null;"
        "function go(){"
        "var ac=new AbortController();"
        "var t=setTimeout(function(){ac.abort();},12000);"
        "fetch('/capture?_='+Date.now(),{signal:ac.signal,cache:'no-store'})"
        ".then(function(r){clearTimeout(t);"
        "if(!r.ok)throw new Error(r.status);"
        "return r.blob();})"
        ".then(function(b){"
        "var u=URL.createObjectURL(b);"
        "img.onload=function(){if(prev)URL.revokeObjectURL(prev);prev=u;};"
        "img.src=u;n++;"
        "st.className='ok';"
        "st.textContent='Live \\u25cf frame '+n;"
        "setTimeout(go,750);})"
        ".catch(function(){"
        "clearTimeout(t);e++;"
        "st.className='er';"
        "st.textContent='Retry #'+e+'\\u2026';"
        "setTimeout(go,2000);});}"
        "go();"
        "</script></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

void startCameraServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 5;
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 15;
    config.lru_purge_enable = true; // Auto-close oldest connections when sockets are full
    
    httpd_uri_t stream_uri = {
        .uri       = "/stream",
        .method    = HTTP_GET,
        .handler   = stream_handler,
        .user_ctx  = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
        , .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
#endif
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
