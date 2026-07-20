#pragma once

#include "esp_http_server.h"

bool httpCheckBasicAuth(httpd_req_t *req, const char *user, const char *pass);
esp_err_t httpSendUnauthorized(httpd_req_t *req);
