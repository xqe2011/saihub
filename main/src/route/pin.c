/**
 * @name Pin routes
 * @file pin.c
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
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* tag = "SAIHUB-Http";

#define TRACE_DEFAULT_DURATION_US 1000000ULL

static const int pinLogicalToHw[] = CONFIG_GPIO_LOGICAL_TO_HW;
static esp_err_t Route_PinSendTraceEvents(httpd_req_t* req, const GpioCtrl_TraceEvent* events, size_t count, bool includePin);

typedef struct { httpd_req_t* req; int* pins; size_t pinCount; GpioCtrl_Edge edge; uint64_t duration; bool includePin; char lockId[64]; } PinTraceJob;
static void Route_PinTraceTask(void* arg) {
  PinTraceJob* j = arg; GpioCtrl_TraceEvent* ev = calloc(CONFIG_GPIO_TRACE_MAX_EVENTS, sizeof(*ev)); size_t n=0;
  esp_err_t r = ev ? GpioCtrl_Trace(j->pins,j->pinCount,j->edge,j->duration,ev,CONFIG_GPIO_TRACE_MAX_EVENTS,&n,j->includePin) : ESP_ERR_NO_MEM;
  if (r == ESP_OK) { Lock_Touch(j->lockId[0] ? j->lockId : NULL); Route_PinSendTraceEvents(j->req,ev,n,j->includePin); }
  else if (r == GPIO_CTRL_ERR_TRACE_BUSY) HttpServer_SendError(j->req,409,"One or more pins already have an active trace request.");
  else HttpServer_SendError(j->req,500,"internal");
  free(ev); free(j->pins); httpd_req_async_handler_complete(j->req); free(j); vTaskDelete(NULL);
}
static esp_err_t Route_PinStartTrace(httpd_req_t* req,const int* pins,size_t count,GpioCtrl_Edge edge,uint64_t duration,bool includePin,const char* lockId) {
  PinTraceJob* j=calloc(1,sizeof(*j)); if(!j) return HttpServer_SendError(req,500,"internal");
  j->pins=malloc(count*sizeof(int)); if(!j->pins){free(j);return HttpServer_SendError(req,500,"internal");} memcpy(j->pins,pins,count*sizeof(int)); j->pinCount=count;j->edge=edge;j->duration=duration;j->includePin=includePin; snprintf(j->lockId,sizeof(j->lockId),"%s",lockId?lockId:"");
  if(httpd_req_async_handler_begin(req,&j->req)!=ESP_OK){free(j->pins);free(j);return HttpServer_SendError(req,500,"internal");}
  if(xTaskCreate(Route_PinTraceTask,"pin_trace",4096,j,5,NULL)!=pdPASS){ HttpServer_SendError(j->req,500,"internal"); httpd_req_async_handler_complete(j->req); free(j->pins); free(j); return ESP_OK; } return ESP_OK;
}

static esp_err_t Route_PinSendTraceEvents(httpd_req_t* req, const GpioCtrl_TraceEvent* events, size_t count, bool includePin)
{
  HttpServer_SetCors(req);
  httpd_resp_set_status(req, "200 OK");
  httpd_resp_set_type(req, "application/json");
  esp_err_t ret = httpd_resp_send_chunk(req, "{\"events\":[", HTTPD_RESP_USE_STRLEN);
  char item[128];
  for (size_t i = 0; ret == ESP_OK && i < count; i++) {
    int len = includePin
                  ? snprintf(item, sizeof(item), "%s{\"pin\":%d,\"edge\":\"%s\",\"level\":%d,\"time\":%lld}",
                             i == 0 ? "" : ",", events[i].pin, events[i].edge, events[i].level,
                             (long long)events[i].time)
                  : snprintf(item, sizeof(item), "%s{\"edge\":\"%s\",\"level\":%d,\"time\":%lld}",
                             i == 0 ? "" : ",", events[i].edge, events[i].level, (long long)events[i].time);
    if (len < 0 || (size_t)len >= sizeof(item)) {
      ret = ESP_ERR_INVALID_SIZE;
      break;
    }
    ret = httpd_resp_send_chunk(req, item, (size_t)len);
  }
  if (ret == ESP_OK) ret = httpd_resp_send_chunk(req, "]}", 2);
  esp_err_t endRet = httpd_resp_send_chunk(req, NULL, 0);
  ESP_LOGI(tag, "Resp %s %s -> 200 trace_events=%u", HttpServer_MethodName(req->method), req->uri, (unsigned)count);
  return ret == ESP_OK ? endRet : ret;
}

static void Route_PinLevelWriteDenied(int pin, char* reason, size_t reasonLen)
{
  snprintf(reason, reasonLen,
           "Pin %d cannot write level in its current mode. PUT /pin/%d with mode digitalOutput or "
           "digitalInputOutput first.",
           pin, pin);
}

static void Route_PinPwmDenied(int pin, char* reason, size_t reasonLen)
{
  snprintf(reason, reasonLen, "Pin %d is not in pwmOutput mode. PUT /pin/%d with mode pwmOutput first.", pin, pin);
}

static esp_err_t Route_PinListHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  TOOL_CALL_LOG("rest %s %s", HttpServer_MethodName(req->method), req->uri);
  Lock_SweepExpired();
  cJSON* root = cJSON_CreateObject();
  cJSON* pinsArr = cJSON_CreateArray();
  int count = GpioCtrl_GetLogicalCount();
  for (int i = 0; i < count; i++) {
    GpioCtrl_State st;
    GpioCtrl_GetState(i, &st);
    cJSON* item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "pin", i);
    cJSON_AddStringToObject(item, "mode", GpioCtrl_ModeToString(st.mode));
    cJSON_AddBoolToObject(item, "openDrain", st.openDrain);
    cJSON_AddBoolToObject(item, "pullUp", st.pullUp);
    cJSON_AddBoolToObject(item, "pullDown", st.pullDown);
    cJSON_AddNumberToObject(item, "level", st.level);
    cJSON_AddItemToArray(pinsArr, item);
  }
  cJSON_AddItemToObject(root, "pins", pinsArr);
  cJSON_AddNumberToObject(root, "time", (double)HttpServer_NowUs());
  return HttpServer_SendJson(req, 200, root);
}

static esp_err_t Route_PinPutModeHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Lock_SweepExpired();
  int pin = 0;
  int pr = HttpServer_ParsePathPin(req->uri, "/pin/", NULL, &pin);
  if (pr == -2) {
    return HttpServer_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  if (pr == -3) {
    char reason[96];
    char range[32];
    HttpServer_FormatPinRange(range, sizeof(range));
    const char* p = req->uri + strlen("/pin/");
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

  GpioCtrl_Mode mode;
  bool openDrain = false;
  bool pullUp = false;
  bool pullDown = false;
  if (HttpServer_ParsePinConfigBody(body, &mode, &openDrain, &pullUp, &pullDown, reason, sizeof(reason)) != ESP_OK) {
    cJSON_Delete(body);
    return HttpServer_SendError(req, 400, reason);
  }
  cJSON_Delete(body);

  esp_err_t cfg = GpioCtrl_SetConfig(pin, mode, openDrain, pullUp, pullDown);
  if (cfg == ESP_ERR_NO_MEM) {
    return HttpServer_SendError(
        req, 422, "PWM output limit reached. Disable another pwmOutput pin before enabling this one.");
  }
  if (cfg == ESP_ERR_NOT_SUPPORTED) {
    return HttpServer_SendError(req, 400, "PWM frequency cannot be generated by this hardware. Try a higher frequency.");
  }
  if (cfg != ESP_OK) {
    return HttpServer_SendError(req, 500, "internal");
  }
  Lock_Touch(lockId[0] ? lockId : NULL);
  return HttpServer_SendEmpty(req, 204);
}

static esp_err_t Route_PinGetLevelHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Lock_SweepExpired();
  int pin = 0;
  int pr = HttpServer_ParsePathPin(req->uri, "/pin/", "/level", &pin);
  if (pr == -2) {
    return HttpServer_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  if (pr == -3) {
    const char* p = req->uri + strlen("/pin/");
    long raw = strtol(p, NULL, 10);
    char reason[96];
    char range[32];
    HttpServer_FormatPinRange(range, sizeof(range));
    snprintf(reason, sizeof(reason), "Pin %ld does not exist. Use a pin from %s.", raw, range);
    return HttpServer_SendError(req, 404, reason);
  }

  TOOL_CALL_LOG("rest %s %s pin=%d", HttpServer_MethodName(req->method), req->uri, pin);

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

static esp_err_t Route_PinPostLevelHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Lock_SweepExpired();
  int pin = 0;
  int pr = HttpServer_ParsePathPin(req->uri, "/pin/", "/level", &pin);
  if (pr == -2) {
    return HttpServer_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  if (pr == -3) {
    const char* p = req->uri + strlen("/pin/");
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
    Route_PinLevelWriteDenied(pin, reason, sizeof(reason));
    return HttpServer_SendError(req, 422, reason);
  }
  if (GpioCtrl_SetLevel(pin, level) != ESP_OK) {
    return HttpServer_SendError(req, 500, "internal");
  }
  Lock_Touch(lockId[0] ? lockId : NULL);
  return HttpServer_SendEmpty(req, 204);
}

static esp_err_t Route_PinPostPulseHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Lock_SweepExpired();
  int pin = 0;
  int pr = HttpServer_ParsePathPin(req->uri, "/pin/", "/pulse", &pin);
  if (pr == -2) {
    return HttpServer_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  if (pr == -3) {
    const char* p = req->uri + strlen("/pin/");
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
    Route_PinLevelWriteDenied(pin, reason, sizeof(reason));
    return HttpServer_SendError(req, 422, reason);
  }
  if (GpioCtrl_Pulse(pin, level, width) != ESP_OK) {
    return HttpServer_SendError(req, 500, "internal");
  }
  Lock_Touch(lockId[0] ? lockId : NULL);
  return HttpServer_SendEmpty(req, 204);
}

static esp_err_t Route_PinGetPwmHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Lock_SweepExpired();
  int pin = 0;
  int pr = HttpServer_ParsePathPin(req->uri, "/pin/", "/pwm", &pin);
  if (pr == -2) {
    return HttpServer_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  if (pr == -3) {
    const char* p = req->uri + strlen("/pin/");
    long raw = strtol(p, NULL, 10);
    char reason[96];
    char range[32];
    HttpServer_FormatPinRange(range, sizeof(range));
    snprintf(reason, sizeof(reason), "Pin %ld does not exist. Use a pin from %s.", raw, range);
    return HttpServer_SendError(req, 404, reason);
  }

  TOOL_CALL_LOG("rest %s %s pin=%d", HttpServer_MethodName(req->method), req->uri, pin);

  char lockId[64];
  char reason[256];
  int st = HttpServer_LockStatus(req, LOCK_KIND_GPIO, pin, LOCK_METHOD_READ, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return HttpServer_SendError(req, st, reason);

  if (!GpioCtrl_IsPwmMode(pin)) {
    Route_PinPwmDenied(pin, reason, sizeof(reason));
    return HttpServer_SendError(req, 422, reason);
  }

  double frequency = 0;
  double duty = 0;
  if (GpioCtrl_GetPwm(pin, &frequency, &duty) != ESP_OK) {
    return HttpServer_SendError(req, 500, "internal");
  }
  Lock_Touch(lockId[0] ? lockId : NULL);
  cJSON* root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "frequency", frequency);
  cJSON_AddNumberToObject(root, "duty", duty);
  cJSON_AddNumberToObject(root, "time", (double)HttpServer_NowUs());
  return HttpServer_SendJson(req, 200, root);
}

static esp_err_t Route_PinPostPwmHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Lock_SweepExpired();
  int pin = 0;
  int pr = HttpServer_ParsePathPin(req->uri, "/pin/", "/pwm", &pin);
  if (pr == -2) {
    return HttpServer_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  if (pr == -3) {
    const char* p = req->uri + strlen("/pin/");
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

  cJSON* freqItem = cJSON_GetObjectItem(body, "frequency");
  cJSON* dutyItem = cJSON_GetObjectItem(body, "duty");
  if (!cJSON_IsNumber(freqItem) || freqItem->valuedouble < 1.0 ||
      freqItem->valuedouble > (double)CONFIG_GPIO_PWM_MAX_FREQ_HZ) {
    cJSON_Delete(body);
    return HttpServer_SendError(req, 400, "frequency must be a number from 1 to 50000 Hz.");
  }
  if (!cJSON_IsNumber(dutyItem) || dutyItem->valuedouble < 0.0 || dutyItem->valuedouble > 100.0) {
    cJSON_Delete(body);
    return HttpServer_SendError(req, 400, "duty must be a number from 0 to 100.");
  }
  double frequency = freqItem->valuedouble;
  double duty = dutyItem->valuedouble;
  cJSON_Delete(body);

  if (!GpioCtrl_IsPwmMode(pin)) {
    Route_PinPwmDenied(pin, reason, sizeof(reason));
    return HttpServer_SendError(req, 422, reason);
  }

  esp_err_t pwm = GpioCtrl_SetPwm(pin, frequency, duty);
  if (pwm == ESP_ERR_NO_MEM) {
    return HttpServer_SendError(
        req, 422, "PWM output limit reached. Disable another pwmOutput pin before enabling this one.");
  }
  if (pwm == ESP_ERR_NOT_SUPPORTED) {
    return HttpServer_SendError(req, 400, "PWM frequency cannot be generated by this hardware. Try a higher frequency.");
  }
  if (pwm == ESP_ERR_INVALID_STATE) {
    Route_PinPwmDenied(pin, reason, sizeof(reason));
    return HttpServer_SendError(req, 422, reason);
  }
  if (pwm != ESP_OK) {
    return HttpServer_SendError(req, 500, "internal");
  }
  Lock_Touch(lockId[0] ? lockId : NULL);
  return HttpServer_SendEmpty(req, 204);
}

static esp_err_t Route_PinGetTraceHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Lock_SweepExpired();
  int pin = 0;
  char path[128];
  strncpy(path, req->uri, sizeof(path) - 1);
  path[sizeof(path) - 1] = '\0';
  char* q = strchr(path, '?');
  if (q) *q = '\0';

  int pr = HttpServer_ParsePathPin(path, "/pin/", "/trace", &pin);
  if (pr == -2) {
    return HttpServer_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  if (pr == -3) {
    const char* p = path + strlen("/pin/");
    long raw = strtol(p, NULL, 10);
    char reason[96];
    char range[32];
    HttpServer_FormatPinRange(range, sizeof(range));
    snprintf(reason, sizeof(reason), "Pin %ld does not exist. Use a pin from %s.", raw, range);
    return HttpServer_SendError(req, 404, reason);
  }

  TOOL_CALL_LOG("rest %s %s pin=%d", HttpServer_MethodName(req->method), req->uri, pin);

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

  return Route_PinStartTrace(req, &pin, 1, edge, duration, false, lockId);
}

static esp_err_t Route_PinBatchGetLevelHandler(httpd_req_t* req)
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
  int pins[TOOL_GET_ARRAY_LENGTH(pinLogicalToHw)];
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

static esp_err_t Route_PinBatchPostLevelHandler(httpd_req_t* req)
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
  int pins[TOOL_GET_ARRAY_LENGTH(pinLogicalToHw)];
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
      Route_PinLevelWriteDenied(pins[i], reason, sizeof(reason));
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

static esp_err_t Route_PinBatchPutConfigHandler(httpd_req_t* req)
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
  int pins[TOOL_GET_ARRAY_LENGTH(pinLogicalToHw)];
  if (maxPins > (int)TOOL_GET_ARRAY_LENGTH(pins)) maxPins = (int)TOOL_GET_ARRAY_LENGTH(pins);
  size_t count = 0;
  char reason[256];
  if (HttpServer_ParsePinsArray(body, pins, (size_t)maxPins, &count, reason, sizeof(reason)) != ESP_OK) {
    cJSON_Delete(body);
    return HttpServer_SendError(req, 400, reason);
  }
  GpioCtrl_Mode mode;
  bool openDrain = false;
  bool pullUp = false;
  bool pullDown = false;
  if (HttpServer_ParsePinConfigBody(body, &mode, &openDrain, &pullUp, &pullDown, reason, sizeof(reason)) != ESP_OK) {
    cJSON_Delete(body);
    return HttpServer_SendError(req, 400, reason);
  }
  cJSON_Delete(body);

  char lockId[64];
  for (size_t i = 0; i < count; i++) {
    int st = HttpServer_LockStatus(req, LOCK_KIND_GPIO, pins[i], LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason,
                                   sizeof(reason));
    if (st) return HttpServer_SendError(req, st, reason);
  }
  for (size_t i = 0; i < count; i++) {
    esp_err_t cfg = GpioCtrl_SetConfig(pins[i], mode, openDrain, pullUp, pullDown);
    if (cfg == ESP_ERR_NO_MEM) {
      return HttpServer_SendError(
          req, 422,
          "PWM output limit reached. Disable another pwmOutput pin before enabling this one.");
    }
    if (cfg == ESP_ERR_NOT_SUPPORTED) {
      return HttpServer_SendError(req, 400, "PWM frequency cannot be generated by this hardware. Try a higher frequency.");
    }
    if (cfg != ESP_OK) {
      return HttpServer_SendError(req, 500, "internal");
    }
  }
  Lock_Touch(lockId[0] ? lockId : NULL);
  return HttpServer_SendEmpty(req, 204);
}

static esp_err_t Route_PinBatchPostPulseHandler(httpd_req_t* req)
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
  int pins[TOOL_GET_ARRAY_LENGTH(pinLogicalToHw)];
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
      Route_PinLevelWriteDenied(pins[i], reason, sizeof(reason));
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

static esp_err_t Route_PinBatchGetTraceHandler(httpd_req_t* req)
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
  int pins[TOOL_GET_ARRAY_LENGTH(pinLogicalToHw)];
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

  return Route_PinStartTrace(req, pins, count, edge, duration, true, lockId);
}

/* Exact /pin/config before /pin/<pin> so wildcard does not swallow "config". */
static const httpd_uri_t uris[] = {
    {.uri = "/pin/", .method = HTTP_GET, .handler = Route_PinListHandler},
    {.uri = "/pin/config", .method = HTTP_PUT, .handler = Route_PinBatchPutConfigHandler},
    {.uri = "/pin/level", .method = HTTP_GET, .handler = Route_PinBatchGetLevelHandler},
    {.uri = "/pin/level", .method = HTTP_POST, .handler = Route_PinBatchPostLevelHandler},
    {.uri = "/pin/pulse", .method = HTTP_POST, .handler = Route_PinBatchPostPulseHandler},
    {.uri = "/pin/trace", .method = HTTP_GET, .handler = Route_PinBatchGetTraceHandler},
    {.uri = "/pin/*/level", .method = HTTP_GET, .handler = Route_PinGetLevelHandler},
    {.uri = "/pin/*/level", .method = HTTP_POST, .handler = Route_PinPostLevelHandler},
    {.uri = "/pin/*/pulse", .method = HTTP_POST, .handler = Route_PinPostPulseHandler},
    {.uri = "/pin/*/pwm", .method = HTTP_GET, .handler = Route_PinGetPwmHandler},
    {.uri = "/pin/*/pwm", .method = HTTP_POST, .handler = Route_PinPostPwmHandler},
    {.uri = "/pin/*/trace", .method = HTTP_GET, .handler = Route_PinGetTraceHandler},
    {.uri = "/pin/*", .method = HTTP_PUT, .handler = Route_PinPutModeHandler},
};

esp_err_t Route_PinRegister(httpd_handle_t server)
{
  for (size_t i = 0; i < TOOL_GET_ARRAY_LENGTH(uris); i++) {
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(httpd_register_uri_handler(server, &uris[i]), "register pin uri failed");
  }
  return ESP_OK;
}
