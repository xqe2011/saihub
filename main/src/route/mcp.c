/**
 * @name MCP Streamable HTTP routes
 * @file mcp.c
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

static const char* tag = "SAIHUB-Mcp";

#define MCP_PROTOCOL_VERSION "2025-06-18"
#define TRACE_DEFAULT_DURATION_US 1000000ULL
#define MCP_MAX_PINS 16

typedef struct {
  int pins[MCP_MAX_PINS];
  size_t count;
  bool batch;
} Mcp_PinSelection;

static bool Route_McpOriginOk(httpd_req_t* req)
{
  char origin[256] = {0};
  if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK) {
    return true;
  }
  if (origin[0] == '\0') return true;
  return strncmp(origin, "http:", 5) == 0 || strncmp(origin, "https:", 6) == 0;
}

static bool Route_McpAcceptOk(httpd_req_t* req)
{
  char accept[256] = {0};
  if (httpd_req_get_hdr_value_str(req, "Accept", accept, sizeof(accept)) != ESP_OK) {
    return false;
  }
  return strstr(accept, "application/json") != NULL || strstr(accept, "text/event-stream") != NULL;
}

static bool Route_McpProtocolVersionOk(httpd_req_t* req, bool isInitialize)
{
  char ver[64] = {0};
  if (httpd_req_get_hdr_value_str(req, "MCP-Protocol-Version", ver, sizeof(ver)) != ESP_OK || ver[0] == '\0') {
    return true;
  }
  if (strcmp(ver, "2025-06-18") == 0 || strcmp(ver, "2025-03-26") == 0 || strcmp(ver, "2024-11-05") == 0) {
    return true;
  }
  (void)isInitialize;
  return false;
}

static esp_err_t Route_McpSendStatus(httpd_req_t* req, int status, const char* statusLine, const char* body,
                                     const char* contentType)
{
  HttpServer_SetCors(req);
  httpd_resp_set_status(req, statusLine);
  if (contentType) httpd_resp_set_type(req, contentType);
  if (body == NULL || body[0] == '\0') {
    return httpd_resp_send(req, NULL, 0);
  }
  return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t Route_McpSendJsonRpc(httpd_req_t* req, int httpStatus, cJSON* root)
{
  HttpServer_SetCors(req);
  char* printed = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (printed == NULL) {
    return Route_McpSendStatus(req, 500, "500 Internal Server Error",
                               "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"internal\"},\"id\":null}",
                               "application/json");
  }
  const char* statusLine = "200 OK";
  if (httpStatus == 400) statusLine = "400 Bad Request";
  else if (httpStatus == 406) statusLine = "406 Not Acceptable";
  else if (httpStatus == 415) statusLine = "415 Unsupported Media Type";
  else if (httpStatus == 500) statusLine = "500 Internal Server Error";
  httpd_resp_set_status(req, statusLine);
  httpd_resp_set_type(req, "application/json");
  esp_err_t ret = httpd_resp_send(req, printed, strlen(printed));
  free(printed);
  return ret;
}

static cJSON* Route_McpJsonRpcError(cJSON* id, int code, const char* message)
{
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "jsonrpc", "2.0");
  cJSON* err = cJSON_CreateObject();
  cJSON_AddNumberToObject(err, "code", code);
  cJSON_AddStringToObject(err, "message", message ? message : "error");
  cJSON_AddItemToObject(root, "error", err);
  if (id == NULL) {
    cJSON_AddNullToObject(root, "id");
  } else {
    cJSON_AddItemToObject(root, "id", cJSON_Duplicate(id, 1));
  }
  return root;
}

static cJSON* Route_McpJsonRpcResult(cJSON* id, cJSON* result)
{
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "jsonrpc", "2.0");
  cJSON_AddItemToObject(root, "result", result);
  if (id == NULL) {
    cJSON_AddNullToObject(root, "id");
  } else {
    cJSON_AddItemToObject(root, "id", cJSON_Duplicate(id, 1));
  }
  return root;
}

static cJSON* Route_McpToolResultOk(cJSON* payload)
{
  char* text = cJSON_PrintUnformatted(payload);
  cJSON_Delete(payload);
  cJSON* result = cJSON_CreateObject();
  cJSON* content = cJSON_CreateArray();
  cJSON* item = cJSON_CreateObject();
  cJSON_AddStringToObject(item, "type", "text");
  cJSON_AddStringToObject(item, "text", text ? text : "{}");
  cJSON_AddItemToArray(content, item);
  cJSON_AddItemToObject(result, "content", content);
  free(text);
  return result;
}

static cJSON* Route_McpToolResultErr(const char* reason)
{
  cJSON* payload = cJSON_CreateObject();
  cJSON_AddStringToObject(payload, "reason", reason ? reason : "error");
  char* text = cJSON_PrintUnformatted(payload);
  cJSON_Delete(payload);
  cJSON* result = cJSON_CreateObject();
  cJSON* content = cJSON_CreateArray();
  cJSON* item = cJSON_CreateObject();
  cJSON_AddStringToObject(item, "type", "text");
  cJSON_AddStringToObject(item, "text", text ? text : "{\"reason\":\"error\"}");
  cJSON_AddItemToArray(content, item);
  cJSON_AddItemToObject(result, "content", content);
  cJSON_AddBoolToObject(result, "isError", true);
  free(text);
  return result;
}

static cJSON* Route_McpToolResultOkEmpty(void)
{
  cJSON* payload = cJSON_CreateObject();
  cJSON_AddBoolToObject(payload, "ok", true);
  return Route_McpToolResultOk(payload);
}

static const char* Route_McpGetLockId(cJSON* args)
{
  if (args == NULL) return NULL;
  cJSON* lockId = cJSON_GetObjectItem(args, "lockId");
  if (!cJSON_IsString(lockId) || lockId->valuestring == NULL || lockId->valuestring[0] == '\0') {
    return NULL;
  }
  return lockId->valuestring;
}

static bool Route_McpParsePinSelection(cJSON* args, Mcp_PinSelection* out, char* reason, size_t reasonLen)
{
  memset(out, 0, sizeof(*out));
  if (args == NULL) {
    snprintf(reason, reasonLen, "arguments are required.");
    return false;
  }
  cJSON* pinItem = cJSON_GetObjectItem(args, "pin");
  cJSON* pinsItem = cJSON_GetObjectItem(args, "pins");
  bool hasPin = cJSON_IsNumber(pinItem);
  bool hasPins = cJSON_IsArray(pinsItem);
  if (hasPin == hasPins) {
    snprintf(reason, reasonLen, "Provide exactly one of pin or pins.");
    return false;
  }
  if (hasPin) {
    int pin = pinItem->valueint;
    if (!GpioCtrl_IsValidLogicalPin(pin)) {
      char range[32];
      HttpServer_FormatPinRange(range, sizeof(range));
      snprintf(reason, reasonLen, "pin is invalid. Use a pin from %s.", range);
      return false;
    }
    out->pins[0] = pin;
    out->count = 1;
    out->batch = false;
    return true;
  }
  size_t maxPins = (size_t)GpioCtrl_GetLogicalCount();
  if (maxPins > MCP_MAX_PINS) maxPins = MCP_MAX_PINS;
  if (HttpServer_ParsePinsArray(args, out->pins, maxPins, &out->count, reason, reasonLen) != ESP_OK) {
    return false;
  }
  out->batch = true;
  return true;
}

static int Route_McpCheckPinsLock(const Mcp_PinSelection* sel, uint8_t methods, const char* lockId, char* reason,
                                  size_t reasonLen)
{
  for (size_t i = 0; i < sel->count; i++) {
    int st = HttpServer_LockStatusId(lockId, LOCK_KIND_GPIO, sel->pins[i], methods, reason, reasonLen);
    if (st) return st;
  }
  return 0;
}

static void Route_McpAddProp(cJSON* props, const char* name, cJSON* schema)
{
  cJSON_AddItemToObject(props, name, schema);
}

static cJSON* Route_McpIntSchema(int minimum, int maximum)
{
  cJSON* s = cJSON_CreateObject();
  cJSON_AddStringToObject(s, "type", "integer");
  cJSON_AddNumberToObject(s, "minimum", minimum);
  cJSON_AddNumberToObject(s, "maximum", maximum);
  return s;
}

static cJSON* Route_McpNumberSchema(double minimum, double maximum)
{
  cJSON* s = cJSON_CreateObject();
  cJSON_AddStringToObject(s, "type", "number");
  cJSON_AddNumberToObject(s, "minimum", minimum);
  cJSON_AddNumberToObject(s, "maximum", maximum);
  return s;
}

static cJSON* Route_McpBoolSchema(void)
{
  cJSON* s = cJSON_CreateObject();
  cJSON_AddStringToObject(s, "type", "boolean");
  return s;
}

static cJSON* Route_McpStringSchema(void)
{
  cJSON* s = cJSON_CreateObject();
  cJSON_AddStringToObject(s, "type", "string");
  return s;
}

static cJSON* Route_McpEnumStringSchema(const char* const* values, size_t count)
{
  cJSON* s = cJSON_CreateObject();
  cJSON_AddStringToObject(s, "type", "string");
  cJSON* arr = cJSON_CreateArray();
  for (size_t i = 0; i < count; i++) {
    cJSON_AddItemToArray(arr, cJSON_CreateString(values[i]));
  }
  cJSON_AddItemToObject(s, "enum", arr);
  return s;
}

static cJSON* Route_McpPinsArraySchema(void)
{
  cJSON* s = cJSON_CreateObject();
  cJSON_AddStringToObject(s, "type", "array");
  cJSON_AddNumberToObject(s, "minItems", 1);
  cJSON_AddItemToObject(s, "items", Route_McpIntSchema(0, 7));
  return s;
}

static void Route_McpAddLockIdProp(cJSON* props)
{
  Route_McpAddProp(props, "lockId", Route_McpStringSchema());
}

static cJSON* Route_McpToolDef(const char* name, const char* description, cJSON* inputSchema)
{
  cJSON* tool = cJSON_CreateObject();
  cJSON_AddStringToObject(tool, "name", name);
  cJSON_AddStringToObject(tool, "description", description);
  cJSON_AddItemToObject(tool, "inputSchema", inputSchema);
  return tool;
}

static cJSON* Route_McpEmptyObjectSchema(void)
{
  cJSON* schema = cJSON_CreateObject();
  cJSON_AddStringToObject(schema, "type", "object");
  cJSON_AddItemToObject(schema, "properties", cJSON_CreateObject());
  cJSON* required = cJSON_CreateArray();
  cJSON_AddItemToObject(schema, "required", required);
  return schema;
}

static cJSON* Route_McpObjectSchema(cJSON* props, const char* const* requiredNames, size_t requiredCount)
{
  cJSON* schema = cJSON_CreateObject();
  cJSON_AddStringToObject(schema, "type", "object");
  cJSON_AddItemToObject(schema, "properties", props);
  cJSON* required = cJSON_CreateArray();
  for (size_t i = 0; i < requiredCount; i++) {
    cJSON_AddItemToArray(required, cJSON_CreateString(requiredNames[i]));
  }
  cJSON_AddItemToObject(schema, "required", required);
  return schema;
}

static cJSON* Route_McpToolsListResult(void)
{
  static const char* modes[] = {"disable", "digitalInput", "digitalOutput", "digitalInputOutput", "pwmOutput"};
  static const char* edges[] = {"raising", "falling", "both"};
  static const char* rails[] = {"3v3", "5v"};
  static const char* methods[] = {"read", "write"};

  cJSON* tools = cJSON_CreateArray();

  cJSON_AddItemToArray(tools, Route_McpToolDef("list_pins", "List logical pins 0-7 with mode, pulls, and level.",
                                               Route_McpEmptyObjectSchema()));

  {
    cJSON* props = cJSON_CreateObject();
    Route_McpAddProp(props, "pin", Route_McpIntSchema(0, 7));
    Route_McpAddProp(props, "pins", Route_McpPinsArraySchema());
    Route_McpAddProp(props, "mode", Route_McpEnumStringSchema(modes, TOOL_GET_ARRAY_LENGTH(modes)));
    Route_McpAddProp(props, "pullUp", Route_McpBoolSchema());
    Route_McpAddProp(props, "pullDown", Route_McpBoolSchema());
    Route_McpAddProp(props, "openDrain", Route_McpBoolSchema());
    Route_McpAddLockIdProp(props);
    static const char* req[] = {"mode", "pullUp", "pullDown"};
    cJSON_AddItemToArray(tools, Route_McpToolDef("configure_pin",
                                                 "Configure one pin or batch pins (exactly one of pin or pins).",
                                                 Route_McpObjectSchema(props, req, TOOL_GET_ARRAY_LENGTH(req))));
  }

  {
    cJSON* props = cJSON_CreateObject();
    Route_McpAddProp(props, "pin", Route_McpIntSchema(0, 7));
    Route_McpAddProp(props, "pins", Route_McpPinsArraySchema());
    Route_McpAddLockIdProp(props);
    cJSON_AddItemToArray(tools, Route_McpToolDef("get_pin_level", "Read digital level for one pin or batch pins.",
                                                 Route_McpObjectSchema(props, NULL, 0)));
  }

  {
    cJSON* props = cJSON_CreateObject();
    Route_McpAddProp(props, "pin", Route_McpIntSchema(0, 7));
    Route_McpAddProp(props, "pins", Route_McpPinsArraySchema());
    cJSON* level = Route_McpIntSchema(0, 1);
    Route_McpAddProp(props, "level", level);
    Route_McpAddLockIdProp(props);
    static const char* req[] = {"level"};
    cJSON_AddItemToArray(tools, Route_McpToolDef("set_pin_level", "Write digital level for one pin or batch pins.",
                                                 Route_McpObjectSchema(props, req, 1)));
  }

  {
    cJSON* props = cJSON_CreateObject();
    Route_McpAddProp(props, "pin", Route_McpIntSchema(0, 7));
    Route_McpAddProp(props, "pins", Route_McpPinsArraySchema());
    Route_McpAddProp(props, "width", Route_McpIntSchema(1, 1000000));
    Route_McpAddProp(props, "level", Route_McpIntSchema(0, 1));
    Route_McpAddLockIdProp(props);
    static const char* req[] = {"width", "level"};
    cJSON_AddItemToArray(tools, Route_McpToolDef("pulse_pin", "Pulse one pin or batch pins for width microseconds.",
                                                 Route_McpObjectSchema(props, req, 2)));
  }

  {
    cJSON* props = cJSON_CreateObject();
    Route_McpAddProp(props, "pin", Route_McpIntSchema(0, 7));
    Route_McpAddLockIdProp(props);
    static const char* req[] = {"pin"};
    cJSON_AddItemToArray(tools, Route_McpToolDef("get_pin_pwm", "Read PWM frequency and duty (pwmOutput only).",
                                                 Route_McpObjectSchema(props, req, 1)));
  }

  {
    cJSON* props = cJSON_CreateObject();
    Route_McpAddProp(props, "pin", Route_McpIntSchema(0, 7));
    Route_McpAddProp(props, "frequency", Route_McpNumberSchema(1, 50000));
    Route_McpAddProp(props, "duty", Route_McpNumberSchema(0, 100));
    Route_McpAddLockIdProp(props);
    static const char* req[] = {"pin", "frequency", "duty"};
    cJSON_AddItemToArray(tools, Route_McpToolDef("set_pin_pwm", "Set PWM frequency and duty (pwmOutput only).",
                                                 Route_McpObjectSchema(props, req, 3)));
  }

  {
    cJSON* props = cJSON_CreateObject();
    Route_McpAddProp(props, "pin", Route_McpIntSchema(0, 7));
    Route_McpAddProp(props, "pins", Route_McpPinsArraySchema());
    Route_McpAddProp(props, "edge", Route_McpEnumStringSchema(edges, TOOL_GET_ARRAY_LENGTH(edges)));
    Route_McpAddProp(props, "duration", Route_McpIntSchema(1, 60000000));
    Route_McpAddLockIdProp(props);
    cJSON_AddItemToArray(tools, Route_McpToolDef("trace_pin", "Capture edge events on one pin or batch pins.",
                                                 Route_McpObjectSchema(props, NULL, 0)));
  }

  {
    cJSON* props = cJSON_CreateObject();
    Route_McpAddProp(props, "rail", Route_McpEnumStringSchema(rails, TOOL_GET_ARRAY_LENGTH(rails)));
    Route_McpAddLockIdProp(props);
    static const char* req[] = {"rail"};
    cJSON_AddItemToArray(tools, Route_McpToolDef("get_output_power_state", "Read 3v3 or 5v rail enable state.",
                                                 Route_McpObjectSchema(props, req, 1)));
  }

  {
    cJSON* props = cJSON_CreateObject();
    Route_McpAddProp(props, "rail", Route_McpEnumStringSchema(rails, TOOL_GET_ARRAY_LENGTH(rails)));
    Route_McpAddProp(props, "enable", Route_McpBoolSchema());
    Route_McpAddLockIdProp(props);
    static const char* req[] = {"rail", "enable"};
    cJSON_AddItemToArray(tools, Route_McpToolDef("set_output_power_state", "Set 3v3 or 5v rail enable state.",
                                                 Route_McpObjectSchema(props, req, 2)));
  }

  {
    cJSON* methodSchema = cJSON_CreateObject();
    cJSON_AddStringToObject(methodSchema, "type", "array");
    cJSON_AddNumberToObject(methodSchema, "minItems", 1);
    cJSON_AddItemToObject(methodSchema, "items", Route_McpEnumStringSchema(methods, TOOL_GET_ARRAY_LENGTH(methods)));

    cJSON* pinRes = cJSON_CreateObject();
    cJSON_AddStringToObject(pinRes, "type", "object");
    cJSON* pinProps = cJSON_CreateObject();
    Route_McpAddProp(pinProps, "pin", Route_McpIntSchema(0, 7));
    Route_McpAddProp(pinProps, "method", cJSON_Duplicate(methodSchema, 1));
    cJSON_AddItemToObject(pinRes, "properties", pinProps);
    cJSON* pinReq = cJSON_CreateArray();
    cJSON_AddItemToArray(pinReq, cJSON_CreateString("pin"));
    cJSON_AddItemToArray(pinReq, cJSON_CreateString("method"));
    cJSON_AddItemToObject(pinRes, "required", pinReq);

    cJSON* powerRes = cJSON_CreateObject();
    cJSON_AddStringToObject(powerRes, "type", "object");
    cJSON* powerProps = cJSON_CreateObject();
    Route_McpAddProp(powerProps, "power", Route_McpEnumStringSchema(rails, TOOL_GET_ARRAY_LENGTH(rails)));
    Route_McpAddProp(powerProps, "method", methodSchema);
    cJSON_AddItemToObject(powerRes, "properties", powerProps);
    cJSON* powerReq = cJSON_CreateArray();
    cJSON_AddItemToArray(powerReq, cJSON_CreateString("power"));
    cJSON_AddItemToArray(powerReq, cJSON_CreateString("method"));
    cJSON_AddItemToObject(powerRes, "required", powerReq);

    cJSON* oneOf = cJSON_CreateArray();
    cJSON_AddItemToArray(oneOf, pinRes);
    cJSON_AddItemToArray(oneOf, powerRes);

    cJSON* resources = cJSON_CreateObject();
    cJSON_AddStringToObject(resources, "type", "array");
    cJSON_AddNumberToObject(resources, "minItems", 1);
    cJSON_AddItemToObject(resources, "items", oneOf);

    cJSON* props = cJSON_CreateObject();
    Route_McpAddProp(props, "resources", resources);
    static const char* req[] = {"resources"};
    cJSON_AddItemToArray(tools, Route_McpToolDef("create_lock", "Create a resource lock for pins and/or power rails.",
                                                 Route_McpObjectSchema(props, req, 1)));
  }

  {
    cJSON* props = cJSON_CreateObject();
    Route_McpAddProp(props, "id", Route_McpStringSchema());
    static const char* req[] = {"id"};
    cJSON_AddItemToArray(tools, Route_McpToolDef("renew_lock", "Renew a lock TTL by id.",
                                                 Route_McpObjectSchema(props, req, 1)));
  }

  {
    cJSON* props = cJSON_CreateObject();
    Route_McpAddProp(props, "id", Route_McpStringSchema());
    static const char* req[] = {"id"};
    cJSON_AddItemToArray(tools, Route_McpToolDef("delete_lock", "Delete a lock by id (idempotent).",
                                                 Route_McpObjectSchema(props, req, 1)));
  }

  cJSON* result = cJSON_CreateObject();
  cJSON_AddItemToObject(result, "tools", tools);
  return result;
}

static cJSON* Route_McpCallListPins(void)
{
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
  return Route_McpToolResultOk(root);
}

static cJSON* Route_McpCallConfigurePin(cJSON* args)
{
  char reason[256];
  Mcp_PinSelection sel;
  if (!Route_McpParsePinSelection(args, &sel, reason, sizeof(reason))) {
    return Route_McpToolResultErr(reason);
  }
  GpioCtrl_Mode mode;
  bool openDrain = false;
  bool pullUp = false;
  bool pullDown = false;
  if (HttpServer_ParsePinConfigBody(args, &mode, &openDrain, &pullUp, &pullDown, reason, sizeof(reason)) != ESP_OK) {
    return Route_McpToolResultErr(reason);
  }
  const char* lockId = Route_McpGetLockId(args);
  if (Route_McpCheckPinsLock(&sel, LOCK_METHOD_WRITE, lockId, reason, sizeof(reason))) {
    return Route_McpToolResultErr(reason);
  }
  for (size_t i = 0; i < sel.count; i++) {
    esp_err_t cfg = GpioCtrl_SetConfig(sel.pins[i], mode, openDrain, pullUp, pullDown);
    if (cfg == ESP_ERR_NO_MEM) {
      return Route_McpToolResultErr(
          "No free LEDC channel or timer for pwmOutput. Free another pwmOutput pin or reuse an existing frequency.");
    }
    if (cfg != ESP_OK) return Route_McpToolResultErr("internal");
  }
  Lock_Touch(lockId);
  return Route_McpToolResultOkEmpty();
}

static cJSON* Route_McpCallGetPinLevel(cJSON* args)
{
  char reason[256];
  Mcp_PinSelection sel;
  if (!Route_McpParsePinSelection(args, &sel, reason, sizeof(reason))) {
    return Route_McpToolResultErr(reason);
  }
  const char* lockId = Route_McpGetLockId(args);
  if (Route_McpCheckPinsLock(&sel, LOCK_METHOD_READ, lockId, reason, sizeof(reason))) {
    return Route_McpToolResultErr(reason);
  }
  cJSON* root = cJSON_CreateObject();
  if (!sel.batch) {
    int level = 0;
    if (GpioCtrl_GetLevel(sel.pins[0], &level) != ESP_OK) {
      cJSON_Delete(root);
      return Route_McpToolResultErr("internal");
    }
    cJSON_AddNumberToObject(root, "level", level);
  } else {
    cJSON* levels = cJSON_CreateArray();
    for (size_t i = 0; i < sel.count; i++) {
      int level = 0;
      GpioCtrl_GetLevel(sel.pins[i], &level);
      cJSON_AddItemToArray(levels, cJSON_CreateNumber(level));
    }
    cJSON_AddItemToObject(root, "levels", levels);
  }
  cJSON_AddNumberToObject(root, "time", (double)HttpServer_NowUs());
  Lock_Touch(lockId);
  return Route_McpToolResultOk(root);
}

static cJSON* Route_McpCallSetPinLevel(cJSON* args)
{
  char reason[256];
  Mcp_PinSelection sel;
  if (!Route_McpParsePinSelection(args, &sel, reason, sizeof(reason))) {
    return Route_McpToolResultErr(reason);
  }
  cJSON* levelItem = cJSON_GetObjectItem(args, "level");
  if (!cJSON_IsNumber(levelItem) || (levelItem->valueint != 0 && levelItem->valueint != 1)) {
    return Route_McpToolResultErr("level must be 0 or 1.");
  }
  int level = levelItem->valueint;
  const char* lockId = Route_McpGetLockId(args);
  if (Route_McpCheckPinsLock(&sel, LOCK_METHOD_WRITE, lockId, reason, sizeof(reason))) {
    return Route_McpToolResultErr(reason);
  }
  for (size_t i = 0; i < sel.count; i++) {
    if (!GpioCtrl_IsOutputCapable(sel.pins[i])) {
      snprintf(reason, sizeof(reason),
               "Pin %d cannot write level in its current mode. PUT /pin/%d with mode digitalOutput or "
               "digitalInputOutput first.",
               sel.pins[i], sel.pins[i]);
      return Route_McpToolResultErr(reason);
    }
  }
  for (size_t i = 0; i < sel.count; i++) {
    if (GpioCtrl_SetLevel(sel.pins[i], level) != ESP_OK) return Route_McpToolResultErr("internal");
  }
  Lock_Touch(lockId);
  return Route_McpToolResultOkEmpty();
}

static cJSON* Route_McpCallPulsePin(cJSON* args)
{
  char reason[256];
  Mcp_PinSelection sel;
  if (!Route_McpParsePinSelection(args, &sel, reason, sizeof(reason))) {
    return Route_McpToolResultErr(reason);
  }
  cJSON* widthItem = cJSON_GetObjectItem(args, "width");
  cJSON* levelItem = cJSON_GetObjectItem(args, "level");
  if (!cJSON_IsNumber(widthItem) || widthItem->valuedouble < 1 ||
      widthItem->valuedouble > (double)CONFIG_GPIO_PULSE_MAX_WIDTH_US) {
    return Route_McpToolResultErr("width must be an integer from 1 to 1000000 microseconds.");
  }
  if (!cJSON_IsNumber(levelItem) || (levelItem->valueint != 0 && levelItem->valueint != 1)) {
    return Route_McpToolResultErr("level must be 0 or 1.");
  }
  uint64_t width = (uint64_t)widthItem->valuedouble;
  int level = levelItem->valueint;
  const char* lockId = Route_McpGetLockId(args);
  if (Route_McpCheckPinsLock(&sel, LOCK_METHOD_WRITE, lockId, reason, sizeof(reason))) {
    return Route_McpToolResultErr(reason);
  }
  for (size_t i = 0; i < sel.count; i++) {
    if (!GpioCtrl_IsOutputCapable(sel.pins[i])) {
      snprintf(reason, sizeof(reason),
               "Pin %d cannot write level in its current mode. PUT /pin/%d with mode digitalOutput or "
               "digitalInputOutput first.",
               sel.pins[i], sel.pins[i]);
      return Route_McpToolResultErr(reason);
    }
  }
  for (size_t i = 0; i < sel.count; i++) {
    if (GpioCtrl_Pulse(sel.pins[i], level, width) != ESP_OK) return Route_McpToolResultErr("internal");
  }
  Lock_Touch(lockId);
  return Route_McpToolResultOkEmpty();
}

static cJSON* Route_McpCallGetPinPwm(cJSON* args)
{
  char reason[256];
  cJSON* pinItem = cJSON_GetObjectItem(args, "pin");
  if (!cJSON_IsNumber(pinItem) || !GpioCtrl_IsValidLogicalPin(pinItem->valueint)) {
    char range[32];
    HttpServer_FormatPinRange(range, sizeof(range));
    snprintf(reason, sizeof(reason), "pin is invalid. Use a pin from %s.", range);
    return Route_McpToolResultErr(reason);
  }
  int pin = pinItem->valueint;
  const char* lockId = Route_McpGetLockId(args);
  if (HttpServer_LockStatusId(lockId, LOCK_KIND_GPIO, pin, LOCK_METHOD_READ, reason, sizeof(reason))) {
    return Route_McpToolResultErr(reason);
  }
  if (!GpioCtrl_IsPwmMode(pin)) {
    snprintf(reason, sizeof(reason), "Pin %d is not in pwmOutput mode. PUT /pin/%d with mode pwmOutput first.", pin, pin);
    return Route_McpToolResultErr(reason);
  }
  double frequency = 0;
  double duty = 0;
  if (GpioCtrl_GetPwm(pin, &frequency, &duty) != ESP_OK) return Route_McpToolResultErr("internal");
  cJSON* root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "frequency", frequency);
  cJSON_AddNumberToObject(root, "duty", duty);
  cJSON_AddNumberToObject(root, "time", (double)HttpServer_NowUs());
  Lock_Touch(lockId);
  return Route_McpToolResultOk(root);
}

static cJSON* Route_McpCallSetPinPwm(cJSON* args)
{
  char reason[256];
  cJSON* pinItem = cJSON_GetObjectItem(args, "pin");
  if (!cJSON_IsNumber(pinItem) || !GpioCtrl_IsValidLogicalPin(pinItem->valueint)) {
    char range[32];
    HttpServer_FormatPinRange(range, sizeof(range));
    snprintf(reason, sizeof(reason), "pin is invalid. Use a pin from %s.", range);
    return Route_McpToolResultErr(reason);
  }
  int pin = pinItem->valueint;
  cJSON* freqItem = cJSON_GetObjectItem(args, "frequency");
  cJSON* dutyItem = cJSON_GetObjectItem(args, "duty");
  if (!cJSON_IsNumber(freqItem) || freqItem->valuedouble < 1 || freqItem->valuedouble > CONFIG_GPIO_PWM_MAX_FREQ_HZ) {
    return Route_McpToolResultErr("frequency must be a number from 1 to 50000.");
  }
  if (!cJSON_IsNumber(dutyItem) || dutyItem->valuedouble < 0 || dutyItem->valuedouble > 100) {
    return Route_McpToolResultErr("duty must be a number from 0 to 100.");
  }
  const char* lockId = Route_McpGetLockId(args);
  if (HttpServer_LockStatusId(lockId, LOCK_KIND_GPIO, pin, LOCK_METHOD_WRITE, reason, sizeof(reason))) {
    return Route_McpToolResultErr(reason);
  }
  if (!GpioCtrl_IsPwmMode(pin)) {
    snprintf(reason, sizeof(reason), "Pin %d is not in pwmOutput mode. PUT /pin/%d with mode pwmOutput first.", pin, pin);
    return Route_McpToolResultErr(reason);
  }
  esp_err_t pwm = GpioCtrl_SetPwm(pin, freqItem->valuedouble, dutyItem->valuedouble);
  if (pwm == ESP_ERR_NO_MEM) {
    return Route_McpToolResultErr(
        "No free LEDC channel or timer for pwmOutput. Free another pwmOutput pin or reuse an existing frequency.");
  }
  if (pwm != ESP_OK) return Route_McpToolResultErr("internal");
  Lock_Touch(lockId);
  return Route_McpToolResultOkEmpty();
}

static cJSON* Route_McpCallTracePin(cJSON* args)
{
  char reason[256];
  Mcp_PinSelection sel;
  if (!Route_McpParsePinSelection(args, &sel, reason, sizeof(reason))) {
    return Route_McpToolResultErr(reason);
  }
  GpioCtrl_Edge edge = GPIO_CTRL_EDGE_BOTH;
  cJSON* edgeItem = cJSON_GetObjectItem(args, "edge");
  if (cJSON_IsString(edgeItem)) {
    if (!GpioCtrl_EdgeFromString(edgeItem->valuestring, &edge)) {
      return Route_McpToolResultErr("edge is invalid. Use one of: raising, falling, both.");
    }
  }
  uint64_t duration = TRACE_DEFAULT_DURATION_US;
  cJSON* durationItem = cJSON_GetObjectItem(args, "duration");
  if (cJSON_IsNumber(durationItem)) {
    duration = (uint64_t)durationItem->valuedouble;
  }
  if (duration < 1 || duration > CONFIG_GPIO_TRACE_MAX_DURATION_US) {
    return Route_McpToolResultErr("duration must be an integer from 1 to 60000000 microseconds (max 60 seconds).");
  }
  const char* lockId = Route_McpGetLockId(args);
  if (Route_McpCheckPinsLock(&sel, LOCK_METHOD_READ | LOCK_METHOD_WRITE, lockId, reason, sizeof(reason))) {
    return Route_McpToolResultErr(reason);
  }
  GpioCtrl_TraceEvent* events = calloc(CONFIG_GPIO_TRACE_MAX_EVENTS, sizeof(GpioCtrl_TraceEvent));
  if (events == NULL) return Route_McpToolResultErr("internal");
  size_t eventCount = 0;
  bool includePin = sel.batch;
  if (GpioCtrl_Trace(sel.pins, sel.count, edge, duration, events, CONFIG_GPIO_TRACE_MAX_EVENTS, &eventCount,
                     includePin) != ESP_OK) {
    free(events);
    return Route_McpToolResultErr("internal");
  }
  cJSON* root = cJSON_CreateObject();
  cJSON* arr = cJSON_CreateArray();
  for (size_t i = 0; i < eventCount; i++) {
    cJSON* ev = cJSON_CreateObject();
    if (includePin) cJSON_AddNumberToObject(ev, "pin", events[i].pin);
    cJSON_AddStringToObject(ev, "edge", events[i].edge);
    cJSON_AddNumberToObject(ev, "level", events[i].level);
    cJSON_AddNumberToObject(ev, "time", (double)events[i].time);
    cJSON_AddItemToArray(arr, ev);
  }
  cJSON_AddItemToObject(root, "events", arr);
  free(events);
  Lock_Touch(lockId);
  return Route_McpToolResultOk(root);
}

static Lock_Kind Route_McpPowerLockKind(GpioCtrl_PowerRail rail)
{
  return rail == GPIO_CTRL_POWER_3V3 ? LOCK_KIND_POWER_3V3 : LOCK_KIND_POWER_5V;
}

static bool Route_McpParseRail(cJSON* args, GpioCtrl_PowerRail* railOut, char* reason, size_t reasonLen)
{
  cJSON* railItem = cJSON_GetObjectItem(args, "rail");
  if (!cJSON_IsString(railItem) || !GpioCtrl_PowerRailFromString(railItem->valuestring, railOut)) {
    snprintf(reason, reasonLen, "rail is invalid. Use one of: 3v3, 5v.");
    return false;
  }
  return true;
}

static cJSON* Route_McpCallGetOutputPowerState(cJSON* args)
{
  char reason[256];
  GpioCtrl_PowerRail rail;
  if (!Route_McpParseRail(args, &rail, reason, sizeof(reason))) return Route_McpToolResultErr(reason);
  const char* lockId = Route_McpGetLockId(args);
  if (HttpServer_LockStatusId(lockId, Route_McpPowerLockKind(rail), 0, LOCK_METHOD_READ, reason, sizeof(reason))) {
    return Route_McpToolResultErr(reason);
  }
  bool enable = false;
  if (GpioCtrl_GetPowerEnable(rail, &enable) != ESP_OK) return Route_McpToolResultErr("internal");
  cJSON* root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "enable", enable);
  cJSON_AddNumberToObject(root, "time", (double)HttpServer_NowUs());
  Lock_Touch(lockId);
  return Route_McpToolResultOk(root);
}

static cJSON* Route_McpCallSetOutputPowerState(cJSON* args)
{
  char reason[256];
  GpioCtrl_PowerRail rail;
  if (!Route_McpParseRail(args, &rail, reason, sizeof(reason))) return Route_McpToolResultErr(reason);
  cJSON* enableItem = cJSON_GetObjectItem(args, "enable");
  if (!cJSON_IsBool(enableItem)) return Route_McpToolResultErr("enable must be boolean.");
  const char* lockId = Route_McpGetLockId(args);
  if (HttpServer_LockStatusId(lockId, Route_McpPowerLockKind(rail), 0, LOCK_METHOD_WRITE, reason, sizeof(reason))) {
    return Route_McpToolResultErr(reason);
  }
  if (GpioCtrl_SetPowerEnable(rail, cJSON_IsTrue(enableItem)) != ESP_OK) return Route_McpToolResultErr("internal");
  Lock_Touch(lockId);
  return Route_McpToolResultOkEmpty();
}

static cJSON* Route_McpCallCreateLock(cJSON* args)
{
  cJSON* resources = cJSON_GetObjectItem(args, "resources");
  if (!cJSON_IsArray(resources) || cJSON_GetArraySize(resources) == 0) {
    return Route_McpToolResultErr("resources must be a non-empty array.");
  }
  Lock_Resource res[LOCK_MAX_RESOURCES];
  size_t count = (size_t)cJSON_GetArraySize(resources);
  if (count > LOCK_MAX_RESOURCES) {
    return Route_McpToolResultErr("resources array is too long.");
  }
  char range[32];
  HttpServer_FormatPinRange(range, sizeof(range));
  char reason[192];
  for (size_t i = 0; i < count; i++) {
    cJSON* item = cJSON_GetArrayItem(resources, (int)i);
    cJSON* pinItem = cJSON_GetObjectItem(item, "pin");
    cJSON* powerItem = cJSON_GetObjectItem(item, "power");
    cJSON* methods = cJSON_GetObjectItem(item, "method");
    bool hasPin = cJSON_IsNumber(pinItem);
    bool hasPower = cJSON_IsString(powerItem);
    if (hasPin == hasPower) {
      return Route_McpToolResultErr("Each resource must have exactly one of pin or power.");
    }
    Lock_Kind kind;
    int pin = 0;
    if (hasPin) {
      if (!GpioCtrl_IsValidLogicalPin(pinItem->valueint)) {
        snprintf(reason, sizeof(reason), "resources[%u].pin is invalid. Use a pin from %s.", (unsigned)i, range);
        return Route_McpToolResultErr(reason);
      }
      kind = LOCK_KIND_GPIO;
      pin = pinItem->valueint;
    } else {
      if (!Lock_PowerFromString(powerItem->valuestring, &kind)) {
        return Route_McpToolResultErr("power is invalid. Use one of: 3v3, 5v.");
      }
    }
    if (!cJSON_IsArray(methods) || cJSON_GetArraySize(methods) == 0) {
      return Route_McpToolResultErr("Each resource.method entry must be read or write.");
    }
    uint8_t bits = 0;
    for (int m = 0; m < cJSON_GetArraySize(methods); m++) {
      cJSON* mv = cJSON_GetArrayItem(methods, m);
      if (!cJSON_IsString(mv)) {
        return Route_McpToolResultErr("Each resource.method entry must be read or write.");
      }
      if (strcmp(mv->valuestring, "read") == 0)
        bits |= LOCK_METHOD_READ;
      else if (strcmp(mv->valuestring, "write") == 0)
        bits |= LOCK_METHOD_WRITE;
      else {
        return Route_McpToolResultErr("method is invalid. Each resource.method entry must be read or write.");
      }
    }
    res[i].kind = kind;
    res[i].pin = pin;
    res[i].methods = bits;
  }

  Lock_Entry created;
  esp_err_t cret = Lock_Create(res, count, &created);
  if (cret == ESP_ERR_INVALID_STATE) {
    if (res[0].kind == LOCK_KIND_GPIO) {
      snprintf(reason, sizeof(reason),
               "Cannot create lock: pin %d is already held. DELETE that lock or wait until it expires.", res[0].pin);
    } else {
      snprintf(reason, sizeof(reason),
               "Cannot create lock: power %s is already held. DELETE that lock or wait until it expires.",
               Lock_KindToString(res[0].kind));
    }
    for (size_t i = 0; i < count; i++) {
      if (res[i].kind == LOCK_KIND_GPIO) {
        snprintf(reason, sizeof(reason),
                 "Cannot create lock: pin %d is already held. DELETE that lock or wait until it expires.", res[i].pin);
      } else {
        snprintf(reason, sizeof(reason),
                 "Cannot create lock: power %s is already held. DELETE that lock or wait until it expires.",
                 Lock_KindToString(res[i].kind));
      }
      break;
    }
    return Route_McpToolResultErr(reason);
  }
  if (cret != ESP_OK) return Route_McpToolResultErr("internal");

  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "id", created.id);
  int64_t now = Lock_NowUs();
  cJSON_AddNumberToObject(root, "ttl", (double)(created.expiresAtUs - now));
  cJSON_AddNumberToObject(root, "expiresAt", (double)created.expiresAtUs);
  cJSON* resArr = cJSON_CreateArray();
  for (size_t i = 0; i < created.resourceCount; i++) {
    cJSON* r = cJSON_CreateObject();
    if (created.resources[i].kind == LOCK_KIND_GPIO) {
      cJSON_AddNumberToObject(r, "pin", created.resources[i].pin);
    } else {
      cJSON_AddStringToObject(r, "power", Lock_KindToString(created.resources[i].kind));
    }
    cJSON* methodsArr = cJSON_CreateArray();
    if (created.resources[i].methods & LOCK_METHOD_READ) {
      cJSON_AddItemToArray(methodsArr, cJSON_CreateString("read"));
    }
    if (created.resources[i].methods & LOCK_METHOD_WRITE) {
      cJSON_AddItemToArray(methodsArr, cJSON_CreateString("write"));
    }
    cJSON_AddItemToObject(r, "method", methodsArr);
    cJSON_AddItemToArray(resArr, r);
  }
  cJSON_AddItemToObject(root, "resources", resArr);
  return Route_McpToolResultOk(root);
}

static cJSON* Route_McpCallRenewLock(cJSON* args)
{
  cJSON* idItem = cJSON_GetObjectItem(args, "id");
  if (!cJSON_IsString(idItem) || idItem->valuestring == NULL || idItem->valuestring[0] == '\0') {
    return Route_McpToolResultErr("id is required.");
  }
  Lock_Entry entry;
  if (Lock_Renew(idItem->valuestring, &entry) != ESP_OK) {
    char reason[160];
    snprintf(reason, sizeof(reason),
             "Lock id %s does not exist. Create a lock with POST /lock (see GET /openapi.json).", idItem->valuestring);
    return Route_McpToolResultErr(reason);
  }
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "id", entry.id);
  int64_t now = Lock_NowUs();
  cJSON_AddNumberToObject(root, "ttl", (double)(entry.expiresAtUs - now));
  cJSON_AddNumberToObject(root, "expiresAt", (double)entry.expiresAtUs);
  return Route_McpToolResultOk(root);
}

static cJSON* Route_McpCallDeleteLock(cJSON* args)
{
  cJSON* idItem = cJSON_GetObjectItem(args, "id");
  if (!cJSON_IsString(idItem) || idItem->valuestring == NULL || idItem->valuestring[0] == '\0') {
    return Route_McpToolResultErr("id is required.");
  }
  Lock_Delete(idItem->valuestring);
  return Route_McpToolResultOkEmpty();
}

static cJSON* Route_McpHandleInitialize(cJSON* params)
{
  const char* negotiated = MCP_PROTOCOL_VERSION;
  if (cJSON_IsObject(params)) {
    cJSON* pv = cJSON_GetObjectItem(params, "protocolVersion");
    if (cJSON_IsString(pv) && pv->valuestring) {
      if (strcmp(pv->valuestring, "2025-06-18") == 0 || strcmp(pv->valuestring, "2025-03-26") == 0 ||
          strcmp(pv->valuestring, "2024-11-05") == 0) {
        negotiated = MCP_PROTOCOL_VERSION;
      }
    }
  }
  cJSON* result = cJSON_CreateObject();
  cJSON_AddStringToObject(result, "protocolVersion", negotiated);
  cJSON* caps = cJSON_CreateObject();
  cJSON_AddItemToObject(caps, "tools", cJSON_CreateObject());
  cJSON_AddItemToObject(result, "capabilities", caps);
  cJSON* info = cJSON_CreateObject();
  cJSON_AddStringToObject(info, "name", "saihub");
  cJSON_AddStringToObject(info, "version", "1.0.0");
  cJSON_AddItemToObject(result, "serverInfo", info);
  cJSON_AddStringToObject(
      result, "instructions",
      "Saihub GPIO hub: logical pins 0-7, times in microseconds, optional lockId for contested resources. "
      "Use tools/list then tools/call. Power rails are rail=3v3|5v.");
  return result;
}

static cJSON* Route_McpHandleToolsCall(cJSON* params)
{
  if (!cJSON_IsObject(params)) {
    return Route_McpToolResultErr("params are required.");
  }
  cJSON* nameItem = cJSON_GetObjectItem(params, "name");
  if (!cJSON_IsString(nameItem) || nameItem->valuestring == NULL) {
    return Route_McpToolResultErr("name is required.");
  }
  cJSON* args = cJSON_GetObjectItem(params, "arguments");
  if (args != NULL && !cJSON_IsObject(args)) {
    return Route_McpToolResultErr("arguments must be an object.");
  }
  if (args == NULL) args = cJSON_CreateObject();
  bool ownedArgs = (cJSON_GetObjectItem(params, "arguments") == NULL);
  const char* name = nameItem->valuestring;
  cJSON* out = NULL;
  if (strcmp(name, "list_pins") == 0)
    out = Route_McpCallListPins();
  else if (strcmp(name, "configure_pin") == 0)
    out = Route_McpCallConfigurePin(args);
  else if (strcmp(name, "get_pin_level") == 0)
    out = Route_McpCallGetPinLevel(args);
  else if (strcmp(name, "set_pin_level") == 0)
    out = Route_McpCallSetPinLevel(args);
  else if (strcmp(name, "pulse_pin") == 0)
    out = Route_McpCallPulsePin(args);
  else if (strcmp(name, "get_pin_pwm") == 0)
    out = Route_McpCallGetPinPwm(args);
  else if (strcmp(name, "set_pin_pwm") == 0)
    out = Route_McpCallSetPinPwm(args);
  else if (strcmp(name, "trace_pin") == 0)
    out = Route_McpCallTracePin(args);
  else if (strcmp(name, "get_output_power_state") == 0)
    out = Route_McpCallGetOutputPowerState(args);
  else if (strcmp(name, "set_output_power_state") == 0)
    out = Route_McpCallSetOutputPowerState(args);
  else if (strcmp(name, "create_lock") == 0)
    out = Route_McpCallCreateLock(args);
  else if (strcmp(name, "renew_lock") == 0)
    out = Route_McpCallRenewLock(args);
  else if (strcmp(name, "delete_lock") == 0)
    out = Route_McpCallDeleteLock(args);
  else {
    char reason[96];
    snprintf(reason, sizeof(reason), "Unknown tool: %s", name);
    out = Route_McpToolResultErr(reason);
  }
  if (ownedArgs) cJSON_Delete(args);
  return out;
}

static esp_err_t Route_McpDispatch(httpd_req_t* req, cJSON* msg)
{
  cJSON* jsonrpc = cJSON_GetObjectItem(msg, "jsonrpc");
  if (!cJSON_IsString(jsonrpc) || strcmp(jsonrpc->valuestring, "2.0") != 0) {
    return Route_McpSendJsonRpc(req, 400, Route_McpJsonRpcError(NULL, -32600, "Invalid Request"));
  }

  cJSON* id = cJSON_GetObjectItem(msg, "id");
  cJSON* methodItem = cJSON_GetObjectItem(msg, "method");
  bool hasMethod = cJSON_IsString(methodItem) && methodItem->valuestring != NULL;
  bool hasId = (id != NULL && !cJSON_IsNull(id));

  /* Client JSON-RPC response (has id, no method) or notification (method, no id) */
  if (!hasMethod) {
    if (!hasId) {
      return Route_McpSendJsonRpc(req, 400, Route_McpJsonRpcError(NULL, -32600, "Invalid Request"));
    }
    return Route_McpSendStatus(req, 202, "202 Accepted", NULL, NULL);
  }

  const char* method = methodItem->valuestring;
  if (!hasId) {
    /* notification */
    if (strcmp(method, "notifications/initialized") == 0 || strcmp(method, "notifications/cancelled") == 0) {
      return Route_McpSendStatus(req, 202, "202 Accepted", NULL, NULL);
    }
    return Route_McpSendStatus(req, 202, "202 Accepted", NULL, NULL);
  }

  Lock_SweepExpired();
  cJSON* params = cJSON_GetObjectItem(msg, "params");

  if (strcmp(method, "initialize") == 0) {
    return Route_McpSendJsonRpc(req, 200, Route_McpJsonRpcResult(id, Route_McpHandleInitialize(params)));
  }
  if (strcmp(method, "ping") == 0) {
    return Route_McpSendJsonRpc(req, 200, Route_McpJsonRpcResult(id, cJSON_CreateObject()));
  }
  if (strcmp(method, "tools/list") == 0) {
    return Route_McpSendJsonRpc(req, 200, Route_McpJsonRpcResult(id, Route_McpToolsListResult()));
  }
  if (strcmp(method, "tools/call") == 0) {
    return Route_McpSendJsonRpc(req, 200, Route_McpJsonRpcResult(id, Route_McpHandleToolsCall(params)));
  }

  return Route_McpSendJsonRpc(req, 200, Route_McpJsonRpcError(id, -32601, "Method not found"));
}

