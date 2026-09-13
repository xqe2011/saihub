/**
 * @name HTTP server module
 * @file http_server.h
 * @author xqe2011
 */
#ifndef HTTP_SERVER_H__
#define HTTP_SERVER_H__

#include "lock.h"

#include <cJSON.h>
#include <esp_err.h>
#include <esp_http_server.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

esp_err_t HttpServer_Init(void);
esp_err_t HttpServer_Start(void);
esp_err_t HttpServer_Stop(void);

const char* HttpServer_MethodName(int method);
void HttpServer_LogCall(httpd_req_t* req);
esp_err_t HttpServer_SendError(httpd_req_t* req, int status, const char* reason);
esp_err_t HttpServer_SendJson(httpd_req_t* req, int status, cJSON* root);
esp_err_t HttpServer_SendEmpty(httpd_req_t* req, int status);
bool HttpServer_HasJsonContentType(httpd_req_t* req);
void HttpServer_GetLockHeader(httpd_req_t* req, char* out, size_t outLen);
int HttpServer_LockStatus(httpd_req_t* req, Lock_Kind kind, int pin, uint8_t methods, char* lockIdBuf, size_t lockIdLen,
                    char* reasonOut, size_t reasonLen);
int HttpServer_ParsePathPin(const char* uri, const char* prefix, const char* suffix, int* pinOut);
esp_err_t HttpServer_ParsePinsArray(cJSON* root, int* pins, size_t maxPins, size_t* countOut, char* reason, size_t reasonLen);
void HttpServer_FormatPinRange(char* out, size_t outLen);
int64_t HttpServer_NowUs(void);
esp_err_t HttpServer_ReadBody(httpd_req_t* req, char** outBuf, size_t* outLen);
cJSON* HttpServer_ParseBody(httpd_req_t* req, esp_err_t* errOut);

#endif
