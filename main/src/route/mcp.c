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

static const int mcpLogicalToHw[] = CONFIG_GPIO_LOGICAL_TO_HW;

typedef struct {
  int pins[TOOL_GET_ARRAY_LENGTH(mcpLogicalToHw)];
  size_t count;
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
  size_t maxPins = (size_t)GpioCtrl_GetLogicalCount();
  if (maxPins > TOOL_GET_ARRAY_LENGTH(out->pins)) maxPins = TOOL_GET_ARRAY_LENGTH(out->pins);
  if (HttpServer_ParsePinsArray(args, out->pins, maxPins, &out->count, reason, reasonLen) != ESP_OK) {
    return false;
  }
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
  static const char* lockTypes[] = {"pin", "power"};

  cJSON* tools = cJSON_CreateArray();

  cJSON_AddItemToArray(tools, Route_McpToolDef("list_pins", "List logical pins 0-7 with mode, pulls, and level.",
                                               Route_McpEmptyObjectSchema()));

  {
    cJSON* props = cJSON_CreateObject();
    Route_McpAddProp(props, "pins", Route_McpPinsArraySchema());
    Route_McpAddProp(props, "mode", Route_McpEnumStringSchema(modes, TOOL_GET_ARRAY_LENGTH(modes)));
    Route_McpAddProp(props, "pullUp", Route_McpBoolSchema());
    Route_McpAddProp(props, "pullDown", Route_McpBoolSchema());
    Route_McpAddProp(props, "openDrain", Route_McpBoolSchema());
    Route_McpAddLockIdProp(props);
    static const char* req[] = {"pins", "mode", "pullUp", "pullDown"};
    cJSON_AddItemToArray(tools, Route_McpToolDef("configure_pins",
                                                 "Configure one or more pins. Always pass pins (e.g. pins:[1]).",
                                                 Route_McpObjectSchema(props, req, TOOL_GET_ARRAY_LENGTH(req))));
  }

  {
    cJSON* props = cJSON_CreateObject();
    Route_McpAddProp(props, "pins", Route_McpPinsArraySchema());
    Route_McpAddLockIdProp(props);
    static const char* req[] = {"pins"};
    cJSON_AddItemToArray(tools,
                         Route_McpToolDef("get_pin_levels",
                                          "Read digital levels for pins. Returns levels array in pins order.",
                                          Route_McpObjectSchema(props, req, 1)));
  }

  {
    cJSON* props = cJSON_CreateObject();
    Route_McpAddProp(props, "pins", Route_McpPinsArraySchema());
    Route_McpAddProp(props, "level", Route_McpIntSchema(0, 1));
    Route_McpAddLockIdProp(props);
    static const char* req[] = {"pins", "level"};
    cJSON_AddItemToArray(tools, Route_McpToolDef("set_pin_levels", "Write digital level for one or more pins.",
                                                 Route_McpObjectSchema(props, req, 2)));
  }

  {
    cJSON* props = cJSON_CreateObject();
    Route_McpAddProp(props, "pins", Route_McpPinsArraySchema());
    Route_McpAddProp(props, "width", Route_McpIntSchema(1, 1000000));
    Route_McpAddProp(props, "level", Route_McpIntSchema(0, 1));
    Route_McpAddLockIdProp(props);
    static const char* req[] = {"pins", "width", "level"};
    cJSON_AddItemToArray(tools, Route_McpToolDef("pulse_pins", "Pulse one or more pins for width microseconds.",
                                                 Route_McpObjectSchema(props, req, 3)));
  }

  {
    cJSON* props = cJSON_CreateObject();
    Route_McpAddProp(props, "pins", Route_McpPinsArraySchema());
    Route_McpAddLockIdProp(props);
    static const char* req[] = {"pins"};
    cJSON_AddItemToArray(tools, Route_McpToolDef("get_pin_pwms", "Read PWM frequency and duty for pins (pwmOutput only).",
                                                 Route_McpObjectSchema(props, req, 1)));
  }

  {
    cJSON* props = cJSON_CreateObject();
    Route_McpAddProp(props, "pins", Route_McpPinsArraySchema());
    Route_McpAddProp(props, "frequency", Route_McpNumberSchema(1, 50000));
    Route_McpAddProp(props, "duty", Route_McpNumberSchema(0, 100));
    Route_McpAddLockIdProp(props);
    static const char* req[] = {"pins", "frequency", "duty"};
    cJSON_AddItemToArray(tools, Route_McpToolDef("set_pin_pwms", "Set PWM frequency and duty for pins (pwmOutput only).",
                                                 Route_McpObjectSchema(props, req, 3)));
  }

  {
    cJSON* props = cJSON_CreateObject();
    Route_McpAddProp(props, "pins", Route_McpPinsArraySchema());
    Route_McpAddProp(props, "edge", Route_McpEnumStringSchema(edges, TOOL_GET_ARRAY_LENGTH(edges)));
    Route_McpAddProp(props, "duration", Route_McpIntSchema(1, 60000000));
    Route_McpAddLockIdProp(props);
    static const char* req[] = {"pins"};
    cJSON_AddItemToArray(tools, Route_McpToolDef("trace_pins", "Capture edge events on one or more pins.",
                                                 Route_McpObjectSchema(props, req, 1)));
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

    cJSON* railsSchema = cJSON_CreateObject();
    cJSON_AddStringToObject(railsSchema, "type", "array");
    cJSON_AddNumberToObject(railsSchema, "minItems", 1);
    cJSON_AddItemToObject(railsSchema, "items", Route_McpEnumStringSchema(rails, TOOL_GET_ARRAY_LENGTH(rails)));

    cJSON* itemSchema = cJSON_CreateObject();
    cJSON_AddStringToObject(itemSchema, "type", "object");
    cJSON_AddStringToObject(itemSchema, "description",
                            "Typed lock group: type pin requires pins; type power requires rails.");
    cJSON* itemProps = cJSON_CreateObject();
    Route_McpAddProp(itemProps, "type", Route_McpEnumStringSchema(lockTypes, TOOL_GET_ARRAY_LENGTH(lockTypes)));
    Route_McpAddProp(itemProps, "pins", Route_McpPinsArraySchema());
    Route_McpAddProp(itemProps, "rails", railsSchema);
    Route_McpAddProp(itemProps, "method", methodSchema);
    cJSON_AddItemToObject(itemSchema, "properties", itemProps);
    cJSON* itemReq = cJSON_CreateArray();
    cJSON_AddItemToArray(itemReq, cJSON_CreateString("type"));
    cJSON_AddItemToArray(itemReq, cJSON_CreateString("method"));
    cJSON_AddItemToObject(itemSchema, "required", itemReq);

    cJSON* resources = cJSON_CreateObject();
    cJSON_AddStringToObject(resources, "type", "array");
    cJSON_AddNumberToObject(resources, "minItems", 1);
    cJSON_AddItemToObject(resources, "items", itemSchema);

    cJSON* props = cJSON_CreateObject();
    Route_McpAddProp(props, "resources", resources);
    static const char* req[] = {"resources"};
    cJSON_AddItemToArray(tools, Route_McpToolDef("create_lock",
                                                 "Create a lock. resources use type pin+pins or type power+rails.",
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
          "PWM resources exhausted. Free another pwmOutput pin or reuse an existing frequency.");
    }
    if (cfg == ESP_ERR_NOT_SUPPORTED) {
      return Route_McpToolResultErr("PWM frequency cannot be generated by this hardware. Try a higher frequency.");
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
  cJSON* levels = cJSON_CreateArray();
  for (size_t i = 0; i < sel.count; i++) {
    int level = 0;
    if (GpioCtrl_GetLevel(sel.pins[i], &level) != ESP_OK) {
      cJSON_Delete(root);
      return Route_McpToolResultErr("internal");
    }
    cJSON_AddItemToArray(levels, cJSON_CreateNumber(level));
  }
  cJSON_AddItemToObject(root, "levels", levels);
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
  Mcp_PinSelection sel;
  if (!Route_McpParsePinSelection(args, &sel, reason, sizeof(reason))) {
    return Route_McpToolResultErr(reason);
  }
  const char* lockId = Route_McpGetLockId(args);
  if (Route_McpCheckPinsLock(&sel, LOCK_METHOD_READ, lockId, reason, sizeof(reason))) {
    return Route_McpToolResultErr(reason);
  }
  cJSON* root = cJSON_CreateObject();
  cJSON* pwms = cJSON_CreateArray();
  for (size_t i = 0; i < sel.count; i++) {
    int pin = sel.pins[i];
    if (!GpioCtrl_IsPwmMode(pin)) {
      cJSON_Delete(root);
      snprintf(reason, sizeof(reason), "Pin %d is not in pwmOutput mode. PUT /pin/%d with mode pwmOutput first.", pin,
               pin);
      return Route_McpToolResultErr(reason);
    }
    double frequency = 0;
    double duty = 0;
    if (GpioCtrl_GetPwm(pin, &frequency, &duty) != ESP_OK) {
      cJSON_Delete(root);
      return Route_McpToolResultErr("internal");
    }
    cJSON* item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "pin", pin);
    cJSON_AddNumberToObject(item, "frequency", frequency);
    cJSON_AddNumberToObject(item, "duty", duty);
    cJSON_AddItemToArray(pwms, item);
  }
  cJSON_AddItemToObject(root, "pwms", pwms);
  cJSON_AddNumberToObject(root, "time", (double)HttpServer_NowUs());
  Lock_Touch(lockId);
  return Route_McpToolResultOk(root);
}

static cJSON* Route_McpCallSetPinPwm(cJSON* args)
{
  char reason[256];
  Mcp_PinSelection sel;
  if (!Route_McpParsePinSelection(args, &sel, reason, sizeof(reason))) {
    return Route_McpToolResultErr(reason);
  }
  cJSON* freqItem = cJSON_GetObjectItem(args, "frequency");
  cJSON* dutyItem = cJSON_GetObjectItem(args, "duty");
  if (!cJSON_IsNumber(freqItem) || freqItem->valuedouble < 1 || freqItem->valuedouble > CONFIG_GPIO_PWM_MAX_FREQ_HZ) {
    return Route_McpToolResultErr("frequency must be a number from 1 to 50000.");
  }
  if (!cJSON_IsNumber(dutyItem) || dutyItem->valuedouble < 0 || dutyItem->valuedouble > 100) {
    return Route_McpToolResultErr("duty must be a number from 0 to 100.");
  }
  const char* lockId = Route_McpGetLockId(args);
  if (Route_McpCheckPinsLock(&sel, LOCK_METHOD_WRITE, lockId, reason, sizeof(reason))) {
    return Route_McpToolResultErr(reason);
  }
  for (size_t i = 0; i < sel.count; i++) {
    int pin = sel.pins[i];
    if (!GpioCtrl_IsPwmMode(pin)) {
      snprintf(reason, sizeof(reason), "Pin %d is not in pwmOutput mode. PUT /pin/%d with mode pwmOutput first.", pin,
               pin);
      return Route_McpToolResultErr(reason);
    }
  }
  for (size_t i = 0; i < sel.count; i++) {
    int pin = sel.pins[i];
    esp_err_t pwm = GpioCtrl_SetPwm(pin, freqItem->valuedouble, dutyItem->valuedouble);
    if (pwm == ESP_ERR_NO_MEM) {
      return Route_McpToolResultErr(
          "PWM resources exhausted. Free another pwmOutput pin or reuse an existing frequency.");
    }
    if (pwm == ESP_ERR_NOT_SUPPORTED) {
      return Route_McpToolResultErr("PWM frequency cannot be generated by this hardware. Try a higher frequency.");
    }
    if (pwm != ESP_OK) return Route_McpToolResultErr("internal");
  }
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
  if (GpioCtrl_Trace(sel.pins, sel.count, edge, duration, events, CONFIG_GPIO_TRACE_MAX_EVENTS, &eventCount, true) !=
      ESP_OK) {
    free(events);
    return Route_McpToolResultErr("internal");
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
  Lock_Resource res[LOCK_MAX_RESOURCES];
  size_t count = 0;
  char reason[192];
  if (HttpServer_ParseLockResources(resources, res, LOCK_MAX_RESOURCES, &count, reason, sizeof(reason)) != ESP_OK) {
    return Route_McpToolResultErr(reason);
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
  cJSON_AddItemToObject(root, "resources", HttpServer_SerializeLockResources(created.resources, created.resourceCount));
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
      "Use plural pin tools (configure_pins, get_pin_levels, …) and always pass pins even for one pin (pins:[1]). "
      "Power tools use rail=3v3|5v. create_lock resources use type pin with pins, or type power with rails.");
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
  else if (strcmp(name, "configure_pins") == 0)
    out = Route_McpCallConfigurePin(args);
  else if (strcmp(name, "get_pin_levels") == 0)
    out = Route_McpCallGetPinLevel(args);
  else if (strcmp(name, "set_pin_levels") == 0)
    out = Route_McpCallSetPinLevel(args);
  else if (strcmp(name, "pulse_pins") == 0)
    out = Route_McpCallPulsePin(args);
  else if (strcmp(name, "get_pin_pwms") == 0)
    out = Route_McpCallGetPinPwm(args);
  else if (strcmp(name, "set_pin_pwms") == 0)
    out = Route_McpCallSetPinPwm(args);
  else if (strcmp(name, "trace_pins") == 0)
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
