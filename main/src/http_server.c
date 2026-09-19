/**
 * @name HTTP server module
 * @file http_server.c
 * @author xqe2011
 */
#include "http_server.h"

#include "config.h"
#include "gpio_ctrl.h"
#include "ntp.h"
#include "route.h"
#include "tool.h"
#include "wifi.h"

#include <esp_http_server.h>
#include <esp_log.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>

static const char* tag = "SAIHUB-Http";
static httpd_handle_t server = NULL;
static bool pairingServer = false;
static const HttpServer_Route* cloudRoutes[48];
static size_t cloudRouteCount;
static bool HttpServer_UriMatch(const char* tpl, const char* uri, size_t match_upto);

int64_t HttpServer_NowUs(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
}

typedef enum {
  RESPONSE_IDLE, RESPONSE_STREAMING, RESPONSE_DONE, RESPONSE_FAILED, RESPONSE_DETACHED,
} HttpServer_ResponseState;

struct HttpServer_Context {
  httpd_req_t* req;
  const char* from;
  HttpServer_ResponseState responseState;
  bool async;
  char status[64];
  cJSON* cloudRequest;
  HttpServer_CloudWrite cloudWrite;
  void* cloudUser;
  bool cloudBodySent;
};

static HttpServer_Method HttpServer_FromEspMethod(httpd_method_t method)
{
  switch (method) {
    case HTTP_POST: return HTTP_SERVER_POST;
    case HTTP_PUT: return HTTP_SERVER_PUT;
    case HTTP_DELETE: return HTTP_SERVER_DELETE;
    case HTTP_PATCH: return HTTP_SERVER_PATCH;
    case HTTP_OPTIONS: return HTTP_SERVER_OPTIONS;
    case HTTP_GET: return HTTP_SERVER_GET;
    default: return HTTP_SERVER_UNKNOWN;
  }
}

static httpd_method_t HttpServer_ToEspMethod(HttpServer_Method method)
{
  switch (method) {
    case HTTP_SERVER_POST: return HTTP_POST;
    case HTTP_SERVER_PUT: return HTTP_PUT;
    case HTTP_SERVER_DELETE: return HTTP_DELETE;
    case HTTP_SERVER_PATCH: return HTTP_PATCH;
    case HTTP_SERVER_OPTIONS: return HTTP_OPTIONS;
    default: return HTTP_GET;
  }
}

const char* HttpServer_GetUri(const HttpServer_Context* ctx) {
  return ctx->cloudRequest ? cJSON_GetObjectItemCaseSensitive(ctx->cloudRequest, "path")->valuestring : ctx->req->uri;
}
HttpServer_Method HttpServer_GetMethod(const HttpServer_Context* ctx) {
  if (!ctx->cloudRequest) return HttpServer_FromEspMethod(ctx->req->method);
  const char* name = cJSON_GetObjectItemCaseSensitive(ctx->cloudRequest, "method")->valuestring;
  for (int i = HTTP_SERVER_GET; i <= HTTP_SERVER_OPTIONS; i++)
    if (strcmp(name, HttpServer_MethodName(i)) == 0) return i;
  return HTTP_SERVER_UNKNOWN;
}
const char* HttpServer_GetFrom(const HttpServer_Context* ctx) { return ctx->from; }

const char* HttpServer_MethodName(HttpServer_Method method)
{
  switch (method) {
    case HTTP_SERVER_GET: return "GET";
    case HTTP_SERVER_POST: return "POST";
    case HTTP_SERVER_PUT: return "PUT";
    case HTTP_SERVER_DELETE: return "DELETE";
    case HTTP_SERVER_PATCH: return "PATCH";
    case HTTP_SERVER_OPTIONS: return "OPTIONS";
    default: return "?";
  }
}

esp_err_t HttpServer_GetHeader(HttpServer_Context* ctx, const char* name, char* out, size_t outLen)
{
  if (!ctx || !name || !out || !outLen) return ESP_ERR_INVALID_ARG;
  out[0] = '\0';
  if (ctx->cloudRequest) {
    cJSON* headers = cJSON_GetObjectItemCaseSensitive(ctx->cloudRequest, "headers");
    cJSON* h = NULL;
    cJSON_ArrayForEach(h, headers) {
      if (h->string && strcasecmp(h->string, name) == 0 && cJSON_IsString(h)) {
        if (strlen(h->valuestring) >= outLen) return ESP_ERR_INVALID_SIZE;
        strcpy(out, h->valuestring);
        return ESP_OK;
      }
    }
    return ESP_ERR_NOT_FOUND;
  }
  return httpd_req_get_hdr_value_str(ctx->req, name, out, outLen);
}

esp_err_t HttpServer_GetQuery(HttpServer_Context* ctx, char* out, size_t outLen)
{
  if (!ctx || !out || !outLen) return ESP_ERR_INVALID_ARG;
  out[0] = '\0';
  if (ctx->cloudRequest) {
    const char* query = strchr(HttpServer_GetUri(ctx), '?');
    if (!query) return ESP_ERR_NOT_FOUND;
    if (strlen(++query) >= outLen) return ESP_ERR_INVALID_SIZE;
    strcpy(out, query);
    return ESP_OK;
  }
  return httpd_req_get_url_query_str(ctx->req, out, outLen);
}

esp_err_t HttpServer_QueryValue(const char* query, const char* name, char* out, size_t outLen)
{
  if (!query || !name || !out || !outLen) return ESP_ERR_INVALID_ARG;
  out[0] = '\0';
  return httpd_query_key_value(query, name, out, outLen);
}

void HttpServer_LogCall(HttpServer_Context* ctx)
{
  char lockId[64] = {0};
  HttpServer_GetLockHeader(ctx, lockId, sizeof(lockId));
  ESP_LOGI(tag, "Call rest/%s %s %s%s%s", ctx->from,
           HttpServer_MethodName(HttpServer_GetMethod(ctx)), HttpServer_GetUri(ctx),
           lockId[0] ? " X-Lock-Id=" : "", lockId);
}

static esp_err_t HttpServer_SetCors(httpd_req_t* req)
{
  esp_err_t ret = httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  if (ret == ESP_OK) ret = httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, PATCH, OPTIONS");
  if (ret == ESP_OK) ret = httpd_resp_set_hdr(req, "Access-Control-Allow-Headers",
      "Content-Type, Accept, X-Lock-Id, MCP-Protocol-Version, Mcp-Session-Id");
  if (ret == ESP_OK) ret = httpd_resp_set_hdr(req, "Access-Control-Expose-Headers", "X-Lock-Id, Mcp-Session-Id, MCP-Protocol-Version");
  if (ret == ESP_OK) ret = httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
  return ret;
}

