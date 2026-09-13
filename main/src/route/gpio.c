/**
 * @name GPIO routes
 * @file gpio.c
 * @author xqe2011
 */
#include "route.h"

#include "config.h"
#include "gpio_ctrl.h"
#include "http_server.h"
#include "lock.h"
#include "tool.h"

#include <cJSON.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* tag = "SAIHUB-Http";

#define TRACE_DEFAULT_DURATION_US 1000000ULL

static esp_err_t Route_GpioListHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Lock_SweepExpired();
  cJSON* root = cJSON_CreateObject();
  cJSON* gpios = cJSON_CreateArray();
  int count = GpioCtrl_GetLogicalCount();
  for (int i = 0; i < count; i++) {
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
  cJSON_AddNumberToObject(root, "time", (double)HttpServer_NowUs());
  return HttpServer_SendJson(req, 200, root);
}

static esp_err_t Route_GpioPutConfigHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Lock_SweepExpired();
  int pin = 0;
  int pr = HttpServer_ParsePathPin(req->uri, "/gpios/", "/config", &pin);
  if (pr == -2) {
    return HttpServer_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  if (pr == -3) {
    char reason[96];
    char range[32];
    HttpServer_FormatPinRange(range, sizeof(range));
    const char* p = req->uri + strlen("/gpios/");
    long raw = strtol(p, NULL, 10);
    snprintf(reason, sizeof(reason), "Pin %ld does not exist. Use a pin from %s.", raw, range);
    return HttpServer_SendError(req, 404, reason);
  }

  if (!HttpServer_HasJsonContentType(req)) {
    return HttpServer_SendError(req, 415, "Content-Type must be application/json.");
  }

  char lockId[64];
  char reason[256];
  int st = HttpServer_LockStatus(req, LOCK_KIND_GPIO, pin, LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return HttpServer_SendError(req, st, reason);

  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(req, &perr);
  if (body == NULL) {
    return HttpServer_SendError(req, 400, "invalid_json");
  }

  cJSON* modeItem = cJSON_GetObjectItem(body, "mode");
  cJSON* pullUpItem = cJSON_GetObjectItem(body, "pullUp");
  cJSON* pullDownItem = cJSON_GetObjectItem(body, "pullDown");
  GpioCtrl_Mode mode;
  if (!cJSON_IsString(modeItem) || !GpioCtrl_ModeFromString(modeItem->valuestring, &mode)) {
    cJSON_Delete(body);
    return HttpServer_SendError(req, 400,
                          "mode is invalid. Use one of: disable, input, output, output_open_drain, input_output, "
                          "input_output_open_drain.");
  }
  if (!cJSON_IsBool(pullUpItem) || !cJSON_IsBool(pullDownItem)) {
    cJSON_Delete(body);
    return HttpServer_SendError(req, 400, "pullUp and pullDown must be boolean.");
  }
  bool pullUp = cJSON_IsTrue(pullUpItem);
  bool pullDown = cJSON_IsTrue(pullDownItem);
  cJSON_Delete(body);

  if (GpioCtrl_SetConfig(pin, mode, pullUp, pullDown) != ESP_OK) {
    return HttpServer_SendError(req, 500, "internal");
  }
  Lock_Touch(lockId[0] ? lockId : NULL);
  return HttpServer_SendEmpty(req, 204);
}

static esp_err_t Route_GpioGetLevelHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Lock_SweepExpired();
  int pin = 0;
  int pr = HttpServer_ParsePathPin(req->uri, "/gpios/", "/level", &pin);
  if (pr == -2) {
    return HttpServer_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  if (pr == -3) {
    const char* p = req->uri + strlen("/gpios/");
    long raw = strtol(p, NULL, 10);
    char reason[96];
    char range[32];
    HttpServer_FormatPinRange(range, sizeof(range));
    snprintf(reason, sizeof(reason), "Pin %ld does not exist. Use a pin from %s.", raw, range);
    return HttpServer_SendError(req, 404, reason);
  }

  char lockId[64];
  char reason[256];
  int st = HttpServer_LockStatus(req, LOCK_KIND_GPIO, pin, LOCK_METHOD_READ, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return HttpServer_SendError(req, st, reason);

  int level = 0;
  if (GpioCtrl_GetLevel(pin, &level) != ESP_OK) {
    return HttpServer_SendError(req, 500, "internal");
  }
  Lock_Touch(lockId[0] ? lockId : NULL);
  cJSON* root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "level", level);
  cJSON_AddNumberToObject(root, "time", (double)HttpServer_NowUs());
  return HttpServer_SendJson(req, 200, root);
}

static esp_err_t Route_GpioPostLevelHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Lock_SweepExpired();
  int pin = 0;
  int pr = HttpServer_ParsePathPin(req->uri, "/gpios/", "/level", &pin);
  if (pr == -2) {
    return HttpServer_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  if (pr == -3) {
    const char* p = req->uri + strlen("/gpios/");
    long raw = strtol(p, NULL, 10);
    char reason[96];
    char range[32];
    HttpServer_FormatPinRange(range, sizeof(range));
    snprintf(reason, sizeof(reason), "Pin %ld does not exist. Use a pin from %s.", raw, range);
    return HttpServer_SendError(req, 404, reason);
  }

  if (!HttpServer_HasJsonContentType(req)) {
    return HttpServer_SendError(req, 415, "Content-Type must be application/json.");
  }

  char lockId[64];
  char reason[256];
  int st = HttpServer_LockStatus(req, LOCK_KIND_GPIO, pin, LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return HttpServer_SendError(req, st, reason);

  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(req, &perr);
  if (body == NULL) return HttpServer_SendError(req, 400, "invalid_json");

  cJSON* levelItem = cJSON_GetObjectItem(body, "level");
  if (!cJSON_IsNumber(levelItem) || (levelItem->valueint != 0 && levelItem->valueint != 1)) {
    cJSON_Delete(body);
    return HttpServer_SendError(req, 400, "level must be 0 or 1.");
  }
  int level = levelItem->valueint;
  cJSON_Delete(body);

  if (!GpioCtrl_IsOutputCapable(pin)) {
    snprintf(reason, sizeof(reason),
             "Pin %d is configured as input, so level cannot be written. PUT /gpios/%d/config with mode output, "
             "output_open_drain, input_output, or input_output_open_drain first.",
             pin, pin);
    return HttpServer_SendError(req, 422, reason);
  }
  if (GpioCtrl_SetLevel(pin, level) != ESP_OK) {
    return HttpServer_SendError(req, 500, "internal");
  }
  Lock_Touch(lockId[0] ? lockId : NULL);
  return HttpServer_SendEmpty(req, 204);
}

static esp_err_t Route_GpioPostPulseHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Lock_SweepExpired();
  int pin = 0;
  int pr = HttpServer_ParsePathPin(req->uri, "/gpios/", "/pulse", &pin);
  if (pr == -2) {
    return HttpServer_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  if (pr == -3) {
    const char* p = req->uri + strlen("/gpios/");
    long raw = strtol(p, NULL, 10);
    char reason[96];
    char range[32];
    HttpServer_FormatPinRange(range, sizeof(range));
    snprintf(reason, sizeof(reason), "Pin %ld does not exist. Use a pin from %s.", raw, range);
    return HttpServer_SendError(req, 404, reason);
  }
  if (!HttpServer_HasJsonContentType(req)) {
    return HttpServer_SendError(req, 415, "Content-Type must be application/json.");
  }

  char lockId[64];
  char reason[256];
  int st = HttpServer_LockStatus(req, LOCK_KIND_GPIO, pin, LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return HttpServer_SendError(req, st, reason);

  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(req, &perr);
  if (body == NULL) return HttpServer_SendError(req, 400, "invalid_json");

  cJSON* widthItem = cJSON_GetObjectItem(body, "width");
  cJSON* levelItem = cJSON_GetObjectItem(body, "level");
  if (!cJSON_IsNumber(widthItem) || widthItem->valuedouble < 1 ||
      widthItem->valuedouble > (double)CONFIG_GPIO_PULSE_MAX_WIDTH_US) {
    cJSON_Delete(body);
    return HttpServer_SendError(req, 400, "width must be an integer from 1 to 1000000 microseconds.");
  }
  if (!cJSON_IsNumber(levelItem) || (levelItem->valueint != 0 && levelItem->valueint != 1)) {
    cJSON_Delete(body);
    return HttpServer_SendError(req, 400, "level must be 0 or 1.");
  }
  uint64_t width = (uint64_t)widthItem->valuedouble;
  int level = levelItem->valueint;
  cJSON_Delete(body);

  if (!GpioCtrl_IsOutputCapable(pin)) {
    snprintf(reason, sizeof(reason),
             "Pin %d is configured as input, so level cannot be written. PUT /gpios/%d/config with mode output, "
             "output_open_drain, input_output, or input_output_open_drain first.",
             pin, pin);
    return HttpServer_SendError(req, 422, reason);
  }
  if (GpioCtrl_Pulse(pin, level, width) != ESP_OK) {
    return HttpServer_SendError(req, 500, "internal");
  }
  Lock_Touch(lockId[0] ? lockId : NULL);
  return HttpServer_SendEmpty(req, 204);
}

static esp_err_t Route_GpioGetTraceHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Lock_SweepExpired();
  int pin = 0;
  char path[128];
  strncpy(path, req->uri, sizeof(path) - 1);
  path[sizeof(path) - 1] = '\0';
  char* q = strchr(path, '?');
  if (q) *q = '\0';

  int pr = HttpServer_ParsePathPin(path, "/gpios/", "/trace", &pin);
  if (pr == -2) {
    return HttpServer_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  if (pr == -3) {
    const char* p = path + strlen("/gpios/");
    long raw = strtol(p, NULL, 10);
    char reason[96];
    char range[32];
    HttpServer_FormatPinRange(range, sizeof(range));
    snprintf(reason, sizeof(reason), "Pin %ld does not exist. Use a pin from %s.", raw, range);
    return HttpServer_SendError(req, 404, reason);
  }

  char lockId[64];
  char reason[256];
  int st = HttpServer_LockStatus(req, LOCK_KIND_GPIO, pin, LOCK_METHOD_READ | LOCK_METHOD_WRITE, lockId, sizeof(lockId),
                           reason, sizeof(reason));
  if (st) return HttpServer_SendError(req, st, reason);

  char edgeBuf[32] = {0};
  char durationBuf[32] = {0};
  httpd_req_get_url_query_str(req, reason, sizeof(reason));
  if (httpd_query_key_value(reason, "edge", edgeBuf, sizeof(edgeBuf)) != ESP_OK) {
    strcpy(edgeBuf, "both");
  }
  uint64_t duration = TRACE_DEFAULT_DURATION_US;
  if (httpd_query_key_value(reason, "duration", durationBuf, sizeof(durationBuf)) == ESP_OK) {
    duration = strtoull(durationBuf, NULL, 10);
  }
  GpioCtrl_Edge edge;
  if (!GpioCtrl_EdgeFromString(edgeBuf, &edge)) {
    return HttpServer_SendError(req, 400, "edge is invalid. Use one of: raising, falling, both.");
  }
  if (duration < 1 || duration > CONFIG_GPIO_TRACE_MAX_DURATION_US) {
    return HttpServer_SendError(req, 400, "duration must be an integer from 1 to 60000000 microseconds (max 60 seconds).");
  }

  GpioCtrl_TraceEvent* events = calloc(CONFIG_GPIO_TRACE_MAX_EVENTS, sizeof(GpioCtrl_TraceEvent));
  if (events == NULL) return HttpServer_SendError(req, 500, "internal");
  size_t count = 0;
  esp_err_t tr = GpioCtrl_Trace(&pin, 1, edge, duration, events, CONFIG_GPIO_TRACE_MAX_EVENTS, &count, false);
  if (tr != ESP_OK) {
    free(events);
    return HttpServer_SendError(req, 500, "internal");
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
  return HttpServer_SendJson(req, 200, root);
}

static esp_err_t Route_GpioBatchGetLevelHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Lock_SweepExpired();
  if (!HttpServer_HasJsonContentType(req)) {
    return HttpServer_SendError(req, 415, "Content-Type must be application/json.");
  }
  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(req, &perr);
  if (body == NULL) return HttpServer_SendError(req, 400, "invalid_json");

  int maxPins = GpioCtrl_GetLogicalCount();
  int pins[16];
  if (maxPins > (int)TOOL_GET_ARRAY_LENGTH(pins)) maxPins = (int)TOOL_GET_ARRAY_LENGTH(pins);
  size_t count = 0;
  char reason[192];
  if (HttpServer_ParsePinsArray(body, pins, (size_t)maxPins, &count, reason, sizeof(reason)) != ESP_OK) {
    cJSON_Delete(body);
    return HttpServer_SendError(req, 400, reason);
  }
  cJSON_Delete(body);

  char lockId[64];
  for (size_t i = 0; i < count; i++) {
    int st =
        HttpServer_LockStatus(req, LOCK_KIND_GPIO, pins[i], LOCK_METHOD_READ, lockId, sizeof(lockId), reason, sizeof(reason));
    if (st) return HttpServer_SendError(req, st, reason);
  }

  cJSON* root = cJSON_CreateObject();
  cJSON* levels = cJSON_CreateArray();
  for (size_t i = 0; i < count; i++) {
    int level = 0;
    GpioCtrl_GetLevel(pins[i], &level);
    cJSON_AddItemToArray(levels, cJSON_CreateNumber(level));
  }
  cJSON_AddItemToObject(root, "levels", levels);
  cJSON_AddNumberToObject(root, "time", (double)HttpServer_NowUs());
  Lock_Touch(lockId[0] ? lockId : NULL);
  return HttpServer_SendJson(req, 200, root);
}

static esp_err_t Route_GpioBatchPostLevelHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Lock_SweepExpired();
  if (!HttpServer_HasJsonContentType(req)) {
    return HttpServer_SendError(req, 415, "Content-Type must be application/json.");
  }
  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(req, &perr);
  if (body == NULL) return HttpServer_SendError(req, 400, "invalid_json");

  int maxPins = GpioCtrl_GetLogicalCount();
  int pins[16];
  if (maxPins > (int)TOOL_GET_ARRAY_LENGTH(pins)) maxPins = (int)TOOL_GET_ARRAY_LENGTH(pins);
  size_t count = 0;
  char reason[256];
  if (HttpServer_ParsePinsArray(body, pins, (size_t)maxPins, &count, reason, sizeof(reason)) != ESP_OK) {
    cJSON_Delete(body);
    return HttpServer_SendError(req, 400, reason);
  }
  cJSON* levelItem = cJSON_GetObjectItem(body, "level");
  if (!cJSON_IsNumber(levelItem) || (levelItem->valueint != 0 && levelItem->valueint != 1)) {
    cJSON_Delete(body);
    return HttpServer_SendError(req, 400, "level must be 0 or 1.");
  }
  int level = levelItem->valueint;
  cJSON_Delete(body);

  char lockId[64];
  for (size_t i = 0; i < count; i++) {
    int st = HttpServer_LockStatus(req, LOCK_KIND_GPIO, pins[i], LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason,
                             sizeof(reason));
    if (st) return HttpServer_SendError(req, st, reason);
    if (!GpioCtrl_IsOutputCapable(pins[i])) {
      snprintf(reason, sizeof(reason),
               "Pin %d is configured as input, so level cannot be written. PUT /gpios/%d/config with mode output, "
               "output_open_drain, input_output, or input_output_open_drain first.",
               pins[i], pins[i]);
      return HttpServer_SendError(req, 422, reason);
    }
  }
  for (size_t i = 0; i < count; i++) {
    if (GpioCtrl_SetLevel(pins[i], level) != ESP_OK) {
      return HttpServer_SendError(req, 500, "internal");
    }
  }
  Lock_Touch(lockId[0] ? lockId : NULL);
  return HttpServer_SendEmpty(req, 204);
}

static esp_err_t Route_GpioBatchPutConfigHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Lock_SweepExpired();
  if (!HttpServer_HasJsonContentType(req)) {
    return HttpServer_SendError(req, 415, "Content-Type must be application/json.");
  }
  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(req, &perr);
  if (body == NULL) return HttpServer_SendError(req, 400, "invalid_json");

  int maxPins = GpioCtrl_GetLogicalCount();
  int pins[16];
  if (maxPins > (int)TOOL_GET_ARRAY_LENGTH(pins)) maxPins = (int)TOOL_GET_ARRAY_LENGTH(pins);
  size_t count = 0;
  char reason[256];
  if (HttpServer_ParsePinsArray(body, pins, (size_t)maxPins, &count, reason, sizeof(reason)) != ESP_OK) {
    cJSON_Delete(body);
    return HttpServer_SendError(req, 400, reason);
  }
  cJSON* modeItem = cJSON_GetObjectItem(body, "mode");
  cJSON* pullUpItem = cJSON_GetObjectItem(body, "pullUp");
  cJSON* pullDownItem = cJSON_GetObjectItem(body, "pullDown");
  GpioCtrl_Mode mode;
  if (!cJSON_IsString(modeItem) || !GpioCtrl_ModeFromString(modeItem->valuestring, &mode)) {
    cJSON_Delete(body);
    return HttpServer_SendError(req, 400,
                          "mode is invalid. Use one of: disable, input, output, output_open_drain, input_output, "
                          "input_output_open_drain.");
  }
  if (!cJSON_IsBool(pullUpItem) || !cJSON_IsBool(pullDownItem)) {
    cJSON_Delete(body);
    return HttpServer_SendError(req, 400, "pullUp and pullDown must be boolean.");
  }
  bool pullUp = cJSON_IsTrue(pullUpItem);
  bool pullDown = cJSON_IsTrue(pullDownItem);
  cJSON_Delete(body);

  char lockId[64];
  for (size_t i = 0; i < count; i++) {
    int st = HttpServer_LockStatus(req, LOCK_KIND_GPIO, pins[i], LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason,
                             sizeof(reason));
    if (st) return HttpServer_SendError(req, st, reason);
  }
  for (size_t i = 0; i < count; i++) {
    if (GpioCtrl_SetConfig(pins[i], mode, pullUp, pullDown) != ESP_OK) {
      return HttpServer_SendError(req, 500, "internal");
    }
  }
  Lock_Touch(lockId[0] ? lockId : NULL);
  return HttpServer_SendEmpty(req, 204);
}

static esp_err_t Route_GpioBatchPostPulseHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Lock_SweepExpired();
  if (!HttpServer_HasJsonContentType(req)) {
    return HttpServer_SendError(req, 415, "Content-Type must be application/json.");
  }
  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(req, &perr);
  if (body == NULL) return HttpServer_SendError(req, 400, "invalid_json");

  int maxPins = GpioCtrl_GetLogicalCount();
  int pins[16];
  if (maxPins > (int)TOOL_GET_ARRAY_LENGTH(pins)) maxPins = (int)TOOL_GET_ARRAY_LENGTH(pins);
  size_t count = 0;
  char reason[256];
  if (HttpServer_ParsePinsArray(body, pins, (size_t)maxPins, &count, reason, sizeof(reason)) != ESP_OK) {
    cJSON_Delete(body);
    return HttpServer_SendError(req, 400, reason);
  }
  cJSON* widthItem = cJSON_GetObjectItem(body, "width");
  cJSON* levelItem = cJSON_GetObjectItem(body, "level");
  if (!cJSON_IsNumber(widthItem) || widthItem->valuedouble < 1 ||
      widthItem->valuedouble > (double)CONFIG_GPIO_PULSE_MAX_WIDTH_US) {
    cJSON_Delete(body);
    return HttpServer_SendError(req, 400, "width must be an integer from 1 to 1000000 microseconds.");
  }
  if (!cJSON_IsNumber(levelItem) || (levelItem->valueint != 0 && levelItem->valueint != 1)) {
    cJSON_Delete(body);
    return HttpServer_SendError(req, 400, "level must be 0 or 1.");
  }
  uint64_t width = (uint64_t)widthItem->valuedouble;
  int level = levelItem->valueint;
  cJSON_Delete(body);

  char lockId[64];
  for (size_t i = 0; i < count; i++) {
    int st = HttpServer_LockStatus(req, LOCK_KIND_GPIO, pins[i], LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason,
                             sizeof(reason));
    if (st) return HttpServer_SendError(req, st, reason);
    if (!GpioCtrl_IsOutputCapable(pins[i])) {
      snprintf(reason, sizeof(reason),
               "Pin %d is configured as input, so level cannot be written. PUT /gpios/%d/config with mode output, "
               "output_open_drain, input_output, or input_output_open_drain first.",
               pins[i], pins[i]);
      return HttpServer_SendError(req, 422, reason);
    }
  }
  for (size_t i = 0; i < count; i++) {
    if (GpioCtrl_Pulse(pins[i], level, width) != ESP_OK) {
      return HttpServer_SendError(req, 500, "internal");
    }
  }
  Lock_Touch(lockId[0] ? lockId : NULL);
  return HttpServer_SendEmpty(req, 204);
}

