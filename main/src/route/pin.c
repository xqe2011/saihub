/**
 * @name Pin routes
 * @file pin.c
 * @author xqe2011
 */
#include "route.h"

#include "api.pb.h"
#include "api_conv.h"
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

static const int pinLogicalToHw[] = CONFIG_GPIO_LOGICAL_TO_HW;

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

static esp_err_t Route_PinParseConfigPb(saihub_api_Mode modePb, bool openDrain, bool pullUp, bool pullDown,
                                        GpioCtrl_Mode* modeOut, bool* openDrainOut, bool* pullUpOut, bool* pullDownOut,
                                        char* reason, size_t reasonLen)
{
  static const char* modeHint = "disable, digitalInput, digitalOutput, digitalInputOutput, pwmOutput";
  GpioCtrl_Mode mode;
  if (!ApiConv_ModeFromPb(modePb, &mode)) {
    snprintf(reason, reasonLen, "mode is invalid. Use one of: %s.", modeHint);
    return ESP_ERR_INVALID_ARG;
  }
  if (openDrain && (mode == GPIO_CTRL_MODE_DISABLE || mode == GPIO_CTRL_MODE_DIGITAL_INPUT)) {
    snprintf(reason, reasonLen, "openDrain cannot be true when mode is disable or digitalInput.");
    return ESP_ERR_INVALID_ARG;
  }
  *modeOut = mode;
  *openDrainOut = openDrain;
  *pullUpOut = pullUp;
  *pullDownOut = pullDown;
  return ESP_OK;
}

static esp_err_t Route_PinParsePinsPb(const int32_t* pinsIn, pb_size_t pinsCount, int* pinsOut, size_t maxPins,
                                      size_t* countOut, char* reason, size_t reasonLen)
{
  char range[32];
  HttpServer_FormatPinRange(range, sizeof(range));
  if (pinsCount == 0) {
    snprintf(reason, reasonLen, "pins must be a non-empty array of integers from %s.", range);
    return ESP_ERR_INVALID_ARG;
  }
  if ((size_t)pinsCount > maxPins) {
    snprintf(reason, reasonLen, "pins array is too long.");
    return ESP_ERR_INVALID_ARG;
  }
  for (pb_size_t i = 0; i < pinsCount; i++) {
    int pin = pinsIn[i];
    if (!GpioCtrl_IsValidLogicalPin(pin)) {
      snprintf(reason, reasonLen, "pins[%u] (%d) is invalid. Use a pin from %s.", (unsigned)i, pin, range);
      return ESP_ERR_INVALID_ARG;
    }
    pinsOut[i] = pin;
  }
  *countOut = (size_t)pinsCount;
  return ESP_OK;
}