static esp_err_t Route_McpPostHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  HttpServer_SetCors(req);

  if (!Route_McpOriginOk(req)) {
    return Route_McpSendStatus(req, 403, "403 Forbidden", NULL, NULL);
  }
  if (!Route_McpAcceptOk(req)) {
    return Route_McpSendJsonRpc(req, 406,
                                Route_McpJsonRpcError(NULL, -32600, "Accept must include application/json or text/event-stream"));
  }
  if (!HttpServer_HasJsonContentType(req)) {
    return Route_McpSendJsonRpc(req, 415, Route_McpJsonRpcError(NULL, -32600, "Content-Type must be application/json"));
  }
  if (!Route_McpProtocolVersionOk(req, false)) {
    return Route_McpSendJsonRpc(req, 400, Route_McpJsonRpcError(NULL, -32600, "Unsupported MCP-Protocol-Version"));
  }

  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(req, &perr);
  if (body == NULL) {
    return Route_McpSendJsonRpc(req, 400, Route_McpJsonRpcError(NULL, -32700, "Parse error"));
  }
  if (cJSON_IsArray(body)) {
    cJSON_Delete(body);
    return Route_McpSendJsonRpc(req, 400, Route_McpJsonRpcError(NULL, -32600, "JSON-RPC batch not supported"));
  }
  if (!cJSON_IsObject(body)) {
    cJSON_Delete(body);
    return Route_McpSendJsonRpc(req, 400, Route_McpJsonRpcError(NULL, -32600, "Invalid Request"));
  }

  esp_err_t ret = Route_McpDispatch(req, body);
  cJSON_Delete(body);
  return ret;
}

