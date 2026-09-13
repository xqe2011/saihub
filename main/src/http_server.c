#include "http_server.h"

#include "config.h"
#include "gpio_ctrl.h"
#include "json_util.h"
#include "lock.h"
#include "tool.h"

#include <cJSON.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* tag = "Http";
static httpd_handle_t s_server = NULL;

extern const uint8_t openapi_json_start[] asm("_binary_openapi_json_start");
extern const uint8_t openapi_json_end[] asm("_binary_openapi_json_end");

static bool Http_HasJsonContentType(httpd_req_t* req)
{
  char type[64] = {0};
  if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK) {
    return false;
  }
  return strstr(type, "application/json") != NULL;
}

static void Http_GetLockHeader(httpd_req_t* req, char* out, size_t outLen)
{
  out[0] = '\0';
  httpd_req_get_hdr_value_str(req, "X-Lock-Id", out, outLen);
}

static int Http_LockStatus(httpd_req_t* req, int pin, uint8_t methods, char* lockIdBuf, size_t lockIdLen,
                           char* reasonOut, size_t reasonLen)
{
  Http_GetLockHeader(req, lockIdBuf, lockIdLen);
  const char* hdr = lockIdBuf[0] ? lockIdBuf : NULL;
  esp_err_t ret = Lock_CheckAccess(pin, methods, hdr);
  if (ret == ESP_ERR_NOT_FOUND) {
    snprintf(reasonOut, reasonLen,
             "X-Lock-Id is missing or is not a known lock. Send a current lock id from POST /locks, or omit the "
             "header only if the pin is unlocked.");
    return 412;
  }
  if (ret == ESP_ERR_INVALID_STATE) {
    const char* methodName = (methods & LOCK_METHOD_WRITE) ? "write" : "read";
    snprintf(reasonOut, reasonLen,
             "Pin %d %s is locked by another lock. Send header X-Lock-Id with the holding lock's id, DELETE that "
             "lock, or wait until it expires.",
             pin, methodName);
    return 423;
  }
  return 0;
}

static int Http_ParsePathPin(const char* uri, const char* prefix, const char* suffix, int* pinOut)
{
  /* uri like /gpios/5/level */
  size_t prefixLen = strlen(prefix);
  if (strncmp(uri, prefix, prefixLen) != 0) return -1;
  const char* p = uri + prefixLen;
  if (*p < '0' || *p > '9') return -2; /* not a number -> unknown URL style */
  char* end = NULL;
  long pin = strtol(p, &end, 10);
  if (end == p) return -2;
  if (suffix) {
    size_t sl = strlen(suffix);
    if (strncmp(end, suffix, sl) != 0) return -2;
    if (end[sl] != '\0' && end[sl] != '?') return -2;
  }
  *pinOut = (int)pin;
  if (!GpioCtrl_IsValidLogicalPin(*pinOut)) return -3; /* out of range */
  return 0;
}

static esp_err_t Http_ParsePinsArray(cJSON* root, int* pins, size_t maxPins, size_t* countOut, char* reason,
                                     size_t reasonLen)
{
  cJSON* arr = cJSON_GetObjectItem(root, "pins");
  if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) == 0) {
    snprintf(reason, reasonLen, "pins must be a non-empty array of integers from 0 to 7.");
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
      snprintf(reason, reasonLen, "pins[%u] must be an integer from 0 to 7.", (unsigned)i);
      return ESP_ERR_INVALID_ARG;
    }
    int pin = item->valueint;
    if (!GpioCtrl_IsValidLogicalPin(pin)) {
      snprintf(reason, reasonLen, "pins[%u] (%d) is invalid. Use a pin from 0 to 7.", (unsigned)i, pin);
      return ESP_ERR_INVALID_ARG;
    }
    pins[i] = pin;
  }
  *countOut = n;
  return ESP_OK;
}

/* ---------- handlers ---------- */

static esp_err_t Handler_OpenApi(httpd_req_t* req)
{
  size_t len = (size_t)(openapi_json_end - openapi_json_start);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, (const char*)openapi_json_start, len);
}

static esp_err_t Handler_ListGpios(httpd_req_t* req)
{
  Lock_SweepExpired();
  cJSON* root = cJSON_CreateObject();
  cJSON* gpios = cJSON_CreateArray();
  for (int i = 0; i < CONFIG_GPIO_LOGICAL_COUNT; i++) {
    GpioCtrl_State st;
    GpioCtrl_GetState(i, &st);
    cJSON* item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "pin", i);
    cJSON_AddStringToObject(item, "mode", GpioCtrl_ModeToString(st.mode));
    cJSON_AddBoolToObject(item, "pullUp", st.pullUp);
    cJSON_AddBoolToObject(item, "pullDown", st.pullDown);
    cJSON_AddNumberToObject(item, "level", st.level);
    cJSON_AddItemToArray(gpios, item);
  }
  cJSON_AddItemToObject(root, "gpios", gpios);
  cJSON_AddNumberToObject(root, "time", (double)Json_NowUs());
  return Json_SendJson(req, 200, root);
}