static const char* HttpServer_StatusReason(int code)
{
  switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 409: return "Conflict";
    case 412: return "Precondition Failed";
    case 415: return "Unsupported Media Type";
    case 422: return "Unprocessable Entity";
    case 423: return "Locked";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "";
  }
}

static esp_err_t HttpServer_PrepareResponse(HttpServer_Context* ctx, int code, const char* contentType,
                                           const HttpServer_Header* headers)
{
  if (!ctx || code < 100 || code > 599) return ESP_ERR_INVALID_ARG;
  if (ctx->responseState != RESPONSE_IDLE) return ESP_ERR_INVALID_STATE;
  if (ctx->cloudWrite) {
    char type[64];
    snprintf(type, sizeof(type), "%s", contentType ? contentType : "application/json");
    char* semicolon = strchr(type, ';');
    if (semicolon) *semicolon = '\0';
    if (strcasecmp(type, "application/json") != 0) return ESP_ERR_NOT_SUPPORTED;
    cJSON* envelope = cJSON_CreateObject();
    cJSON* h = cJSON_CreateObject();
    if (!envelope || !h) { cJSON_Delete(envelope); cJSON_Delete(h); return ESP_ERR_NO_MEM; }
    cJSON_AddStringToObject(envelope, "type", "response");
    cJSON_AddStringToObject(envelope, "requestId", cJSON_GetObjectItemCaseSensitive(ctx->cloudRequest, "requestId")->valuestring);
    cJSON_AddNumberToObject(envelope, "status", code);
    cJSON_AddItemToObject(envelope, "headers", h);
    cJSON_AddStringToObject(h, "content-type", "application/json");
    for (const HttpServer_Header* p = headers; p && p->name; p++) {
      if (!p->value || strcasecmp(p->name, "content-type") == 0) { cJSON_Delete(envelope); return ESP_ERR_INVALID_ARG; }
      cJSON_AddStringToObject(h, p->name, p->value);
    }
    char* text = cJSON_PrintUnformatted(envelope);
    cJSON_Delete(envelope);
    if (!text) return ESP_ERR_NO_MEM;
    size_t length = strlen(text);
    text[length - 1] = ',';
    esp_err_t ret = ctx->cloudWrite(ctx->cloudUser, HTTP_CLOUD_BEGIN, text, length);
    free(text);
    if (ret == ESP_OK) ret = ctx->cloudWrite(ctx->cloudUser, HTTP_CLOUD_CHUNK, "\"body\":", 7);
    if (ret != ESP_OK) {
      ctx->cloudWrite(ctx->cloudUser, HTTP_CLOUD_ABORT, NULL, 0);
      ctx->responseState = RESPONSE_FAILED;
    }
    return ret;
  }
  snprintf(ctx->status, sizeof(ctx->status), "%d %s", code, HttpServer_StatusReason(code));
  esp_err_t ret = httpd_resp_set_status(ctx->req, ctx->status);
  if (ret == ESP_OK) ret = HttpServer_SetCors(ctx->req);
  if (ret == ESP_OK && contentType) ret = httpd_resp_set_type(ctx->req, contentType);
  for (const HttpServer_Header* h = headers; ret == ESP_OK && h && h->name; h++) {
    ret = h->value ? httpd_resp_set_hdr(ctx->req, h->name, h->value) : ESP_ERR_INVALID_ARG;
  }
  if (ret != ESP_OK) ctx->responseState = RESPONSE_FAILED;
  else ESP_LOGI(tag, "Resp rest/%s %s %s -> %d", ctx->from,
                HttpServer_MethodName(HttpServer_GetMethod(ctx)), HttpServer_GetUri(ctx), code);
  return ret;
}

esp_err_t HttpServer_Send(HttpServer_Context* ctx, int code, const char* contentType,
                          const HttpServer_Header* headers, const void* data, size_t length)
{
  if ((!data && length) || length > INT_MAX) return ESP_ERR_INVALID_ARG;
  if (ctx && ctx->cloudWrite) {
    esp_err_t ret = HttpServer_SendChunkBegin(ctx, code, contentType, headers);
    if (ret == ESP_OK) ret = HttpServer_SendChunk(ctx, data, length);
    if (ret == ESP_OK) ret = HttpServer_SendChunkDone(ctx);
    return ret;
  }
  esp_err_t ret = HttpServer_PrepareResponse(ctx, code, contentType, headers);
  if (ret != ESP_OK) return ret;
  ret = httpd_resp_send(ctx->req, data, (ssize_t)length);
  ctx->responseState = ret == ESP_OK ? RESPONSE_DONE : RESPONSE_FAILED;
  return ret;
}

esp_err_t HttpServer_SendChunkBegin(HttpServer_Context* ctx, int code, const char* contentType,
                                    const HttpServer_Header* headers)
{
  esp_err_t ret = HttpServer_PrepareResponse(ctx, code, contentType, headers);
  if (ret == ESP_OK) ctx->responseState = RESPONSE_STREAMING;
  return ret;
}

esp_err_t HttpServer_SendChunk(HttpServer_Context* ctx, const void* data, size_t length)
{
  if (!ctx) return ESP_ERR_INVALID_ARG;
  if (ctx->responseState != RESPONSE_STREAMING) return ESP_ERR_INVALID_STATE;
  if ((!data && length) || length > INT_MAX) return ESP_ERR_INVALID_ARG;
  /* Only Done may send the zero-length terminator. */
  if (length == 0) return ESP_OK;
  esp_err_t ret = ctx->cloudWrite ? ctx->cloudWrite(ctx->cloudUser, HTTP_CLOUD_CHUNK, data, length)
                                : httpd_resp_send_chunk(ctx->req, data, (ssize_t)length);
  if (ctx->cloudWrite && ret == ESP_OK) ctx->cloudBodySent = true;
  if (ctx->cloudWrite && ret != ESP_OK) ctx->cloudWrite(ctx->cloudUser, HTTP_CLOUD_ABORT, NULL, 0);
  if (ret != ESP_OK) ctx->responseState = RESPONSE_FAILED;
  return ret;
}