static esp_err_t Route_PinListHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  TOOL_CALL_LOG("rest %s %s", HttpServer_MethodName(req->method), req->uri);
  Lock_SweepExpired();
  int count = GpioCtrl_GetLogicalCount();
  if (HttpServer_HasProtobufContentType(req)) {
    saihub_api_PinListResponse msg = saihub_api_PinListResponse_init_zero;
    if (count > (int)TOOL_GET_ARRAY_LENGTH(msg.pins)) count = (int)TOOL_GET_ARRAY_LENGTH(msg.pins);
    for (int i = 0; i < count; i++) {
      GpioCtrl_State st;
      GpioCtrl_GetState(i, &st);
      saihub_api_PinState* item = &msg.pins[msg.pins_count++];
      item->pin = i;
      item->mode = ApiConv_ModeToPb(st.mode);
      item->openDrain = st.openDrain;
      item->pullUp = st.pullUp;
      item->pullDown = st.pullDown;
      item->level = st.level;
    }
    msg.time = HttpServer_NowUs();
    return HttpServer_SendPb(req, 200, saihub_api_PinListResponse_fields, &msg);
  }
  cJSON* root = cJSON_CreateObject();
  cJSON* pinsArr = cJSON_CreateArray();
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

  if (!HttpServer_HasApiContentType(req)) {
    return HttpServer_SendError(req, 415, HttpServer_UnsupportedMediaTypeReason());
  }

  char lockId[64];
  char reason[256];
  int st = HttpServer_LockStatus(req, LOCK_KIND_GPIO, pin, LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return HttpServer_SendError(req, st, reason);

  GpioCtrl_Mode mode;
  bool openDrain = false;
  bool pullUp = false;
  bool pullDown = false;
  if (HttpServer_HasProtobufContentType(req)) {
    saihub_api_PinConfig body = saihub_api_PinConfig_init_zero;
    esp_err_t dr = HttpServer_DecodePb(req, saihub_api_PinConfig_fields, &body);
    if (dr != ESP_OK) return HttpServer_SendError(req, 400, "invalid_protobuf");
    if (Route_PinParseConfigPb(body.mode, body.openDrain, body.pullUp, body.pullDown, &mode, &openDrain, &pullUp,
                               &pullDown, reason, sizeof(reason)) != ESP_OK) {
      return HttpServer_SendError(req, 400, reason);
    }
  } else {
    esp_err_t perr = ESP_OK;
    cJSON* body = HttpServer_ParseBody(req, &perr);
    if (body == NULL) {
      return HttpServer_SendError(req, 400, "invalid_json");
    }
    if (HttpServer_ParsePinConfigBody(body, &mode, &openDrain, &pullUp, &pullDown, reason, sizeof(reason)) != ESP_OK) {
      cJSON_Delete(body);
      return HttpServer_SendError(req, 400, reason);
    }
    cJSON_Delete(body);
  }

  esp_err_t cfg = GpioCtrl_SetConfig(pin, mode, openDrain, pullUp, pullDown);
  if (cfg == ESP_ERR_NO_MEM) {
    return HttpServer_SendError(
        req, 422, "PWM resources exhausted. Free another pwmOutput pin or reuse an existing frequency.");
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
  if (HttpServer_HasProtobufContentType(req)) {
    saihub_api_LevelState msg = saihub_api_LevelState_init_zero;
    msg.level = level;
    msg.time = HttpServer_NowUs();
    return HttpServer_SendPb(req, 200, saihub_api_LevelState_fields, &msg);
  }
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

  if (!HttpServer_HasApiContentType(req)) {
    return HttpServer_SendError(req, 415, HttpServer_UnsupportedMediaTypeReason());
  }

  char lockId[64];
  char reason[256];
  int st = HttpServer_LockStatus(req, LOCK_KIND_GPIO, pin, LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return HttpServer_SendError(req, st, reason);

  int level;
  if (HttpServer_HasProtobufContentType(req)) {
    saihub_api_LevelBody body = saihub_api_LevelBody_init_zero;
    esp_err_t dr = HttpServer_DecodePb(req, saihub_api_LevelBody_fields, &body);
    if (dr != ESP_OK) return HttpServer_SendError(req, 400, "invalid_protobuf");
    if (body.level != 0 && body.level != 1) {
      return HttpServer_SendError(req, 400, "level must be 0 or 1.");
    }
    level = body.level;
  } else {
    esp_err_t perr = ESP_OK;
    cJSON* body = HttpServer_ParseBody(req, &perr);
    if (body == NULL) return HttpServer_SendError(req, 400, "invalid_json");

    cJSON* levelItem = cJSON_GetObjectItem(body, "level");
    if (!cJSON_IsNumber(levelItem) || (levelItem->valueint != 0 && levelItem->valueint != 1)) {
      cJSON_Delete(body);
      return HttpServer_SendError(req, 400, "level must be 0 or 1.");
    }
    level = levelItem->valueint;
    cJSON_Delete(body);
  }

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
  if (!HttpServer_HasApiContentType(req)) {
    return HttpServer_SendError(req, 415, HttpServer_UnsupportedMediaTypeReason());
  }

  char lockId[64];
  char reason[256];
  int st = HttpServer_LockStatus(req, LOCK_KIND_GPIO, pin, LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return HttpServer_SendError(req, st, reason);

  uint64_t width;
  int level;
  if (HttpServer_HasProtobufContentType(req)) {
    saihub_api_PulseBody body = saihub_api_PulseBody_init_zero;
    esp_err_t dr = HttpServer_DecodePb(req, saihub_api_PulseBody_fields, &body);
    if (dr != ESP_OK) return HttpServer_SendError(req, 400, "invalid_protobuf");
    if (body.width < 1 || body.width > (int64_t)CONFIG_GPIO_PULSE_MAX_WIDTH_US) {
      return HttpServer_SendError(req, 400, "width must be an integer from 1 to 1000000 microseconds.");
    }
    if (body.level != 0 && body.level != 1) {
      return HttpServer_SendError(req, 400, "level must be 0 or 1.");
    }
    width = (uint64_t)body.width;
    level = body.level;
  } else {
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
    width = (uint64_t)widthItem->valuedouble;
    level = levelItem->valueint;
    cJSON_Delete(body);
  }

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
  if (HttpServer_HasProtobufContentType(req)) {
    saihub_api_PwmState msg = saihub_api_PwmState_init_zero;
    msg.frequency = frequency;
    msg.duty = duty;
    msg.time = HttpServer_NowUs();
    return HttpServer_SendPb(req, 200, saihub_api_PwmState_fields, &msg);
  }
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

  if (!HttpServer_HasApiContentType(req)) {
    return HttpServer_SendError(req, 415, HttpServer_UnsupportedMediaTypeReason());
  }

  char lockId[64];
  char reason[256];
  int st = HttpServer_LockStatus(req, LOCK_KIND_GPIO, pin, LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return HttpServer_SendError(req, st, reason);

  double frequency;
  double duty;
  if (HttpServer_HasProtobufContentType(req)) {
    saihub_api_PwmBody body = saihub_api_PwmBody_init_zero;
    esp_err_t dr = HttpServer_DecodePb(req, saihub_api_PwmBody_fields, &body);
    if (dr != ESP_OK) return HttpServer_SendError(req, 400, "invalid_protobuf");
    if (body.frequency < 1.0 || body.frequency > (double)CONFIG_GPIO_PWM_MAX_FREQ_HZ) {
      return HttpServer_SendError(req, 400, "frequency must be a number from 1 to 50000 Hz.");
    }
    if (body.duty < 0.0 || body.duty > 100.0) {
      return HttpServer_SendError(req, 400, "duty must be a number from 0 to 100.");
    }
    frequency = body.frequency;
    duty = body.duty;
  } else {
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
    frequency = freqItem->valuedouble;
    duty = dutyItem->valuedouble;
    cJSON_Delete(body);
  }

  if (!GpioCtrl_IsPwmMode(pin)) {
    Route_PinPwmDenied(pin, reason, sizeof(reason));
    return HttpServer_SendError(req, 422, reason);
  }

  esp_err_t pwm = GpioCtrl_SetPwm(pin, frequency, duty);
  if (pwm == ESP_ERR_NO_MEM) {
    return HttpServer_SendError(
        req, 422, "PWM resources exhausted. Free another pwmOutput pin or reuse an existing frequency.");
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

  GpioCtrl_TraceEvent* events = calloc(CONFIG_GPIO_TRACE_MAX_EVENTS, sizeof(GpioCtrl_TraceEvent));
  if (events == NULL) return HttpServer_SendError(req, 500, "internal");
  size_t count = 0;
  esp_err_t tr = GpioCtrl_Trace(&pin, 1, edge, duration, events, CONFIG_GPIO_TRACE_MAX_EVENTS, &count, false);
  if (tr != ESP_OK) {
    free(events);
    return HttpServer_SendError(req, 500, "internal");
  }

  Lock_Touch(lockId[0] ? lockId : NULL);
  if (HttpServer_HasProtobufContentType(req)) {
    saihub_api_TraceResponse msg = saihub_api_TraceResponse_init_zero;
    saihub_api_TraceEvent* pbEvents = NULL;
    if (count > 0) {
      pbEvents = calloc(count, sizeof(saihub_api_TraceEvent));
      if (pbEvents == NULL) {
        free(events);
        return HttpServer_SendError(req, 500, "internal");
      }
      for (size_t i = 0; i < count; i++) {
        pbEvents[i].edge = ApiConv_TraceEdgeToPb(events[i].edge);
        pbEvents[i].level = events[i].level;
        pbEvents[i].time = (int64_t)events[i].time;
      }
    }
    free(events);
    msg.events = pbEvents;
    msg.events_count = (pb_size_t)count;
    esp_err_t ret = HttpServer_SendPb(req, 200, saihub_api_TraceResponse_fields, &msg);
    free(pbEvents);
    return ret;
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
  return HttpServer_SendJson(req, 200, root);
}

static esp_err_t Route_PinBatchGetLevelHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Lock_SweepExpired();
  if (!HttpServer_HasApiContentType(req)) {
    return HttpServer_SendError(req, 415, HttpServer_UnsupportedMediaTypeReason());
  }

  int maxPins = GpioCtrl_GetLogicalCount();
  int pins[TOOL_GET_ARRAY_LENGTH(pinLogicalToHw)];
  if (maxPins > (int)TOOL_GET_ARRAY_LENGTH(pins)) maxPins = (int)TOOL_GET_ARRAY_LENGTH(pins);
  size_t count = 0;
  char reason[192];

  if (HttpServer_HasProtobufContentType(req)) {
    saihub_api_PinsBody body = saihub_api_PinsBody_init_zero;
    esp_err_t dr = HttpServer_DecodePb(req, saihub_api_PinsBody_fields, &body);
    if (dr != ESP_OK) return HttpServer_SendError(req, 400, "invalid_protobuf");
    if (Route_PinParsePinsPb(body.pins, body.pins_count, pins, (size_t)maxPins, &count, reason, sizeof(reason)) !=
        ESP_OK) {
      return HttpServer_SendError(req, 400, reason);
    }
  } else {
    esp_err_t perr = ESP_OK;
    cJSON* body = HttpServer_ParseBody(req, &perr);
    if (body == NULL) return HttpServer_SendError(req, 400, "invalid_json");
    if (HttpServer_ParsePinsArray(body, pins, (size_t)maxPins, &count, reason, sizeof(reason)) != ESP_OK) {
      cJSON_Delete(body);
      return HttpServer_SendError(req, 400, reason);
    }
    cJSON_Delete(body);
  }

  char lockId[64];
  for (size_t i = 0; i < count; i++) {
    int st =
        HttpServer_LockStatus(req, LOCK_KIND_GPIO, pins[i], LOCK_METHOD_READ, lockId, sizeof(lockId), reason, sizeof(reason));
    if (st) return HttpServer_SendError(req, st, reason);
  }

  if (HttpServer_HasProtobufContentType(req)) {
    saihub_api_LevelsResponse msg = saihub_api_LevelsResponse_init_zero;
    for (size_t i = 0; i < count; i++) {
      int level = 0;
      GpioCtrl_GetLevel(pins[i], &level);
      msg.levels[msg.levels_count++] = level;
    }
    msg.time = HttpServer_NowUs();
    Lock_Touch(lockId[0] ? lockId : NULL);
    return HttpServer_SendPb(req, 200, saihub_api_LevelsResponse_fields, &msg);
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
  if (!HttpServer_HasApiContentType(req)) {
    return HttpServer_SendError(req, 415, HttpServer_UnsupportedMediaTypeReason());
  }

  int maxPins = GpioCtrl_GetLogicalCount();
  int pins[TOOL_GET_ARRAY_LENGTH(pinLogicalToHw)];
  if (maxPins > (int)TOOL_GET_ARRAY_LENGTH(pins)) maxPins = (int)TOOL_GET_ARRAY_LENGTH(pins);
  size_t count = 0;
  char reason[256];
  int level;

  if (HttpServer_HasProtobufContentType(req)) {
    saihub_api_PinsLevelBody body = saihub_api_PinsLevelBody_init_zero;
    esp_err_t dr = HttpServer_DecodePb(req, saihub_api_PinsLevelBody_fields, &body);
    if (dr != ESP_OK) return HttpServer_SendError(req, 400, "invalid_protobuf");
    if (Route_PinParsePinsPb(body.pins, body.pins_count, pins, (size_t)maxPins, &count, reason, sizeof(reason)) !=
        ESP_OK) {
      return HttpServer_SendError(req, 400, reason);
    }
    if (body.level != 0 && body.level != 1) {
      return HttpServer_SendError(req, 400, "level must be 0 or 1.");
    }
    level = body.level;
  } else {
    esp_err_t perr = ESP_OK;
    cJSON* body = HttpServer_ParseBody(req, &perr);
    if (body == NULL) return HttpServer_SendError(req, 400, "invalid_json");
    if (HttpServer_ParsePinsArray(body, pins, (size_t)maxPins, &count, reason, sizeof(reason)) != ESP_OK) {
      cJSON_Delete(body);
      return HttpServer_SendError(req, 400, reason);
    }
    cJSON* levelItem = cJSON_GetObjectItem(body, "level");
    if (!cJSON_IsNumber(levelItem) || (levelItem->valueint != 0 && levelItem->valueint != 1)) {
      cJSON_Delete(body);
      return HttpServer_SendError(req, 400, "level must be 0 or 1.");
    }
    level = levelItem->valueint;
    cJSON_Delete(body);
  }

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
  if (!HttpServer_HasApiContentType(req)) {
    return HttpServer_SendError(req, 415, HttpServer_UnsupportedMediaTypeReason());
  }

  int maxPins = GpioCtrl_GetLogicalCount();
  int pins[TOOL_GET_ARRAY_LENGTH(pinLogicalToHw)];
  if (maxPins > (int)TOOL_GET_ARRAY_LENGTH(pins)) maxPins = (int)TOOL_GET_ARRAY_LENGTH(pins);
  size_t count = 0;
  char reason[256];
  GpioCtrl_Mode mode;
  bool openDrain = false;
  bool pullUp = false;
  bool pullDown = false;

  if (HttpServer_HasProtobufContentType(req)) {
    saihub_api_PinConfigBatchBody body = saihub_api_PinConfigBatchBody_init_zero;
    esp_err_t dr = HttpServer_DecodePb(req, saihub_api_PinConfigBatchBody_fields, &body);
    if (dr != ESP_OK) return HttpServer_SendError(req, 400, "invalid_protobuf");
    if (Route_PinParsePinsPb(body.pins, body.pins_count, pins, (size_t)maxPins, &count, reason, sizeof(reason)) !=
        ESP_OK) {
      return HttpServer_SendError(req, 400, reason);
    }
    if (Route_PinParseConfigPb(body.mode, body.openDrain, body.pullUp, body.pullDown, &mode, &openDrain, &pullUp,
                               &pullDown, reason, sizeof(reason)) != ESP_OK) {
      return HttpServer_SendError(req, 400, reason);
    }
  } else {
    esp_err_t perr = ESP_OK;
    cJSON* body = HttpServer_ParseBody(req, &perr);
    if (body == NULL) return HttpServer_SendError(req, 400, "invalid_json");
    if (HttpServer_ParsePinsArray(body, pins, (size_t)maxPins, &count, reason, sizeof(reason)) != ESP_OK) {
      cJSON_Delete(body);
      return HttpServer_SendError(req, 400, reason);
    }
    if (HttpServer_ParsePinConfigBody(body, &mode, &openDrain, &pullUp, &pullDown, reason, sizeof(reason)) != ESP_OK) {
      cJSON_Delete(body);
      return HttpServer_SendError(req, 400, reason);
    }
    cJSON_Delete(body);
  }

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
          "PWM resources exhausted. Free another pwmOutput pin or reuse an existing frequency.");
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
  if (!HttpServer_HasApiContentType(req)) {
    return HttpServer_SendError(req, 415, HttpServer_UnsupportedMediaTypeReason());
  }

  int maxPins = GpioCtrl_GetLogicalCount();
  int pins[TOOL_GET_ARRAY_LENGTH(pinLogicalToHw)];
  if (maxPins > (int)TOOL_GET_ARRAY_LENGTH(pins)) maxPins = (int)TOOL_GET_ARRAY_LENGTH(pins);
  size_t count = 0;
  char reason[256];
  uint64_t width;
  int level;

  if (HttpServer_HasProtobufContentType(req)) {
    saihub_api_PulseBatchBody body = saihub_api_PulseBatchBody_init_zero;
    esp_err_t dr = HttpServer_DecodePb(req, saihub_api_PulseBatchBody_fields, &body);
    if (dr != ESP_OK) return HttpServer_SendError(req, 400, "invalid_protobuf");
    if (Route_PinParsePinsPb(body.pins, body.pins_count, pins, (size_t)maxPins, &count, reason, sizeof(reason)) !=
        ESP_OK) {
      return HttpServer_SendError(req, 400, reason);
    }
    if (body.width < 1 || body.width > (int64_t)CONFIG_GPIO_PULSE_MAX_WIDTH_US) {
      return HttpServer_SendError(req, 400, "width must be an integer from 1 to 1000000 microseconds.");
    }
    if (body.level != 0 && body.level != 1) {
      return HttpServer_SendError(req, 400, "level must be 0 or 1.");
    }
    width = (uint64_t)body.width;
    level = body.level;
  } else {
    esp_err_t perr = ESP_OK;
    cJSON* body = HttpServer_ParseBody(req, &perr);
    if (body == NULL) return HttpServer_SendError(req, 400, "invalid_json");
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
    width = (uint64_t)widthItem->valuedouble;
    level = levelItem->valueint;
    cJSON_Delete(body);
  }

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
  if (!HttpServer_HasApiContentType(req)) {
    return HttpServer_SendError(req, 415, HttpServer_UnsupportedMediaTypeReason());
  }

  int maxPins = GpioCtrl_GetLogicalCount();
  int pins[TOOL_GET_ARRAY_LENGTH(pinLogicalToHw)];
  if (maxPins > (int)TOOL_GET_ARRAY_LENGTH(pins)) maxPins = (int)TOOL_GET_ARRAY_LENGTH(pins);
  size_t count = 0;
  char reason[256];
  GpioCtrl_Edge edge = GPIO_CTRL_EDGE_BOTH;
  uint64_t duration = TRACE_DEFAULT_DURATION_US;

  if (HttpServer_HasProtobufContentType(req)) {
    saihub_api_TraceBatchBody body = saihub_api_TraceBatchBody_init_zero;
    esp_err_t dr = HttpServer_DecodePb(req, saihub_api_TraceBatchBody_fields, &body);
    if (dr != ESP_OK) return HttpServer_SendError(req, 400, "invalid_protobuf");
    if (Route_PinParsePinsPb(body.pins, body.pins_count, pins, (size_t)maxPins, &count, reason, sizeof(reason)) !=
        ESP_OK) {
      return HttpServer_SendError(req, 400, reason);
    }
    if (body.edge != saihub_api_Edge_EDGE_UNSPECIFIED) {
      if (!ApiConv_EdgeFromPb(body.edge, &edge)) {
        return HttpServer_SendError(req, 400, "edge is invalid. Use one of: raising, falling, both.");
      }
    }
    if (body.duration != 0) {
      duration = (uint64_t)body.duration;
    }
  } else {
    esp_err_t perr = ESP_OK;
    cJSON* body = HttpServer_ParseBody(req, &perr);
    if (body == NULL) return HttpServer_SendError(req, 400, "invalid_json");
    if (HttpServer_ParsePinsArray(body, pins, (size_t)maxPins, &count, reason, sizeof(reason)) != ESP_OK) {
      cJSON_Delete(body);
      return HttpServer_SendError(req, 400, reason);
    }
    cJSON* edgeItem = cJSON_GetObjectItem(body, "edge");
    cJSON* durationItem = cJSON_GetObjectItem(body, "duration");
    if (cJSON_IsString(edgeItem)) {
      if (!GpioCtrl_EdgeFromString(edgeItem->valuestring, &edge)) {
        cJSON_Delete(body);
        return HttpServer_SendError(req, 400, "edge is invalid. Use one of: raising, falling, both.");
      }
    }
    if (cJSON_IsNumber(durationItem)) {
      duration = (uint64_t)durationItem->valuedouble;
    }
    cJSON_Delete(body);
  }
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

  Lock_Touch(lockId[0] ? lockId : NULL);
  if (HttpServer_HasProtobufContentType(req)) {
    saihub_api_TraceWithPinResponse msg = saihub_api_TraceWithPinResponse_init_zero;
    saihub_api_TraceEventWithPin* pbEvents = NULL;
    if (eventCount > 0) {
      pbEvents = calloc(eventCount, sizeof(saihub_api_TraceEventWithPin));
      if (pbEvents == NULL) {
        free(events);
        return HttpServer_SendError(req, 500, "internal");
      }
      for (size_t i = 0; i < eventCount; i++) {
        pbEvents[i].pin = events[i].pin;
        pbEvents[i].edge = ApiConv_TraceEdgeToPb(events[i].edge);
        pbEvents[i].level = events[i].level;
        pbEvents[i].time = (int64_t)events[i].time;
      }
    }
    free(events);
    msg.events = pbEvents;
    msg.events_count = (pb_size_t)eventCount;
    esp_err_t ret = HttpServer_SendPb(req, 200, saihub_api_TraceWithPinResponse_fields, &msg);
    free(pbEvents);
    return ret;
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
  return HttpServer_SendJson(req, 200, root);
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