static esp_err_t Route_GpioBatchGetTraceHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Lock_SweepExpired();
  if (!HttpServer_HasJsonContentType(req)) {
    return HttpServer_SendError(req, 415, "Content-Type must be application/json.");
  }
  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(req, &perr);
  if (body == NULL) return HttpServer_SendError(req, 400, "invalid_json");

  int maxPins = GpioCtrl_GetLogicalCount();
  int pins[16];
  if (maxPins > (int)TOOL_GET_ARRAY_LENGTH(pins)) maxPins = (int)TOOL_GET_ARRAY_LENGTH(pins);
  size_t count = 0;
  char reason[256];
  if (HttpServer_ParsePinsArray(body, pins, (size_t)maxPins, &count, reason, sizeof(reason)) != ESP_OK) {
    cJSON_Delete(body);
    return HttpServer_SendError(req, 400, reason);
  }
  cJSON* edgeItem = cJSON_GetObjectItem(body, "edge");
  cJSON* durationItem = cJSON_GetObjectItem(body, "duration");
  GpioCtrl_Edge edge = GPIO_CTRL_EDGE_BOTH;
  if (cJSON_IsString(edgeItem)) {
    if (!GpioCtrl_EdgeFromString(edgeItem->valuestring, &edge)) {
      cJSON_Delete(body);
      return HttpServer_SendError(req, 400, "edge is invalid. Use one of: raising, falling, both.");
    }
  }
  uint64_t duration = TRACE_DEFAULT_DURATION_US;
  if (cJSON_IsNumber(durationItem)) {
    duration = (uint64_t)durationItem->valuedouble;
  }
  cJSON_Delete(body);
  if (duration < 1 || duration > CONFIG_GPIO_TRACE_MAX_DURATION_US) {
    return HttpServer_SendError(req, 400, "duration must be an integer from 1 to 60000000 microseconds (max 60 seconds).");
  }

  char lockId[64];
  for (size_t i = 0; i < count; i++) {
    int st = HttpServer_LockStatus(req, LOCK_KIND_GPIO, pins[i], LOCK_METHOD_READ | LOCK_METHOD_WRITE, lockId, sizeof(lockId),
                             reason, sizeof(reason));
    if (st) return HttpServer_SendError(req, st, reason);
  }

  GpioCtrl_TraceEvent* events = calloc(CONFIG_GPIO_TRACE_MAX_EVENTS, sizeof(GpioCtrl_TraceEvent));
  if (events == NULL) return HttpServer_SendError(req, 500, "internal");
  size_t eventCount = 0;
  if (GpioCtrl_Trace(pins, count, edge, duration, events, CONFIG_GPIO_TRACE_MAX_EVENTS, &eventCount, true) != ESP_OK) {
    free(events);
    return HttpServer_SendError(req, 500, "internal");
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
  return HttpServer_SendJson(req, 200, root);
}