esp_err_t HttpServer_SendChunkDone(HttpServer_Context* ctx)
{
  if (!ctx) return ESP_ERR_INVALID_ARG;
  if (ctx->responseState != RESPONSE_STREAMING) return ESP_ERR_INVALID_STATE;
  esp_err_t ret;
  if (ctx->cloudWrite) {
    ret = ctx->cloudWrite(ctx->cloudUser, HTTP_CLOUD_END, ctx->cloudBodySent ? "}" : "null}", ctx->cloudBodySent ? 1 : 5);
    if (ret != ESP_OK) ctx->cloudWrite(ctx->cloudUser, HTTP_CLOUD_ABORT, NULL, 0);
  } else ret = httpd_resp_send_chunk(ctx->req, NULL, 0);
  ctx->responseState = ret == ESP_OK ? RESPONSE_DONE : RESPONSE_FAILED;
  return ret;
}

esp_err_t HttpServer_AsyncBegin(HttpServer_Context* ctx, HttpServer_Context** out)
{
  if (!ctx || !out) return ESP_ERR_INVALID_ARG;
  *out = NULL;
  if (ctx->async || ctx->responseState != RESPONSE_IDLE) return ESP_ERR_INVALID_STATE;
  HttpServer_Context* copy = calloc(1, sizeof(*copy));
  if (!copy) return ESP_ERR_NO_MEM;
  if (ctx->cloudWrite) {
    *copy = *ctx;
    ctx->cloudRequest = NULL; /* Ownership moves to AsyncComplete. */
    copy->async = true;
    ctx->responseState = RESPONSE_DETACHED;
    *out = copy;
    return ESP_OK;
  }
  esp_err_t ret = httpd_req_async_handler_begin(ctx->req, &copy->req);
  if (ret != ESP_OK) { free(copy); return ret; }
  copy->from = ctx->from;
  copy->async = true;
  ctx->responseState = RESPONSE_DETACHED;
  *out = copy;
  return ESP_OK;
}

esp_err_t HttpServer_AsyncComplete(HttpServer_Context* ctx)
{
  if (!ctx) return ESP_ERR_INVALID_ARG;
  if (!ctx->async) return ESP_ERR_INVALID_STATE;
  esp_err_t ret = ESP_OK;
  if (ctx->cloudWrite) {
    if (ctx->responseState == RESPONSE_STREAMING) ctx->cloudWrite(ctx->cloudUser, HTTP_CLOUD_ABORT, NULL, 0);
    cJSON_Delete(ctx->cloudRequest);
  } else ret = httpd_req_async_handler_complete(ctx->req);
  free(ctx);
  return ret;
}

static esp_err_t HttpServer_RouteAdapter(httpd_req_t* req)
{
  const HttpServer_Route* route = req->user_ctx;
  HttpServer_Context ctx = {.req = req, .from = "http"};
  return route->handler(&ctx);
}

esp_err_t HttpServer_RegisterRoutes(const HttpServer_Route* routes, size_t count)
{
  if (!routes && count) return ESP_ERR_INVALID_ARG;
  if (!server) return ESP_ERR_INVALID_STATE;
  for (size_t i = 0; i < count; i++) {
    if (!routes[i].uri || !routes[i].handler || routes[i].method < HTTP_SERVER_GET ||
        routes[i].method > HTTP_SERVER_OPTIONS) return ESP_ERR_INVALID_ARG;
    httpd_uri_t uri = {.uri = routes[i].uri, .method = HttpServer_ToEspMethod(routes[i].method),
                       .handler = HttpServer_RouteAdapter, .user_ctx = (void*)&routes[i]};
    esp_err_t ret = httpd_register_uri_handler(server, &uri);
    if (ret != ESP_OK) return ret;
    if (!pairingServer) {
      bool registered = false;
      for (size_t j = 0; j < cloudRouteCount; j++) if (cloudRoutes[j] == &routes[i]) registered = true;
      if (!registered) {
        if (cloudRouteCount >= 48) return ESP_ERR_NO_MEM;
        cloudRoutes[cloudRouteCount++] = &routes[i];
      }
    }
  }
  return ESP_OK;
}

esp_err_t HttpServer_SendOptions(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  return HttpServer_Send(ctx, 204, NULL, NULL, NULL, 0);
}

esp_err_t HttpServer_SendError(HttpServer_Context* ctx, int status, const char* reason)
{
  ESP_LOGW(tag, "Resp rest/%s %s %s -> %d %s", ctx->from,
           HttpServer_MethodName(HttpServer_GetMethod(ctx)), HttpServer_GetUri(ctx), status, reason ? reason : "");
  cJSON* root = cJSON_CreateObject();
  if (!root || !cJSON_AddStringToObject(root, "reason", reason ? reason : "internal")) {
    cJSON_Delete(root);
    return HttpServer_Send(ctx, 500, "application/json", NULL, "{\"reason\":\"internal\"}", sizeof("{\"reason\":\"internal\"}") - 1);
  }
  return HttpServer_SendJson(ctx, status, root);
}

esp_err_t HttpServer_SendJson(HttpServer_Context* ctx, int status, cJSON* root)
{
  char* printed = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!printed) return HttpServer_Send(ctx, 500, "application/json", NULL, "{\"reason\":\"internal\"}", sizeof("{\"reason\":\"internal\"}") - 1);
  esp_err_t ret = HttpServer_Send(ctx, status, "application/json", NULL, printed, strlen(printed));
  free(printed);
  return ret;
}

esp_err_t HttpServer_SendEmpty(HttpServer_Context* ctx, int status)
{
  return HttpServer_Send(ctx, status, "application/json", NULL, NULL, 0);
}

bool HttpServer_HasJsonContentType(HttpServer_Context* ctx)
{
  char type[64] = {0};
  if (HttpServer_GetHeader(ctx, "Content-Type", type, sizeof(type)) != ESP_OK) return false;
  char* end = strchr(type, ';');
  if (!end) end = type + strlen(type);
  while (end > type && (end[-1] == ' ' || end[-1] == '\t')) end--;
  *end = '\0';
  return strcasecmp(type, "application/json") == 0;
}

void HttpServer_GetLockHeader(HttpServer_Context* ctx, char* out, size_t outLen)
{
  HttpServer_GetHeader(ctx, "X-Lock-Id", out, outLen);
}