static esp_err_t Route_McpGetHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  HttpServer_SetCors(req);
  if (!Route_McpOriginOk(req)) {
    return Route_McpSendStatus(req, 403, "403 Forbidden", NULL, NULL);
  }
  return Route_McpSendStatus(req, 405, "405 Method Not Allowed", NULL, NULL);
}

static esp_err_t Route_McpDeleteHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  HttpServer_SetCors(req);
  if (!Route_McpOriginOk(req)) {
    return Route_McpSendStatus(req, 403, "403 Forbidden", NULL, NULL);
  }
  return Route_McpSendStatus(req, 405, "405 Method Not Allowed", NULL, NULL);
}

static esp_err_t Route_McpOptionsHandler(httpd_req_t* req)
{
  return HttpServer_SendOptions(req);
}

static const httpd_uri_t uris[] = {
    {.uri = "/mcp", .method = HTTP_POST, .handler = Route_McpPostHandler},
    {.uri = "/mcp", .method = HTTP_GET, .handler = Route_McpGetHandler},
    {.uri = "/mcp", .method = HTTP_DELETE, .handler = Route_McpDeleteHandler},
    {.uri = "/mcp", .method = HTTP_OPTIONS, .handler = Route_McpOptionsHandler},
};

esp_err_t Route_McpRegister(httpd_handle_t server)
{
  for (size_t i = 0; i < TOOL_GET_ARRAY_LENGTH(uris); i++) {
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(httpd_register_uri_handler(server, &uris[i]), "register mcp uri failed");
  }
  return ESP_OK;
}