static esp_err_t Handler_PutConfigSingle(httpd_req_t* req)
{
  Lock_SweepExpired();
  int pin = 0;
  int pr = Http_ParsePathPin(req->uri, "/gpios/", "/config", &pin);
  if (pr == -2) {
    return Json_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  if (pr == -3) {
    char reason[96];
    /* recover pin number from uri */
    const char* p = req->uri + strlen("/gpios/");
    long raw = strtol(p, NULL, 10);
    snprintf(reason, sizeof(reason), "Pin %ld does not exist. Use a pin from 0 to 7.", raw);
    return Json_SendError(req, 404, reason);
  }

  if (req->content_len > 0 && !Http_HasJsonContentType(req)) {
    return Json_SendError(req, 415, "Content-Type must be application/json.");
  }
  if (!Http_HasJsonContentType(req)) {
    return Json_SendError(req, 415, "Content-Type must be application/json.");
  }

  char lockId[64];
  char reason[256];
  int st = Http_LockStatus(req, pin, LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return Json_SendError(req, st, reason);

  esp_err_t perr = ESP_OK;
  cJSON* body = Json_ParseBody(req, &perr);
  if (body == NULL) {
    return Json_SendError(req, 400, "invalid_json");
  }

  cJSON* modeItem = cJSON_GetObjectItem(body, "mode");
  cJSON* pullUpItem = cJSON_GetObjectItem(body, "pullUp");
  cJSON* pullDownItem = cJSON_GetObjectItem(body, "pullDown");
  GpioCtrl_Mode mode;
  if (!cJSON_IsString(modeItem) || !GpioCtrl_ModeFromString(modeItem->valuestring, &mode)) {
    cJSON_Delete(body);
    return Json_SendError(req, 400,
                          "mode is invalid. Use one of: disable, input, output, output_open_drain, input_output, "
                          "input_output_open_drain.");
  }
  if (!cJSON_IsBool(pullUpItem) || !cJSON_IsBool(pullDownItem)) {
    cJSON_Delete(body);
    return Json_SendError(req, 400, "pullUp and pullDown must be boolean.");
  }
  bool pullUp = cJSON_IsTrue(pullUpItem);
  bool pullDown = cJSON_IsTrue(pullDownItem);
  cJSON_Delete(body);

  if (GpioCtrl_SetConfig(pin, mode, pullUp, pullDown) != ESP_OK) {
    return Json_SendError(req, 500, "internal");
  }
  Lock_Touch(lockId[0] ? lockId : NULL);
  return Json_SendEmpty(req, 204);
}

static esp_err_t Handler_GetLevelSingle(httpd_req_t* req)
{
  Lock_SweepExpired();
  int pin = 0;
  int pr = Http_ParsePathPin(req->uri, "/gpios/", "/level", &pin);
  if (pr == -2) {
    return Json_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  if (pr == -3) {
    const char* p = req->uri + strlen("/gpios/");
    long raw = strtol(p, NULL, 10);
    char reason[96];
    snprintf(reason, sizeof(reason), "Pin %ld does not exist. Use a pin from 0 to 7.", raw);
    return Json_SendError(req, 404, reason);
  }

  char lockId[64];
  char reason[256];
  int st = Http_LockStatus(req, pin, LOCK_METHOD_READ, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return Json_SendError(req, st, reason);

  int level = 0;
  if (GpioCtrl_GetLevel(pin, &level) != ESP_OK) {
    return Json_SendError(req, 500, "internal");
  }
  Lock_Touch(lockId[0] ? lockId : NULL);
  cJSON* root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "level", level);
  cJSON_AddNumberToObject(root, "time", (double)Json_NowUs());
  return Json_SendJson(req, 200, root);
}

static esp_err_t Handler_PostLevelSingle(httpd_req_t* req)
{
  Lock_SweepExpired();
  int pin = 0;
  int pr = Http_ParsePathPin(req->uri, "/gpios/", "/level", &pin);
  if (pr == -2) {
    return Json_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  if (pr == -3) {
    const char* p = req->uri + strlen("/gpios/");
    long raw = strtol(p, NULL, 10);
    char reason[96];
    snprintf(reason, sizeof(reason), "Pin %ld does not exist. Use a pin from 0 to 7.", raw);
    return Json_SendError(req, 404, reason);
  }

  if (!Http_HasJsonContentType(req)) {
    return Json_SendError(req, 415, "Content-Type must be application/json.");
  }

  char lockId[64];
  char reason[256];
  int st = Http_LockStatus(req, pin, LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return Json_SendError(req, st, reason);

  esp_err_t perr = ESP_OK;
  cJSON* body = Json_ParseBody(req, &perr);
  if (body == NULL) return Json_SendError(req, 400, "invalid_json");

  cJSON* levelItem = cJSON_GetObjectItem(body, "level");
  if (!cJSON_IsNumber(levelItem) || (levelItem->valueint != 0 && levelItem->valueint != 1)) {
    cJSON_Delete(body);
    return Json_SendError(req, 400, "level must be 0 or 1.");
  }
  int level = levelItem->valueint;
  cJSON_Delete(body);

  if (!GpioCtrl_IsOutputCapable(pin)) {
    snprintf(reason, sizeof(reason),
             "Pin %d is configured as input, so level cannot be written. PUT /gpios/%d/config with mode output, "
             "output_open_drain, input_output, or input_output_open_drain first.",
             pin, pin);
    return Json_SendError(req, 422, reason);
  }
  if (GpioCtrl_SetLevel(pin, level) != ESP_OK) {
    return Json_SendError(req, 500, "internal");
  }
  Lock_Touch(lockId[0] ? lockId : NULL);
  return Json_SendEmpty(req, 204);
}

static esp_err_t Handler_PostPulseSingle(httpd_req_t* req)
{
  Lock_SweepExpired();
  int pin = 0;
  int pr = Http_ParsePathPin(req->uri, "/gpios/", "/pulse", &pin);
  if (pr == -2) {
    return Json_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  if (pr == -3) {
    const char* p = req->uri + strlen("/gpios/");
    long raw = strtol(p, NULL, 10);
    char reason[96];
    snprintf(reason, sizeof(reason), "Pin %ld does not exist. Use a pin from 0 to 7.", raw);
    return Json_SendError(req, 404, reason);
  }
  if (!Http_HasJsonContentType(req)) {
    return Json_SendError(req, 415, "Content-Type must be application/json.");
  }

  char lockId[64];
  char reason[256];
  int st = Http_LockStatus(req, pin, LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return Json_SendError(req, st, reason);

  esp_err_t perr = ESP_OK;
  cJSON* body = Json_ParseBody(req, &perr);
  if (body == NULL) return Json_SendError(req, 400, "invalid_json");

  cJSON* widthItem = cJSON_GetObjectItem(body, "width");
  cJSON* levelItem = cJSON_GetObjectItem(body, "level");
  if (!cJSON_IsNumber(widthItem) || widthItem->valuedouble < 1 ||
      widthItem->valuedouble > (double)CONFIG_PULSE_MAX_WIDTH_US) {
    cJSON_Delete(body);
    return Json_SendError(req, 400, "width must be an integer from 1 to 1000000 microseconds.");
  }
  if (!cJSON_IsNumber(levelItem) || (levelItem->valueint != 0 && levelItem->valueint != 1)) {
    cJSON_Delete(body);
    return Json_SendError(req, 400, "level must be 0 or 1.");
  }
  uint64_t width = (uint64_t)widthItem->valuedouble;
  int level = levelItem->valueint;
  cJSON_Delete(body);

  if (!GpioCtrl_IsOutputCapable(pin)) {
    snprintf(reason, sizeof(reason),
             "Pin %d is configured as input, so level cannot be written. PUT /gpios/%d/config with mode output, "
             "output_open_drain, input_output, or input_output_open_drain first.",
             pin, pin);
    return Json_SendError(req, 422, reason);
  }
  if (GpioCtrl_Pulse(pin, level, width) != ESP_OK) {
    return Json_SendError(req, 500, "internal");
  }
  Lock_Touch(lockId[0] ? lockId : NULL);
  return Json_SendEmpty(req, 204);
}

static esp_err_t Handler_GetTraceSingle(httpd_req_t* req)
{
  Lock_SweepExpired();
  int pin = 0;
  /* URI may include query string in some stacks; strip at ? */
  char path[128];
  strncpy(path, req->uri, sizeof(path) - 1);
  path[sizeof(path) - 1] = '\0';
  char* q = strchr(path, '?');
  if (q) *q = '\0';

  int pr = Http_ParsePathPin(path, "/gpios/", "/trace", &pin);
  if (pr == -2) {
    return Json_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  if (pr == -3) {
    const char* p = path + strlen("/gpios/");
    long raw = strtol(p, NULL, 10);
    char reason[96];
    snprintf(reason, sizeof(reason), "Pin %ld does not exist. Use a pin from 0 to 7.", raw);
    return Json_SendError(req, 404, reason);
  }

  char lockId[64];
  char reason[256];
  int st = Http_LockStatus(req, pin, LOCK_METHOD_READ | LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason,
                           sizeof(reason));
  if (st) return Json_SendError(req, st, reason);

  char edgeBuf[32] = {0};
  char durationBuf[32] = {0};
  httpd_req_get_url_query_str(req, reason, sizeof(reason)); /* reuse */
  if (httpd_query_key_value(reason, "edge", edgeBuf, sizeof(edgeBuf)) != ESP_OK) {
    strcpy(edgeBuf, "both");
  }
  uint64_t duration = CONFIG_TRACE_DEFAULT_DURATION_US;
  if (httpd_query_key_value(reason, "duration", durationBuf, sizeof(durationBuf)) == ESP_OK) {
    duration = strtoull(durationBuf, NULL, 10);
  }
  GpioCtrl_Edge edge;
  if (!GpioCtrl_EdgeFromString(edgeBuf, &edge)) {
    return Json_SendError(req, 400, "edge is invalid. Use one of: raising, falling, both.");
  }
  if (duration < 1 || duration > CONFIG_TRACE_MAX_DURATION_US) {
    return Json_SendError(req, 400, "duration must be an integer from 1 to 60000000 microseconds (max 60 seconds).");
  }

  GpioCtrl_TraceEvent* events = calloc(CONFIG_TRACE_MAX_EVENTS, sizeof(GpioCtrl_TraceEvent));
  if (events == NULL) return Json_SendError(req, 500, "internal");
  size_t count = 0;
  esp_err_t tr = GpioCtrl_Trace(&pin, 1, edge, duration, events, CONFIG_TRACE_MAX_EVENTS, &count, false);
  if (tr != ESP_OK) {
    free(events);
    return Json_SendError(req, 500, "internal");
  }

  cJSON* root = cJSON_CreateObject();
  cJSON* arr = cJSON_CreateArray();
  for (size_t i = 0; i < count; i++) {
    cJSON* ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "edge", events[i].edge);
    cJSON_AddNumberToObject(ev, "level", events[i].level);
    cJSON_AddNumberToObject(ev, "time", (double)events[i].time);
    cJSON_AddItemToArray(arr, ev);
  }
  cJSON_AddItemToObject(root, "events", arr);
  free(events);
  Lock_Touch(lockId[0] ? lockId : NULL);
  return Json_SendJson(req, 200, root);
}

static esp_err_t Handler_BatchLevelGet(httpd_req_t* req)
{
  Lock_SweepExpired();
  if (!Http_HasJsonContentType(req)) {
    return Json_SendError(req, 415, "Content-Type must be application/json.");
  }
  esp_err_t perr = ESP_OK;
  cJSON* body = Json_ParseBody(req, &perr);
  if (body == NULL) return Json_SendError(req, 400, "invalid_json");

  int pins[CONFIG_GPIO_LOGICAL_COUNT];
  size_t count = 0;
  char reason[192];
  if (Http_ParsePinsArray(body, pins, CONFIG_GPIO_LOGICAL_COUNT, &count, reason, sizeof(reason)) != ESP_OK) {
    cJSON_Delete(body);
    return Json_SendError(req, 400, reason);
  }
  cJSON_Delete(body);

  char lockId[64];
  for (size_t i = 0; i < count; i++) {
    int st = Http_LockStatus(req, pins[i], LOCK_METHOD_READ, lockId, sizeof(lockId), reason, sizeof(reason));
    if (st) return Json_SendError(req, st, reason);
  }

  cJSON* root = cJSON_CreateObject();
  cJSON* levels = cJSON_CreateArray();
  for (size_t i = 0; i < count; i++) {
    int level = 0;
    GpioCtrl_GetLevel(pins[i], &level);
    cJSON_AddItemToArray(levels, cJSON_CreateNumber(level));
  }
  cJSON_AddItemToObject(root, "levels", levels);
  cJSON_AddNumberToObject(root, "time", (double)Json_NowUs());
  Lock_Touch(lockId[0] ? lockId : NULL);
  return Json_SendJson(req, 200, root);
}

static esp_err_t Handler_BatchLevelPost(httpd_req_t* req)
{
  Lock_SweepExpired();
  if (!Http_HasJsonContentType(req)) {
    return Json_SendError(req, 415, "Content-Type must be application/json.");
  }
  esp_err_t perr = ESP_OK;
  cJSON* body = Json_ParseBody(req, &perr);
  if (body == NULL) return Json_SendError(req, 400, "invalid_json");

  int pins[CONFIG_GPIO_LOGICAL_COUNT];
  size_t count = 0;
  char reason[256];
  if (Http_ParsePinsArray(body, pins, CONFIG_GPIO_LOGICAL_COUNT, &count, reason, sizeof(reason)) != ESP_OK) {
    cJSON_Delete(body);
    return Json_SendError(req, 400, reason);
  }
  cJSON* levelItem = cJSON_GetObjectItem(body, "level");
  if (!cJSON_IsNumber(levelItem) || (levelItem->valueint != 0 && levelItem->valueint != 1)) {
    cJSON_Delete(body);
    return Json_SendError(req, 400, "level must be 0 or 1.");
  }
  int level = levelItem->valueint;
  cJSON_Delete(body);

  char lockId[64];
  for (size_t i = 0; i < count; i++) {
    int st = Http_LockStatus(req, pins[i], LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason, sizeof(reason));
    if (st) return Json_SendError(req, st, reason);
    if (!GpioCtrl_IsOutputCapable(pins[i])) {
      snprintf(reason, sizeof(reason),
               "Pin %d is configured as input, so level cannot be written. PUT /gpios/%d/config with mode output, "
               "output_open_drain, input_output, or input_output_open_drain first.",
               pins[i], pins[i]);
      return Json_SendError(req, 422, reason);
    }
  }
  for (size_t i = 0; i < count; i++) {
    if (GpioCtrl_SetLevel(pins[i], level) != ESP_OK) {
      return Json_SendError(req, 500, "internal");
    }
  }
  Lock_Touch(lockId[0] ? lockId : NULL);
  return Json_SendEmpty(req, 204);
}

static esp_err_t Handler_BatchConfigPut(httpd_req_t* req)
{
  Lock_SweepExpired();
  if (!Http_HasJsonContentType(req)) {
    return Json_SendError(req, 415, "Content-Type must be application/json.");
  }
  esp_err_t perr = ESP_OK;
  cJSON* body = Json_ParseBody(req, &perr);
  if (body == NULL) return Json_SendError(req, 400, "invalid_json");

  int pins[CONFIG_GPIO_LOGICAL_COUNT];
  size_t count = 0;
  char reason[256];
  if (Http_ParsePinsArray(body, pins, CONFIG_GPIO_LOGICAL_COUNT, &count, reason, sizeof(reason)) != ESP_OK) {
    cJSON_Delete(body);
    return Json_SendError(req, 400, reason);
  }
  cJSON* modeItem = cJSON_GetObjectItem(body, "mode");
  cJSON* pullUpItem = cJSON_GetObjectItem(body, "pullUp");
  cJSON* pullDownItem = cJSON_GetObjectItem(body, "pullDown");
  GpioCtrl_Mode mode;
  if (!cJSON_IsString(modeItem) || !GpioCtrl_ModeFromString(modeItem->valuestring, &mode)) {
    cJSON_Delete(body);
    return Json_SendError(req, 400,
                          "mode is invalid. Use one of: disable, input, output, output_open_drain, input_output, "
                          "input_output_open_drain.");
  }
  if (!cJSON_IsBool(pullUpItem) || !cJSON_IsBool(pullDownItem)) {
    cJSON_Delete(body);
    return Json_SendError(req, 400, "pullUp and pullDown must be boolean.");
  }
  bool pullUp = cJSON_IsTrue(pullUpItem);
  bool pullDown = cJSON_IsTrue(pullDownItem);
  cJSON_Delete(body);

  char lockId[64];
  for (size_t i = 0; i < count; i++) {
    int st = Http_LockStatus(req, pins[i], LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason, sizeof(reason));
    if (st) return Json_SendError(req, st, reason);
  }
  for (size_t i = 0; i < count; i++) {
    if (GpioCtrl_SetConfig(pins[i], mode, pullUp, pullDown) != ESP_OK) {
      return Json_SendError(req, 500, "internal");
    }
  }
  Lock_Touch(lockId[0] ? lockId : NULL);
  return Json_SendEmpty(req, 204);
}

static esp_err_t Handler_BatchPulsePost(httpd_req_t* req)
{
  Lock_SweepExpired();
  if (!Http_HasJsonContentType(req)) {
    return Json_SendError(req, 415, "Content-Type must be application/json.");
  }
  esp_err_t perr = ESP_OK;
  cJSON* body = Json_ParseBody(req, &perr);
  if (body == NULL) return Json_SendError(req, 400, "invalid_json");

  int pins[CONFIG_GPIO_LOGICAL_COUNT];
  size_t count = 0;
  char reason[256];
  if (Http_ParsePinsArray(body, pins, CONFIG_GPIO_LOGICAL_COUNT, &count, reason, sizeof(reason)) != ESP_OK) {
    cJSON_Delete(body);
    return Json_SendError(req, 400, reason);
  }
  cJSON* widthItem = cJSON_GetObjectItem(body, "width");
  cJSON* levelItem = cJSON_GetObjectItem(body, "level");
  if (!cJSON_IsNumber(widthItem) || widthItem->valuedouble < 1 ||
      widthItem->valuedouble > (double)CONFIG_PULSE_MAX_WIDTH_US) {
    cJSON_Delete(body);
    return Json_SendError(req, 400, "width must be an integer from 1 to 1000000 microseconds.");
  }
  if (!cJSON_IsNumber(levelItem) || (levelItem->valueint != 0 && levelItem->valueint != 1)) {
    cJSON_Delete(body);
    return Json_SendError(req, 400, "level must be 0 or 1.");
  }
  uint64_t width = (uint64_t)widthItem->valuedouble;
  int level = levelItem->valueint;
  cJSON_Delete(body);

  char lockId[64];
  for (size_t i = 0; i < count; i++) {
    int st = Http_LockStatus(req, pins[i], LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason, sizeof(reason));
    if (st) return Json_SendError(req, st, reason);
    if (!GpioCtrl_IsOutputCapable(pins[i])) {
      snprintf(reason, sizeof(reason),
               "Pin %d is configured as input, so level cannot be written. PUT /gpios/%d/config with mode output, "
               "output_open_drain, input_output, or input_output_open_drain first.",
               pins[i], pins[i]);
      return Json_SendError(req, 422, reason);
    }
  }
  for (size_t i = 0; i < count; i++) {
    if (GpioCtrl_Pulse(pins[i], level, width) != ESP_OK) {
      return Json_SendError(req, 500, "internal");
    }
  }
  Lock_Touch(lockId[0] ? lockId : NULL);
  return Json_SendEmpty(req, 204);
}

static esp_err_t Handler_BatchTraceGet(httpd_req_t* req)
{
  Lock_SweepExpired();
  if (!Http_HasJsonContentType(req)) {
    return Json_SendError(req, 415, "Content-Type must be application/json.");
  }
  esp_err_t perr = ESP_OK;
  cJSON* body = Json_ParseBody(req, &perr);
  if (body == NULL) return Json_SendError(req, 400, "invalid_json");

  int pins[CONFIG_GPIO_LOGICAL_COUNT];
  size_t count = 0;
  char reason[256];
  if (Http_ParsePinsArray(body, pins, CONFIG_GPIO_LOGICAL_COUNT, &count, reason, sizeof(reason)) != ESP_OK) {
    cJSON_Delete(body);
    return Json_SendError(req, 400, reason);
  }
  cJSON* edgeItem = cJSON_GetObjectItem(body, "edge");
  cJSON* durationItem = cJSON_GetObjectItem(body, "duration");
  GpioCtrl_Edge edge = GPIO_CTRL_EDGE_BOTH;
  if (cJSON_IsString(edgeItem)) {
    if (!GpioCtrl_EdgeFromString(edgeItem->valuestring, &edge)) {
      cJSON_Delete(body);
      return Json_SendError(req, 400, "edge is invalid. Use one of: raising, falling, both.");
    }
  }
  uint64_t duration = CONFIG_TRACE_DEFAULT_DURATION_US;
  if (cJSON_IsNumber(durationItem)) {
    duration = (uint64_t)durationItem->valuedouble;
  }
  cJSON_Delete(body);
  if (duration < 1 || duration > CONFIG_TRACE_MAX_DURATION_US) {
    return Json_SendError(req, 400, "duration must be an integer from 1 to 60000000 microseconds (max 60 seconds).");
  }

  char lockId[64];
  for (size_t i = 0; i < count; i++) {
    int st = Http_LockStatus(req, pins[i], LOCK_METHOD_READ | LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason,
                             sizeof(reason));
    if (st) return Json_SendError(req, st, reason);
  }

  GpioCtrl_TraceEvent* events = calloc(CONFIG_TRACE_MAX_EVENTS, sizeof(GpioCtrl_TraceEvent));
  if (events == NULL) return Json_SendError(req, 500, "internal");
  size_t eventCount = 0;
  if (GpioCtrl_Trace(pins, count, edge, duration, events, CONFIG_TRACE_MAX_EVENTS, &eventCount, true) != ESP_OK) {
    free(events);
    return Json_SendError(req, 500, "internal");
  }
  cJSON* root = cJSON_CreateObject();
  cJSON* arr = cJSON_CreateArray();
  for (size_t i = 0; i < eventCount; i++) {
    cJSON* ev = cJSON_CreateObject();
    cJSON_AddNumberToObject(ev, "pin", events[i].pin);
    cJSON_AddStringToObject(ev, "edge", events[i].edge);
    cJSON_AddNumberToObject(ev, "level", events[i].level);
    cJSON_AddNumberToObject(ev, "time", (double)events[i].time);
    cJSON_AddItemToArray(arr, ev);
  }
  cJSON_AddItemToObject(root, "events", arr);
  free(events);
  Lock_Touch(lockId[0] ? lockId : NULL);
  return Json_SendJson(req, 200, root);
}

static esp_err_t Handler_PostLocks(httpd_req_t* req)
{
  Lock_SweepExpired();
  if (!Http_HasJsonContentType(req)) {
    return Json_SendError(req, 415, "Content-Type must be application/json.");
  }
  esp_err_t perr = ESP_OK;
  cJSON* body = Json_ParseBody(req, &perr);
  if (body == NULL) return Json_SendError(req, 400, "invalid_json");

  cJSON* resources = cJSON_GetObjectItem(body, "resources");
  if (!cJSON_IsArray(resources) || cJSON_GetArraySize(resources) == 0) {
    cJSON_Delete(body);
    return Json_SendError(req, 400, "resources must be a non-empty array.");
  }

  Lock_Resource res[CONFIG_LOCK_MAX_RESOURCES];
  size_t count = (size_t)cJSON_GetArraySize(resources);
  if (count > CONFIG_LOCK_MAX_RESOURCES) {
    cJSON_Delete(body);
    return Json_SendError(req, 400, "resources array is too long.");
  }

  for (size_t i = 0; i < count; i++) {
    cJSON* item = cJSON_GetArrayItem(resources, (int)i);
    cJSON* pinItem = cJSON_GetObjectItem(item, "pin");
    cJSON* methods = cJSON_GetObjectItem(item, "method");
    if (!cJSON_IsNumber(pinItem) || !GpioCtrl_IsValidLogicalPin(pinItem->valueint)) {
      cJSON_Delete(body);
      char reason[96];
      snprintf(reason, sizeof(reason), "resources[%u].pin is invalid. Use a pin from 0 to 7.", (unsigned)i);
      return Json_SendError(req, 400, reason);
    }
    if (!cJSON_IsArray(methods) || cJSON_GetArraySize(methods) == 0) {
      cJSON_Delete(body);
      return Json_SendError(req, 400, "Each resource.method entry must be read or write.");
    }
    uint8_t bits = 0;
    for (int m = 0; m < cJSON_GetArraySize(methods); m++) {
      cJSON* mv = cJSON_GetArrayItem(methods, m);
      if (!cJSON_IsString(mv)) {
        cJSON_Delete(body);
        return Json_SendError(req, 400, "Each resource.method entry must be read or write.");
      }
      if (strcmp(mv->valuestring, "read") == 0) bits |= LOCK_METHOD_READ;
      else if (strcmp(mv->valuestring, "write") == 0)
        bits |= LOCK_METHOD_WRITE;
      else {
        cJSON_Delete(body);
        return Json_SendError(req, 400, "method is invalid. Each resource.method entry must be read or write.");
      }
    }
    res[i].pin = pinItem->valueint;
    res[i].methods = bits;
  }
  cJSON_Delete(body);

  Lock_Entry created;
  esp_err_t cret = Lock_Create(res, count, &created);
  if (cret == ESP_ERR_INVALID_STATE) {
    char reason[160];
    snprintf(reason, sizeof(reason),
             "Cannot create lock: pin %d write is already held. DELETE that lock or wait until it expires.",
             res[0].pin);
    /* find first conflicting pin/method for better message */
    for (size_t i = 0; i < count; i++) {
      snprintf(reason, sizeof(reason),
               "Cannot create lock: pin %d is already held. DELETE that lock or wait until it expires.", res[i].pin);
      break;
    }
    return Json_SendError(req, 409, reason);
  }
  if (cret != ESP_OK) {
    return Json_SendError(req, 500, "internal");
  }

  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "id", created.id);
  int64_t now = Lock_NowUs();
  cJSON_AddNumberToObject(root, "ttl", (double)(created.expiresAtUs - now));
  cJSON_AddNumberToObject(root, "expiresAt", (double)created.expiresAtUs);
  cJSON* resArr = cJSON_CreateArray();
  for (size_t i = 0; i < created.resourceCount; i++) {
    cJSON* r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "pin", created.resources[i].pin);
    cJSON* methods = cJSON_CreateArray();
    if (created.resources[i].methods & LOCK_METHOD_READ) cJSON_AddItemToArray(methods, cJSON_CreateString("read"));
    if (created.resources[i].methods & LOCK_METHOD_WRITE) cJSON_AddItemToArray(methods, cJSON_CreateString("write"));
    cJSON_AddItemToObject(r, "method", methods);
    cJSON_AddItemToArray(resArr, r);
  }
  cJSON_AddItemToObject(root, "resources", resArr);
  return Json_SendJson(req, 201, root);
}

static esp_err_t Handler_PutLock(httpd_req_t* req)
{
  Lock_SweepExpired();
  const char* prefix = "/locks/";
  if (strncmp(req->uri, prefix, strlen(prefix)) != 0) {
    return Json_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  const char* id = req->uri + strlen(prefix);
  if (id[0] == '\0') {
    return Json_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  Lock_Entry entry;
  if (Lock_Renew(id, &entry) != ESP_OK) {
    char reason[160];
    snprintf(reason, sizeof(reason),
             "Lock id %s does not exist. Create a lock with POST /locks (see GET /openapi.json).", id);
    return Json_SendError(req, 404, reason);
  }
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "id", entry.id);
  int64_t now = Lock_NowUs();
  cJSON_AddNumberToObject(root, "ttl", (double)(entry.expiresAtUs - now));
  cJSON_AddNumberToObject(root, "expiresAt", (double)entry.expiresAtUs);
  return Json_SendJson(req, 200, root);
}

static esp_err_t Handler_DeleteLock(httpd_req_t* req)
{
  Lock_SweepExpired();
  const char* prefix = "/locks/";
  if (strncmp(req->uri, prefix, strlen(prefix)) != 0) {
    return Json_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  const char* id = req->uri + strlen(prefix);
  Lock_Delete(id); /* always 204 */
  return Json_SendEmpty(req, 204);
}

static esp_err_t Handler_NotFound(httpd_req_t* req, httpd_err_code_t err)
{
  (void)err;
  return Json_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
}

static httpd_uri_t s_uris[] = {
    {.uri = "/openapi.json", .method = HTTP_GET, .handler = Handler_OpenApi},
    {.uri = "/gpios/", .method = HTTP_GET, .handler = Handler_ListGpios},
    {.uri = "/gpios/*/config", .method = HTTP_PUT, .handler = Handler_PutConfigSingle},
    {.uri = "/gpios/*/level", .method = HTTP_GET, .handler = Handler_GetLevelSingle},
    {.uri = "/gpios/*/level", .method = HTTP_POST, .handler = Handler_PostLevelSingle},
    {.uri = "/gpios/*/pulse", .method = HTTP_POST, .handler = Handler_PostPulseSingle},
    {.uri = "/gpios/*/trace", .method = HTTP_GET, .handler = Handler_GetTraceSingle},
    {.uri = "/gpios/level", .method = HTTP_GET, .handler = Handler_BatchLevelGet},
    {.uri = "/gpios/level", .method = HTTP_POST, .handler = Handler_BatchLevelPost},
    {.uri = "/gpios/config", .method = HTTP_PUT, .handler = Handler_BatchConfigPut},
    {.uri = "/gpios/pulse", .method = HTTP_POST, .handler = Handler_BatchPulsePost},
    {.uri = "/gpios/trace", .method = HTTP_GET, .handler = Handler_BatchTraceGet},
    {.uri = "/locks", .method = HTTP_POST, .handler = Handler_PostLocks},
    {.uri = "/locks/*", .method = HTTP_PUT, .handler = Handler_PutLock},
    {.uri = "/locks/*", .method = HTTP_DELETE, .handler = Handler_DeleteLock},
};

esp_err_t HttpServer_Init(void)
{
  return ESP_OK;
}

esp_err_t HttpServer_Stop(void)
{
  if (s_server) {
    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(tag, "HTTP server stopped");
  }
  return ESP_OK;
}

esp_err_t HttpServer_Start(void)
{
  if (s_server) {
    return ESP_OK;
  }
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = CONFIG_HTTP_PORT;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.max_uri_handlers = 24;
  config.lru_purge_enable = true;
  config.recv_wait_timeout = 65;
  config.send_wait_timeout = 65;

  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(httpd_start(&s_server, &config), "httpd_start failed");
  for (size_t i = 0; i < TOOL_GET_ARRAY_LENGTH(s_uris); i++) {
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(httpd_register_uri_handler(s_server, &s_uris[i]), "register uri failed");
  }
  httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, Handler_NotFound);
  ESP_LOGI(tag, "HTTP server started on port %d", CONFIG_HTTP_PORT);
  return ESP_OK;
}