void HttpServer_FormatPinRange(char* out, size_t outLen)
{
  int maxPin = GpioCtrl_GetLogicalCount() - 1;
  if (maxPin < 0) maxPin = 0;
  snprintf(out, outLen, "0 to %d", maxPin);
}

int HttpServer_LockStatusId(const char* lockId, Lock_Kind kind, int pin, uint8_t methods, char* reasonOut,
                            size_t reasonLen)
{
  const char* hdr = (lockId && lockId[0]) ? lockId : NULL;
  char peripheralLabel[48];
  esp_err_t ret = Lock_CheckAccess(kind, pin, methods, hdr, peripheralLabel, sizeof(peripheralLabel));
  if (ret == ESP_ERR_NOT_FOUND) {
    snprintf(reasonOut, reasonLen,
             "X-Lock-Id is missing or is not a known lock. Send a current lock id from POST /lock, or omit the "
             "header only if the resource is unlocked.");
    return 412;
  }
  if (ret == ESP_ERR_NOT_ALLOWED) {
    const char* methodName = (methods & LOCK_METHOD_WRITE) ? "write" : "read";
    const char* label = peripheralLabel[0] ? peripheralLabel : "a peripheral";
    if (kind == LOCK_KIND_GPIO) {
      snprintf(reasonOut, reasonLen, "Pin %d %s is reserved by %s. Disable %s or use other pins.", pin, methodName,
               label, label);
    } else if (kind == LOCK_KIND_UART) {
      snprintf(reasonOut, reasonLen, "UART %d %s is reserved by %s. Disable %s or use another UART.", pin, methodName,
               label, label);
    } else {
      snprintf(reasonOut, reasonLen, "Power %s %s is reserved by %s. Disable %s or use another resource.",
               Lock_KindToString(kind), methodName, label, label);
    }
    return 423;
  }
  if (ret == ESP_ERR_INVALID_STATE) {
    const char* methodName = (methods & LOCK_METHOD_WRITE) ? "write" : "read";
    if (kind == LOCK_KIND_GPIO) {
      snprintf(reasonOut, reasonLen,
               "Pin %d %s is locked by another lock. Send header X-Lock-Id with the holding lock's id, DELETE that "
               "lock, or wait until it expires.",
               pin, methodName);
    } else if (kind == LOCK_KIND_UART) {
      snprintf(reasonOut, reasonLen,
               "UART %d %s is locked by another lock. Send header X-Lock-Id with the holding lock's id, DELETE that "
               "lock, or wait until it expires.",
               pin, methodName);
    } else {
      snprintf(reasonOut, reasonLen,
               "Power %s %s is locked by another lock. Send header X-Lock-Id with the holding lock's id, DELETE that "
               "lock, or wait until it expires.",
               Lock_KindToString(kind), methodName);
    }
    return 423;
  }
  return 0;
}

int HttpServer_LockStatus(HttpServer_Context* ctx, Lock_Kind kind, int pin, uint8_t methods, char* lockIdBuf, size_t lockIdLen,
                    char* reasonOut, size_t reasonLen)
{
  HttpServer_GetLockHeader(ctx, lockIdBuf, lockIdLen);
  return HttpServer_LockStatusId(lockIdBuf, kind, pin, methods, reasonOut, reasonLen);
}

void HttpServer_FormatLockConflict(const Lock_Conflict* conflict, char* reason, size_t reasonLen)
{
  if (reason == NULL || reasonLen == 0) return;
  if (conflict == NULL) {
    snprintf(reason, reasonLen, "Cannot create lock: a resource is already held. DELETE that lock or wait until it expires.");
    return;
  }
  if (conflict->isPeripheral) {
    const char* label = conflict->peripheralLabel[0] ? conflict->peripheralLabel : "a peripheral";
    if (conflict->kind == LOCK_KIND_GPIO) {
      snprintf(reason, reasonLen, "Cannot create lock: pin %d is reserved by %s. Disable %s or use other pins.",
               conflict->pin, label, label);
    } else if (conflict->kind == LOCK_KIND_UART) {
      snprintf(reason, reasonLen, "Cannot create lock: UART %d is reserved by %s. Disable %s or use another UART.",
               conflict->pin, label, label);
    } else {
      snprintf(reason, reasonLen, "Cannot create lock: power %s is reserved by %s. Disable %s or use another resource.",
               Lock_KindToString(conflict->kind), label, label);
    }
    return;
  }
  if (conflict->kind == LOCK_KIND_GPIO) {
    snprintf(reason, reasonLen, "Cannot create lock: pin %d is already held. DELETE that lock or wait until it expires.",
             conflict->pin);
  } else if (conflict->kind == LOCK_KIND_UART) {
    snprintf(reason, reasonLen,
             "Cannot create lock: UART %d is already held. DELETE that lock or wait until it expires.", conflict->pin);
  } else {
    snprintf(reason, reasonLen,
             "Cannot create lock: power %s is already held. DELETE that lock or wait until it expires.",
             Lock_KindToString(conflict->kind));
  }
}

esp_err_t HttpServer_ParsePinConfigBody(cJSON* body, GpioCtrl_Mode* modeOut, bool* openDrainOut, bool* pullUpOut,
                                        bool* pullDownOut, char* reason, size_t reasonLen)
{
  static const char* modeHint = "disable, digitalInput, digitalOutput, digitalInputOutput, pwmOutput";
  cJSON* modeItem = cJSON_GetObjectItem(body, "mode");
  cJSON* pullUpItem = cJSON_GetObjectItem(body, "pullUp");
  cJSON* pullDownItem = cJSON_GetObjectItem(body, "pullDown");
  cJSON* openDrainItem = cJSON_GetObjectItem(body, "openDrain");
  GpioCtrl_Mode mode;
  if (!cJSON_IsString(modeItem) || !GpioCtrl_ModeFromString(modeItem->valuestring, &mode)) {
    snprintf(reason, reasonLen, "mode is invalid. Use one of: %s.", modeHint);
    return ESP_ERR_INVALID_ARG;
  }
  if (!cJSON_IsBool(pullUpItem) || !cJSON_IsBool(pullDownItem)) {
    snprintf(reason, reasonLen, "pullUp and pullDown must be boolean.");
    return ESP_ERR_INVALID_ARG;
  }
  bool openDrain = false;
  if (openDrainItem != NULL) {
    if (!cJSON_IsBool(openDrainItem)) {
      snprintf(reason, reasonLen, "openDrain must be boolean.");
      return ESP_ERR_INVALID_ARG;
    }
    openDrain = cJSON_IsTrue(openDrainItem);
  }
  if (openDrain && (mode == GPIO_CTRL_MODE_DISABLE || mode == GPIO_CTRL_MODE_DIGITAL_INPUT)) {
    snprintf(reason, reasonLen, "openDrain cannot be true when mode is disable or digitalInput.");
    return ESP_ERR_INVALID_ARG;
  }
  *modeOut = mode;
  *openDrainOut = openDrain;
  *pullUpOut = cJSON_IsTrue(pullUpItem);
  *pullDownOut = cJSON_IsTrue(pullDownItem);
  return ESP_OK;
}

