#include "http_auth.h"
#include <Arduino.h>
#include <mbedtls/base64.h>

bool httpCheckBasicAuth(httpd_req_t *req, const char *user, const char *pass) {
    size_t buf_len = httpd_req_get_hdr_value_len(req, "Authorization") + 1;
    if (buf_len > 1) {
        char *buf = (char *)malloc(buf_len);
        if (buf) {
            if (httpd_req_get_hdr_value_str(req, "Authorization", buf, buf_len) == ESP_OK) {
                // Check if it's Basic auth
                if (strncmp(buf, "Basic ", 6) == 0) {
                    char *b64_str = buf + 6;
                    
                    // Create expected string "user:pass"
                    String expected = String(user) + ":" + String(pass);
                    
                    // Encode expected to base64
                    unsigned char out[128];
                    size_t out_len = 0;
                    mbedtls_base64_encode(out, sizeof(out) - 1, &out_len, (const unsigned char*)expected.c_str(), expected.length());
                    out[out_len] = '\0';
                    
                    if (strcmp(b64_str, (char*)out) == 0) {
                        free(buf);
                        return true;
                    }
                }
            }
            free(buf);
        }
    }
    return false;
}

esp_err_t httpSendUnauthorized(httpd_req_t *req) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32-CAM\"");
    httpd_resp_send(req, "401 Unauthorized", 16);
    return ESP_OK;
}
