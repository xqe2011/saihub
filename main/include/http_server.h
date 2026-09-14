/**
 * @name HTTP server module
 * @file http_server.h
 * @author xqe2011
 */
#ifndef HTTP_SERVER_H__
#define HTTP_SERVER_H__

#include "gpio_ctrl.h"
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
void HttpServer_SetCors(httpd_req_t* req);
esp_err_t HttpServer_SendError(httpd_req_t* req, int status, const char* reason);
esp_err_t HttpServer_SendJson(httpd_req_t* req, int status, cJSON* root);
esp_err_t HttpServer_SendEmpty(httpd_req_t* req, int status);
esp_err_t HttpServer_SendOptions(httpd_req_t* req);
bool HttpServer_HasJsonContentType(httpd_req_t* req);
void HttpServer_GetLockHeader(httpd_req_t* req, char* out, size_t outLen);
int HttpServer_LockStatusId(const char* lockId, Lock_Kind kind, int pin, uint8_t methods, char* reasonOut,
                            size_t reasonLen);
int HttpServer_LockStatus(httpd_req_t* req, Lock_Kind kind, int pin, uint8_t methods, char* lockIdBuf, size_t lockIdLen,
                    char* reasonOut, size_t reasonLen);
int HttpServer_ParsePathPin(const char* uri, const char* prefix, const char* suffix, int* pinOut);
esp_err_t HttpServer_ParsePinsArray(cJSON* root, int* pins, size_t maxPins, size_t* countOut, char* reason, size_t reasonLen);
esp_err_t HttpServer_ParsePinConfigBody(cJSON* body, GpioCtrl_Mode* modeOut, bool* openDrainOut, bool* pullUpOut,
                                        bool* pullDownOut, char* reason, size_t reasonLen);
/**
 * Expand typed lock resources [{type:pin,pins,method}|{type:power,rails,method}] into Lock_Resource entries.
 * Expanded count is capped by maxOut (typically CONFIG_LOCK_MAX_RESOURCES).
 */
esp_err_t HttpServer_ParseLockResources(cJSON* resourcesArr, Lock_Resource* out, size_t maxOut, size_t* countOut,
                                        char* reason, size_t reasonLen);
/** Group Lock_Resource entries back into typed resources for API responses. Caller owns the array. */
cJSON* HttpServer_SerializeLockResources(const Lock_Resource* resources, size_t count);
void HttpServer_FormatPinRange(char* out, size_t outLen);
int64_t HttpServer_NowUs(void);
esp_err_t HttpServer_ReadBody(httpd_req_t* req, char** outBuf, size_t* outLen);
cJSON* HttpServer_ParseBody(httpd_req_t* req, esp_err_t* errOut);

#endif