int HttpServer_ParsePathPin(const char* uri, const char* prefix, const char* suffix, int* pinOut)
{
  size_t prefixLen = strlen(prefix);
  if (strncmp(uri, prefix, prefixLen) != 0) return -1;
  const char* p = uri + prefixLen;
  if (*p < '0' || *p > '9') return -2;
  char* end = NULL;
  long pin = strtol(p, &end, 10);
  if (end == p) return -2;
  if (suffix) {
    size_t sl = strlen(suffix);
    if (strncmp(end, suffix, sl) != 0) return -2;
    if (end[sl] != '\0' && end[sl] != '?') return -2;
  } else if (*end != '\0' && *end != '?') {
    return -2;
  }
  *pinOut = (int)pin;
  if (!GpioCtrl_IsValidLogicalPin(*pinOut)) return -3;
  return 0;
}

esp_err_t HttpServer_ParsePinsArray(cJSON* root, int* pins, size_t maxPins, size_t* countOut, char* reason, size_t reasonLen)
{
  char range[32];
  HttpServer_FormatPinRange(range, sizeof(range));
  cJSON* arr = cJSON_GetObjectItem(root, "pins");
  if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) == 0) {
    snprintf(reason, reasonLen, "pins must be a non-empty array of integers from %s.", range);
    return ESP_ERR_INVALID_ARG;
  }
  size_t n = (size_t)cJSON_GetArraySize(arr);
  if (n > maxPins) {
    snprintf(reason, reasonLen, "pins array is too long.");
    return ESP_ERR_INVALID_ARG;
  }
  for (size_t i = 0; i < n; i++) {
    cJSON* item = cJSON_GetArrayItem(arr, (int)i);
    if (!cJSON_IsNumber(item)) {
      snprintf(reason, reasonLen, "pins[%u] must be an integer from %s.", (unsigned)i, range);
      return ESP_ERR_INVALID_ARG;
    }
    int pin = item->valueint;
    if (!GpioCtrl_IsValidLogicalPin(pin)) {
      snprintf(reason, reasonLen, "pins[%u] (%d) is invalid. Use a pin from %s.", (unsigned)i, pin, range);
      return ESP_ERR_INVALID_ARG;
    }
    pins[i] = pin;
  }
  *countOut = n;
  return ESP_OK;
}

static esp_err_t HttpServer_ParseLockMethods(cJSON* methods, uint8_t* bitsOut, char* reason, size_t reasonLen)
{
  if (!cJSON_IsArray(methods) || cJSON_GetArraySize(methods) == 0) {
    snprintf(reason, reasonLen, "Each resource.method entry must be read or write.");
    return ESP_ERR_INVALID_ARG;
  }
  uint8_t bits = 0;
  for (int m = 0; m < cJSON_GetArraySize(methods); m++) {
    cJSON* mv = cJSON_GetArrayItem(methods, m);
    if (!cJSON_IsString(mv)) {
      snprintf(reason, reasonLen, "Each resource.method entry must be read or write.");
      return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(mv->valuestring, "read") == 0)
      bits |= LOCK_METHOD_READ;
    else if (strcmp(mv->valuestring, "write") == 0)
      bits |= LOCK_METHOD_WRITE;
    else {
      snprintf(reason, reasonLen, "method is invalid. Each resource.method entry must be read or write.");
      return ESP_ERR_INVALID_ARG;
    }
  }
  *bitsOut = bits;
  return ESP_OK;
}

static cJSON* HttpServer_MethodsToJson(uint8_t methods)
{
  cJSON* methodsArr = cJSON_CreateArray();
  if (methods & LOCK_METHOD_READ) cJSON_AddItemToArray(methodsArr, cJSON_CreateString("read"));
  if (methods & LOCK_METHOD_WRITE) cJSON_AddItemToArray(methodsArr, cJSON_CreateString("write"));
  return methodsArr;
}

