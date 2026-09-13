#ifndef JSON_UTIL_H__
#define JSON_UTIL_H__

#include <cJSON.h>
#include <esp_http_server.h>
#include <stdint.h>

int64_t Json_NowUs(void);
esp_err_t Json_SendError(httpd_req_t* req, int status, const char* reason);
esp_err_t Json_SendJson(httpd_req_t* req, int status, cJSON* root);
esp_err_t Json_SendEmpty(httpd_req_t* req, int status);
esp_err_t Json_ReadBody(httpd_req_t* req, char** outBuf, size_t* outLen);
cJSON* Json_ParseBody(httpd_req_t* req, esp_err_t* errOut);

#endif