static const httpd_uri_t uris[] = {
    {.uri = "/gpios/", .method = HTTP_GET, .handler = Route_GpioListHandler},
    {.uri = "/gpios/*/config", .method = HTTP_PUT, .handler = Route_GpioPutConfigHandler},
    {.uri = "/gpios/*/level", .method = HTTP_GET, .handler = Route_GpioGetLevelHandler},
    {.uri = "/gpios/*/level", .method = HTTP_POST, .handler = Route_GpioPostLevelHandler},
    {.uri = "/gpios/*/pulse", .method = HTTP_POST, .handler = Route_GpioPostPulseHandler},
    {.uri = "/gpios/*/trace", .method = HTTP_GET, .handler = Route_GpioGetTraceHandler},
    {.uri = "/gpios/level", .method = HTTP_GET, .handler = Route_GpioBatchGetLevelHandler},
    {.uri = "/gpios/level", .method = HTTP_POST, .handler = Route_GpioBatchPostLevelHandler},
    {.uri = "/gpios/config", .method = HTTP_PUT, .handler = Route_GpioBatchPutConfigHandler},
    {.uri = "/gpios/pulse", .method = HTTP_POST, .handler = Route_GpioBatchPostPulseHandler},
    {.uri = "/gpios/trace", .method = HTTP_GET, .handler = Route_GpioBatchGetTraceHandler},
};

esp_err_t Route_GpioRegister(httpd_handle_t server)
{
  for (size_t i = 0; i < TOOL_GET_ARRAY_LENGTH(uris); i++) {
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(httpd_register_uri_handler(server, &uris[i]), "register gpio uri failed");
  }
  return ESP_OK;
}