esp_err_t HttpServer_ParseLockResources(cJSON* resourcesArr, Lock_Resource* out, size_t maxOut, size_t* countOut,
                                        char* reason, size_t reasonLen)
{
  if (!cJSON_IsArray(resourcesArr) || cJSON_GetArraySize(resourcesArr) == 0) {
    snprintf(reason, reasonLen, "resources must be a non-empty array.");
    return ESP_ERR_INVALID_ARG;
  }
  char range[32];
  HttpServer_FormatPinRange(range, sizeof(range));
  size_t count = 0;
  int groupCount = cJSON_GetArraySize(resourcesArr);
  for (int g = 0; g < groupCount; g++) {
    cJSON* item = cJSON_GetArrayItem(resourcesArr, g);
    if (!cJSON_IsObject(item)) {
      snprintf(reason, reasonLen, "resources[%d] must be an object.", g);
      return ESP_ERR_INVALID_ARG;
    }
    cJSON* typeItem = cJSON_GetObjectItem(item, "type");
    cJSON* pinsItem = cJSON_GetObjectItem(item, "pins");
    cJSON* railsItem = cJSON_GetObjectItem(item, "rails");
    cJSON* idsItem = cJSON_GetObjectItem(item, "ids");
    cJSON* methodsItem = cJSON_GetObjectItem(item, "method");
    if (!cJSON_IsString(typeItem) || typeItem->valuestring == NULL) {
      snprintf(reason, reasonLen, "resources[%d].type must be pin, power, or uart.", g);
      return ESP_ERR_INVALID_ARG;
    }
    uint8_t bits = 0;
    if (HttpServer_ParseLockMethods(methodsItem, &bits, reason, reasonLen) != ESP_OK) return ESP_ERR_INVALID_ARG;

    if (strcmp(typeItem->valuestring, "pin") == 0) {
      if (railsItem != NULL || idsItem != NULL) {
        snprintf(reason, reasonLen, "resources[%d] with type pin must not include rails or ids.", g);
        return ESP_ERR_INVALID_ARG;
      }
      if (!cJSON_IsArray(pinsItem) || cJSON_GetArraySize(pinsItem) == 0) {
        snprintf(reason, reasonLen, "resources[%d].pins must be a non-empty array of integers from %s.", g, range);
        return ESP_ERR_INVALID_ARG;
      }
      int pinN = cJSON_GetArraySize(pinsItem);
      for (int p = 0; p < pinN; p++) {
        if (count >= maxOut) {
          snprintf(reason, reasonLen, "resources expand to too many lock entries (max %u).", (unsigned)maxOut);
          return ESP_ERR_INVALID_ARG;
        }
        cJSON* pinVal = cJSON_GetArrayItem(pinsItem, p);
        if (!cJSON_IsNumber(pinVal) || !GpioCtrl_IsValidLogicalPin(pinVal->valueint)) {
          snprintf(reason, reasonLen, "resources[%d].pins[%d] is invalid. Use a pin from %s.", g, p, range);
          return ESP_ERR_INVALID_ARG;
        }
        out[count].kind = LOCK_KIND_GPIO;
        out[count].pin = pinVal->valueint;
        out[count].methods = bits;
        count++;
      }
    } else if (strcmp(typeItem->valuestring, "power") == 0) {
      if (pinsItem != NULL || idsItem != NULL) {
        snprintf(reason, reasonLen, "resources[%d] with type power must not include pins or ids.", g);
        return ESP_ERR_INVALID_ARG;
      }
      if (!cJSON_IsArray(railsItem) || cJSON_GetArraySize(railsItem) == 0) {
        snprintf(reason, reasonLen, "resources[%d].rails must be a non-empty array of 3v3 or 5v.", g);
        return ESP_ERR_INVALID_ARG;
      }
      int railN = cJSON_GetArraySize(railsItem);
      for (int r = 0; r < railN; r++) {
        if (count >= maxOut) {
          snprintf(reason, reasonLen, "resources expand to too many lock entries (max %u).", (unsigned)maxOut);
          return ESP_ERR_INVALID_ARG;
        }
        cJSON* railVal = cJSON_GetArrayItem(railsItem, r);
        Lock_Kind kind;
        if (!cJSON_IsString(railVal) || !Lock_PowerFromString(railVal->valuestring, &kind)) {
          snprintf(reason, reasonLen, "resources[%d].rails[%d] is invalid. Use one of: 3v3, 5v.", g, r);
          return ESP_ERR_INVALID_ARG;
        }
        out[count].kind = kind;
        out[count].pin = 0;
        out[count].methods = bits;
        count++;
      }
    } else if (strcmp(typeItem->valuestring, "uart") == 0) {
      if (pinsItem != NULL || railsItem != NULL) {
        snprintf(reason, reasonLen, "resources[%d] with type uart must not include pins or rails.", g);
        return ESP_ERR_INVALID_ARG;
      }
      if (!cJSON_IsArray(idsItem) || cJSON_GetArraySize(idsItem) == 0) {
        snprintf(reason, reasonLen, "resources[%d].ids must be a non-empty array of UART ids from GET /uart/.", g);
        return ESP_ERR_INVALID_ARG;
      }
      int idN = cJSON_GetArraySize(idsItem);
      for (int u = 0; u < idN; u++) {
        if (count >= maxOut) {
          snprintf(reason, reasonLen, "resources expand to too many lock entries (max %u).", (unsigned)maxOut);
          return ESP_ERR_INVALID_ARG;
        }
        cJSON* idVal = cJSON_GetArrayItem(idsItem, u);
        if (!cJSON_IsNumber(idVal) || !Lock_IsValidUartId(idVal->valueint)) {
          snprintf(reason, reasonLen, "resources[%d].ids[%d] is invalid. Use an id from GET /uart/.", g, u);
          return ESP_ERR_INVALID_ARG;
        }
        out[count].kind = LOCK_KIND_UART;
        out[count].pin = idVal->valueint;
        out[count].methods = bits;
        count++;
      }
    } else {
      snprintf(reason, reasonLen, "resources[%d].type must be pin, power, or uart.", g);
      return ESP_ERR_INVALID_ARG;
    }
  }
  *countOut = count;
  return ESP_OK;
}

cJSON* HttpServer_SerializeLockResources(const Lock_Resource* resources, size_t count)
{
  cJSON* resArr = cJSON_CreateArray();
  if (resArr == NULL || resources == NULL || count == 0) return resArr;
  bool used[CONFIG_LOCK_MAX_RESOURCES];
  memset(used, 0, sizeof(used));
  if (count > CONFIG_LOCK_MAX_RESOURCES) count = CONFIG_LOCK_MAX_RESOURCES;

  for (size_t i = 0; i < count; i++) {
    if (used[i]) continue;
    uint8_t methods = resources[i].methods;
    cJSON* r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "method", HttpServer_MethodsToJson(methods));

    if (resources[i].kind == LOCK_KIND_GPIO) {
      cJSON_AddStringToObject(r, "type", "pin");
      cJSON* pins = cJSON_CreateArray();
      for (size_t j = i; j < count; j++) {
        if (used[j]) continue;
        if (resources[j].kind != LOCK_KIND_GPIO || resources[j].methods != methods) continue;
        cJSON_AddItemToArray(pins, cJSON_CreateNumber(resources[j].pin));
        used[j] = true;
      }
      cJSON_AddItemToObject(r, "pins", pins);
    } else if (resources[i].kind == LOCK_KIND_UART) {
      cJSON_AddStringToObject(r, "type", "uart");
      cJSON* ids = cJSON_CreateArray();
      for (size_t j = i; j < count; j++) {
        if (used[j]) continue;
        if (resources[j].kind != LOCK_KIND_UART || resources[j].methods != methods) continue;
        cJSON_AddItemToArray(ids, cJSON_CreateNumber(resources[j].pin));
        used[j] = true;
      }
      cJSON_AddItemToObject(r, "ids", ids);
    } else {
      cJSON_AddStringToObject(r, "type", "power");
      cJSON* rails = cJSON_CreateArray();
      for (size_t j = i; j < count; j++) {
        if (used[j]) continue;
        if (resources[j].kind == LOCK_KIND_GPIO || resources[j].kind == LOCK_KIND_UART ||
            resources[j].methods != methods)
          continue;
        cJSON_AddItemToArray(rails, cJSON_CreateString(Lock_KindToString(resources[j].kind)));
        used[j] = true;
      }
      cJSON_AddItemToObject(r, "rails", rails);
    }
    cJSON_AddItemToArray(resArr, r);
  }
  return resArr;
}

