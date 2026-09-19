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
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Request-scoped wrapper. Routes must not retain it without AsyncBegin. */
typedef struct HttpServer_Context HttpServer_Context;
typedef enum {
  HTTP_SERVER_UNKNOWN = -1,
  HTTP_SERVER_GET, HTTP_SERVER_POST, HTTP_SERVER_PUT, HTTP_SERVER_DELETE,
  HTTP_SERVER_PATCH, HTTP_SERVER_OPTIONS,
} HttpServer_Method;
typedef struct { const char* name; const char* value; } HttpServer_Header;
typedef enum { HTTP_CLOUD_BEGIN, HTTP_CLOUD_CHUNK, HTTP_CLOUD_END, HTTP_CLOUD_ABORT } HttpServer_CloudWriteKind;
typedef esp_err_t (*HttpServer_CloudWrite)(void* user, HttpServer_CloudWriteKind kind, const void* data, size_t length);
/** Consumes the JSON envelope on all paths; async dispatch transfers it to AsyncComplete. */
esp_err_t HttpServer_DispatchCloud(cJSON* request, HttpServer_CloudWrite write, void* user);
typedef esp_err_t (*HttpServer_Handler)(HttpServer_Context* ctx);
typedef struct {
  const char* uri;
  HttpServer_Method method;
  HttpServer_Handler handler;
} HttpServer_Route;

/** Descriptors and their strings must remain valid for the server lifetime. */
esp_err_t HttpServer_RegisterRoutes(const HttpServer_Route* routes, size_t count);
const char* HttpServer_GetUri(const HttpServer_Context* ctx);
HttpServer_Method HttpServer_GetMethod(const HttpServer_Context* ctx);
const char* HttpServer_GetFrom(const HttpServer_Context* ctx);
esp_err_t HttpServer_GetHeader(HttpServer_Context* ctx, const char* name, char* out, size_t outLen);
esp_err_t HttpServer_GetQuery(HttpServer_Context* ctx, char* out, size_t outLen);
esp_err_t HttpServer_QueryValue(const char* query, const char* name, char* out, size_t outLen);
/** Headers end with {NULL, NULL}. Send consumes bytes synchronously. */
esp_err_t HttpServer_Send(HttpServer_Context* ctx, int code, const char* contentType,
                          const HttpServer_Header* headers, const void* data, size_t length);
/** Content type and header strings must remain valid through Done.
 * Begin must precede Chunk/Done; zero-length Chunk is a no-op, only Done terminates.
 * A failed send prevents further writes. Ordinary Send cannot follow Begin.
 */
esp_err_t HttpServer_SendChunkBegin(HttpServer_Context* ctx, int code, const char* contentType,
                                    const HttpServer_Header* headers);
esp_err_t HttpServer_SendChunk(HttpServer_Context* ctx, const void* data, size_t length);
esp_err_t HttpServer_SendChunkDone(HttpServer_Context* ctx);
/** On success, original context is detached and must no longer be used.
 * Complete the returned context exactly once, including task-creation/send failures.
 */
esp_err_t HttpServer_AsyncBegin(HttpServer_Context* ctx, HttpServer_Context** out);
esp_err_t HttpServer_AsyncComplete(HttpServer_Context* ctx);

esp_err_t HttpServer_Init(void);
esp_err_t HttpServer_Start(void);
esp_err_t HttpServer_StartPairing(void);
esp_err_t HttpServer_Stop(void);

const char* HttpServer_MethodName(HttpServer_Method method);
void HttpServer_LogCall(HttpServer_Context* ctx);
esp_err_t HttpServer_SendError(HttpServer_Context* ctx, int status, const char* reason);
esp_err_t HttpServer_SendJson(HttpServer_Context* ctx, int status, cJSON* root);
esp_err_t HttpServer_SendEmpty(HttpServer_Context* ctx, int status);
esp_err_t HttpServer_SendOptions(HttpServer_Context* ctx);
bool HttpServer_HasJsonContentType(HttpServer_Context* ctx);
void HttpServer_GetLockHeader(HttpServer_Context* ctx, char* out, size_t outLen);
int HttpServer_LockStatusId(const char* lockId, Lock_Kind kind, int pin, uint8_t methods, char* reasonOut,
                            size_t reasonLen);
int HttpServer_LockStatus(HttpServer_Context* ctx, Lock_Kind kind, int pin, uint8_t methods, char* lockIdBuf, size_t lockIdLen,
                    char* reasonOut, size_t reasonLen);
int HttpServer_ParsePathPin(const char* uri, const char* prefix, const char* suffix, int* pinOut);
esp_err_t HttpServer_ParsePinsArray(cJSON* root, int* pins, size_t maxPins, size_t* countOut, char* reason, size_t reasonLen);
esp_err_t HttpServer_ParsePinConfigBody(cJSON* body, GpioCtrl_Mode* modeOut, bool* openDrainOut, bool* pullUpOut,
                                        bool* pullDownOut, char* reason, size_t reasonLen);
/**
 * Expand typed lock resources [{type:pin,pins,method}|{type:power,rails,method}|{type:uart,ids,method}] into
 * Lock_Resource entries. Expanded count is capped by maxOut (typically CONFIG_LOCK_MAX_RESOURCES).
 */
esp_err_t HttpServer_ParseLockResources(cJSON* resourcesArr, Lock_Resource* out, size_t maxOut, size_t* countOut,
                                        char* reason, size_t reasonLen);
/** Group Lock_Resource entries back into typed resources for API responses. Caller owns the array. */
cJSON* HttpServer_SerializeLockResources(const Lock_Resource* resources, size_t count);
/** Format a Lock_Create conflict into a client-facing reason sentence. */
void HttpServer_FormatLockConflict(const Lock_Conflict* conflict, char* reason, size_t reasonLen);
void HttpServer_FormatPinRange(char* out, size_t outLen);
int64_t HttpServer_NowUs(void);
/** Returns an owned JSON tree; caller must cJSON_Delete it. Detaches cloud bodies without copying. */
cJSON* HttpServer_ParseBody(HttpServer_Context* ctx, esp_err_t* errOut);

#endif