static esp_err_t HttpServer_ReadHttpBody(HttpServer_Context* ctx, char** outBuf, size_t* outLen)
{
  if (!ctx || !outBuf || !outLen) return ESP_ERR_INVALID_ARG;
  *outBuf = NULL;
  *outLen = 0;
  if (ctx->responseState != RESPONSE_IDLE) return ESP_ERR_INVALID_STATE;
  size_t total = ctx->req->content_len;
  if (total > 16 * 1024) {
    return ESP_ERR_INVALID_SIZE;
  }
  char* buf = calloc(1, (size_t)total + 1);
  if (buf == NULL) return ESP_ERR_NO_MEM;
  size_t received = 0;
  while (received < total) {
    int r = httpd_req_recv(ctx->req, buf + received, total - received);
    if (r <= 0) {
      free(buf);
      return ESP_FAIL;
    }
    received += r;
  }
  *outBuf = buf;
  *outLen = (size_t)received;
  return ESP_OK;
}

cJSON* HttpServer_ParseBody(HttpServer_Context* ctx, esp_err_t* errOut)
{
  if (ctx && ctx->cloudRequest) {
    cJSON* body = cJSON_GetObjectItemCaseSensitive(ctx->cloudRequest, "body");
    if (ctx->responseState != RESPONSE_IDLE || !cJSON_IsObject(body)) {
      if (errOut) *errOut = ESP_ERR_INVALID_ARG;
      return NULL;
    }
    if (errOut) *errOut = ESP_OK;
    return cJSON_DetachItemFromObjectCaseSensitive(ctx->cloudRequest, "body");
  }
  char* buf = NULL;
  size_t len = 0;
  esp_err_t ret = HttpServer_ReadHttpBody(ctx, &buf, &len);
  if (ret != ESP_OK) {
    if (errOut) *errOut = ret;
    return NULL;
  }
  if (len == 0) {
    free(buf);
    if (errOut) *errOut = ESP_ERR_INVALID_ARG;
    return NULL;
  }
  /* Skip /mcp: JSON-RPC envelope is logged at tools/call instead. */
  if (strcmp(HttpServer_GetUri(ctx), "/mcp") != 0) {
    int n = (int)(len < 512 ? len : 512);
    TOOL_CALL_LOG("rest/%s %s %s args=%.*s", HttpServer_GetFrom(ctx), HttpServer_MethodName(HttpServer_GetMethod(ctx)), HttpServer_GetUri(ctx), n, buf);
  }
  cJSON* root = cJSON_Parse(buf);
  free(buf);
  if (root == NULL) {
    if (errOut) *errOut = ESP_ERR_INVALID_ARG;
    return NULL;
  }
  if (errOut) *errOut = ESP_OK;
  return root;
}

static esp_err_t HttpServer_NotFoundHandler(httpd_req_t* req, httpd_err_code_t err)
{
  (void)err;
  HttpServer_Context ctx = {.req = req, .from = "http"};
  HttpServer_LogCall(&ctx);
  if (pairingServer) {
    return HttpServer_SendError(&ctx, 404, "This URL does not exist. Open /wifi/page for Wi-Fi setup.");
  }
  return HttpServer_SendError(&ctx, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
}

static void HttpServer_OnNtpSynced(void)
{
  if (!Wifi_IsConnected() || Wifi_IsPairing()) return;
  if (HttpServer_Start() != ESP_OK) {
    ESP_LOGE(tag, "HTTP start failed");
  }
}

static void HttpServer_OnWifiDisconnected(void)
{
  if (Wifi_IsPairing()) {
    ESP_LOGW(tag, "WiFi disconnected during pairing; keeping portal up");
    return;
  }
  ESP_LOGW(tag, "WiFi disconnected; stopping HTTP");
  HttpServer_Stop();
}

static void HttpServer_OnPairingStarted(void)
{
  ESP_LOGI(tag, "Pairing started; switching to portal HTTP");
  HttpServer_Stop();
  if (HttpServer_StartPairing() != ESP_OK) {
    ESP_LOGE(tag, "Portal HTTP start failed");
  }
}

static void HttpServer_OnPairingStopped(void)
{
  ESP_LOGI(tag, "Pairing stopped; closing portal HTTP");
  HttpServer_Stop();

  char ip[16] = {0};
  char reason[96] = {0};
  Wifi_PairState state = Wifi_GetPairStatus(ip, sizeof(ip), reason, sizeof(reason));
  /* Success path fires connected callback next (NTP then API). Cancel-while-online restores API. */
  if (Wifi_IsConnected() && state != WIFI_PAIR_STATE_CONNECTED) {
    ESP_LOGI(tag, "Restoring API HTTP after pairing cancel");
    if (HttpServer_Start() != ESP_OK) {
      ESP_LOGE(tag, "HTTP start failed after pairing cancel");
    }
  }
}

esp_err_t HttpServer_Init(void)
{
  TOOL_CHECK_ESP_OK_OR_RETURN(Ntp_RegisterSyncedCallback(HttpServer_OnNtpSynced));
  TOOL_CHECK_ESP_OK_OR_RETURN(Wifi_RegisterDisconnectedCallback(HttpServer_OnWifiDisconnected));
  TOOL_CHECK_ESP_OK_OR_RETURN(Wifi_RegisterPairingStartedCallback(HttpServer_OnPairingStarted));
  TOOL_CHECK_ESP_OK_OR_RETURN(Wifi_RegisterPairingStoppedCallback(HttpServer_OnPairingStopped));
  return ESP_OK;
}

esp_err_t HttpServer_Stop(void)
{
  if (server) {
    httpd_stop(server);
    server = NULL;
    pairingServer = false;
    ESP_LOGI(tag, "HTTP server stopped");
  }
  return ESP_OK;
}

/* URI matcher for templates like "/pin/<id>/pwm" written with a mid-path '*'.
 * Stock httpd_uri_match_wildcard only treats a trailing star; a mid-path star is
 * literal, so those handlers never match and trailing "/pin/" + star (PUT) yields
 * 405 on GET/POST. Here: mid-path '*' = one non-empty path segment; trailing '*' =
 * remainder.
 */
static bool HttpServer_UriMatch(const char* tpl, const char* uri, size_t match_upto)
{
  const char* t = tpl;
  const char* u = uri;
  const char* u_end = uri + match_upto;

  while (*t) {
    if (*t == '*') {
      t++;
      if (*t == '\0') {
        return true;
      }
      if (u >= u_end || *u == '/') {
        return false;
      }
      while (u < u_end && *u != '/') {
        u++;
      }
      continue;
    }
    if (u >= u_end || *u != *t) {
      return false;
    }
    t++;
    u++;
  }
  return u == u_end;
}

esp_err_t HttpServer_DispatchCloud(cJSON* request, HttpServer_CloudWrite write, void* user)
{
  cJSON* id = cJSON_GetObjectItemCaseSensitive(request, "requestId");
  cJSON* path = cJSON_GetObjectItemCaseSensitive(request, "path");
  cJSON* method = cJSON_GetObjectItemCaseSensitive(request, "method");
  cJSON* headers = cJSON_GetObjectItemCaseSensitive(request, "headers");
  cJSON* body = cJSON_GetObjectItemCaseSensitive(request, "body");
  if (!write || !cJSON_IsString(id) || !id->valuestring[0] || strlen(id->valuestring) > 128 ||
      !cJSON_IsString(path) || path->valuestring[0] != '/' || strlen(path->valuestring) > 1024 ||
      !cJSON_IsString(method) || !cJSON_IsObject(headers)) {
    cJSON_Delete(request);
    return ESP_ERR_INVALID_ARG;
  }
  HttpServer_Context ctx = {.from = "cloud", .cloudRequest = request, .cloudWrite = write, .cloudUser = user};
  esp_err_t ret;
  if (!HttpServer_HasJsonContentType(&ctx)) {
    ret = HttpServer_SendError(&ctx, 415, "cloud relay not support non-JSON content-type currently, use application/json instead");
    goto cleanup;
  }
  if (!cJSON_IsObject(body) && !cJSON_IsNull(body)) {
    ret = HttpServer_SendError(&ctx, 400, "body must be a JSON object or null");
    goto cleanup;
  }
  if (!server || pairingServer || Wifi_IsPairing()) {
    ret = HttpServer_SendError(&ctx, 503, "device API unavailable");
    goto cleanup;
  }
  size_t pathLen = strcspn(path->valuestring, "?");
  HttpServer_Handler handler = NULL;
  bool foundPath = false;
  for (size_t i = 0; i < cloudRouteCount; i++) {
    if (!HttpServer_UriMatch(cloudRoutes[i]->uri, path->valuestring, pathLen)) continue;
    foundPath = true;
    if (cloudRoutes[i]->method == HttpServer_GetMethod(&ctx)) { handler = cloudRoutes[i]->handler; break; }
  }
  ret = handler ? handler(&ctx) : HttpServer_SendError(&ctx, foundPath ? 405 : 404, foundPath ? "method not allowed" : "not found");
  if (ctx.responseState == RESPONSE_IDLE) {
    ret = HttpServer_SendError(&ctx, ret == ESP_ERR_NOT_SUPPORTED ? 415 : 500,
        ret == ESP_ERR_NOT_SUPPORTED ? "cloud relay not support non-JSON content-type currently, use application/json instead" : "internal");
  } else if (ctx.responseState == RESPONSE_STREAMING) {
    write(user, HTTP_CLOUD_ABORT, NULL, 0);
  }
cleanup:
  cJSON_Delete(ctx.cloudRequest); /* NULL when AsyncBegin took ownership. */
  return ret;
}

static esp_err_t HttpServer_PortalRedirect(httpd_req_t* req)
{
  HttpServer_Context ctx = {.req = req, .from = "http"};
  HttpServer_LogCall(&ctx);
  static const HttpServer_Header headers[] = {
      {"Location", CONFIG_WIFI_PORTAL_URL}, {"Cache-Control", "no-store"}, {NULL, NULL},
  };
  return HttpServer_Send(&ctx, 302, "text/plain", headers, "Redirecting", 11);
}

static esp_err_t HttpServer_StartWithConfig(bool pairing)
{
  if (server) {
    if (pairingServer == pairing) return ESP_OK;
    HttpServer_Stop();
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 8192;
  config.server_port = 80;
  config.uri_match_fn = HttpServer_UriMatch;
  config.max_uri_handlers = 48;
  config.lru_purge_enable = true;
  config.recv_wait_timeout = 65;
  config.send_wait_timeout = 65;
  if (pairing) config.max_open_sockets = 12;

  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(httpd_start(&server, &config), "httpd_start failed");
  pairingServer = pairing;

  if (pairing) {
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(Route_PortalRegister(), "portal routes failed");
    static const httpd_uri_t redirect = {.uri = "/*", .method = HTTP_GET, .handler = HttpServer_PortalRedirect};
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(httpd_register_uri_handler(server, &redirect), "portal redirect failed");
  } else {
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(Route_ControlRegister(), "control ui route failed");
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(Route_OpenApiRegister(), "openapi routes failed");
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(Route_PinRegister(), "pin routes failed");
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(Route_UartRegister(), "uart routes failed");
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(Route_LockRegister(), "lock routes failed");
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(Route_PowerRegister(), "power routes failed");
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(Route_ScriptRegister(), "script routes failed");
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(Route_McpRegister(), "mcp routes failed");
  }

  static const HttpServer_Route optionsUri = {.uri = "/*", .method = HTTP_SERVER_OPTIONS, .handler = HttpServer_SendOptions};
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(HttpServer_RegisterRoutes(&optionsUri, 1), "options cors route failed");
  httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, HttpServer_NotFoundHandler);
  ESP_LOGI(tag, "HTTP server started on port 80 (%s)", pairing ? "pairing" : "api");
  return ESP_OK;
}

esp_err_t HttpServer_Start(void)
{
  return HttpServer_StartWithConfig(false);
}

esp_err_t HttpServer_StartPairing(void)
{
  return HttpServer_StartWithConfig(true);
}
