/**
 * @name MCP Streamable HTTP routes
 * @file mcp.c
 * @author xqe2011
 */
#include "route.h"

#include "config.h"
#include "http_server.h"
#include "lock.h"
#include "script.h"
#include "tool.h"
#include "tool_call.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* tag = "SAIHub-Mcp";

extern const uint8_t mcp_json_start[] asm("_binary_mcp_json_start");
extern const uint8_t mcp_json_end[] asm("_binary_mcp_json_end");

#define MCP_PROTOCOL_VERSION "2025-06-18"
#define MCP_DEFS_REF_PREFIX "#/$defs/"

static bool Route_McpOriginOk(HttpServer_Context* ctx)
{
  char origin[256] = {0};
  if (HttpServer_GetHeader(ctx, "Origin", origin, sizeof(origin)) != ESP_OK) {
    return true;
  }
  if (origin[0] == '\0') return true;
  return strncmp(origin, "http:", 5) == 0 || strncmp(origin, "https:", 6) == 0;
}

static bool Route_McpAcceptOk(HttpServer_Context* ctx)
{
  char accept[256] = {0};
  if (HttpServer_GetHeader(ctx, "Accept", accept, sizeof(accept)) != ESP_OK) {
    return false;
  }
  return strstr(accept, "application/json") != NULL || strstr(accept, "text/event-stream") != NULL;
}

static bool Route_McpProtocolVersionOk(HttpServer_Context* ctx, bool isInitialize)
{
  char ver[64] = {0};
  if (HttpServer_GetHeader(ctx, "MCP-Protocol-Version", ver, sizeof(ver)) != ESP_OK || ver[0] == '\0') {
    return true;
  }
  if (strcmp(ver, "2025-06-18") == 0 || strcmp(ver, "2025-03-26") == 0 || strcmp(ver, "2024-11-05") == 0) {
    return true;
  }
  (void)isInitialize;
  return false;
}

static esp_err_t Route_McpSendStatus(HttpServer_Context* ctx, int status, const char* body,
                                     const char* contentType)
{
  return HttpServer_Send(ctx, status, contentType, NULL, body, body ? strlen(body) : 0);
}

static esp_err_t Route_McpSendJsonRpc(HttpServer_Context* ctx, int httpStatus, cJSON* root)
{
  char* printed = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (printed == NULL) {
    return Route_McpSendStatus(ctx, 500, "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"internal\"},\"id\":null}",
                               "application/json");
  }
  esp_err_t ret = HttpServer_Send(ctx, httpStatus, "application/json", NULL, printed, strlen(printed));
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

static bool Route_McpCollectDefs(const cJSON* node, const cJSON* allDefs, cJSON* selectedDefs)
{
  if (node == NULL) return true;

  if (cJSON_IsObject(node)) {
    const cJSON* ref = cJSON_GetObjectItemCaseSensitive((cJSON*)node, "$ref");
    if (cJSON_IsString(ref) && ref->valuestring != NULL &&
        strncmp(ref->valuestring, MCP_DEFS_REF_PREFIX, strlen(MCP_DEFS_REF_PREFIX)) == 0) {
      const char* name = ref->valuestring + strlen(MCP_DEFS_REF_PREFIX);
      if (name[0] == '\0' || strchr(name, '/') != NULL) return false;
      if (cJSON_GetObjectItemCaseSensitive(selectedDefs, name) == NULL) {
        const cJSON* definition = cJSON_GetObjectItemCaseSensitive((cJSON*)allDefs, name);
        if (definition == NULL) return false;
        cJSON* copy = cJSON_Duplicate(definition, 1);
        if (copy == NULL || !cJSON_AddItemToObject(selectedDefs, name, copy)) {
          cJSON_Delete(copy);
          return false;
        }
        if (!Route_McpCollectDefs(definition, allDefs, selectedDefs)) return false;
      }
    }
  }

  if (cJSON_IsObject(node) || cJSON_IsArray(node)) {
    const cJSON* child = NULL;
    cJSON_ArrayForEach(child, node) {
      if (!Route_McpCollectDefs(child, allDefs, selectedDefs)) return false;
    }
  }
  return true;
}

static esp_err_t Route_McpSendToolsList(HttpServer_Context* ctx, cJSON* id)
{
  size_t len = (size_t)(mcp_json_end - mcp_json_start);
  cJSON* inventory = cJSON_ParseWithLength((const char*)mcp_json_start, len);
  if (inventory == NULL) {
    ESP_LOGE(tag, "mcp.json parse failed");
    return Route_McpSendJsonRpc(ctx, 500, Route_McpJsonRpcError(id, -32603, "internal"));
  }

  cJSON* defs = cJSON_DetachItemFromObjectCaseSensitive(inventory, "$defs");
  cJSON* tools = cJSON_GetObjectItemCaseSensitive(inventory, "tools");
  if (!cJSON_IsArray(tools)) {
    ESP_LOGE(tag, "mcp.json tools missing");
    cJSON_Delete(defs);
    cJSON_Delete(inventory);
    return Route_McpSendJsonRpc(ctx, 500, Route_McpJsonRpcError(id, -32603, "internal"));
  }

  esp_err_t ret = HttpServer_SendChunkBegin(ctx, 200, "application/json", NULL);
  if (ret == ESP_OK) ret = HttpServer_SendChunk(ctx, "{\"jsonrpc\":\"2.0\",\"result\":{\"tools\":[", sizeof("{\"jsonrpc\":\"2.0\",\"result\":{\"tools\":[") - 1);
  bool first = true;
  cJSON* tool = NULL;
  cJSON_ArrayForEach(tool, tools) {
    cJSON* schema = cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
    cJSON* selectedDefs = NULL;
    if (ret == ESP_OK && defs != NULL && cJSON_IsObject(schema)) {
      selectedDefs = cJSON_CreateObject();
      if (selectedDefs == NULL || !Route_McpCollectDefs(schema, defs, selectedDefs)) {
        ESP_LOGE(tag, "tool schema definition expansion failed");
        ret = ESP_ERR_NO_MEM;
      } else if (selectedDefs->child == NULL) {
        cJSON_Delete(selectedDefs);
        selectedDefs = NULL;
      } else if (!cJSON_AddItemToObject(schema, "$defs", selectedDefs)) {
        cJSON_Delete(selectedDefs);
        selectedDefs = NULL;
        ret = ESP_ERR_NO_MEM;
      }
    }

    char* printed = ret == ESP_OK ? cJSON_PrintUnformatted(tool) : NULL;
    if (ret == ESP_OK && printed == NULL) ret = ESP_ERR_NO_MEM;
    if (ret == ESP_OK && !first) ret = HttpServer_SendChunk(ctx, ",", 1);
    if (ret == ESP_OK) ret = HttpServer_SendChunk(ctx, printed, strlen(printed));
    free(printed);

    if (selectedDefs != NULL) {
      cJSON* detached = cJSON_DetachItemFromObjectCaseSensitive(schema, "$defs");
      cJSON_Delete(detached);
    }
    if (ret != ESP_OK) break;
    first = false;
  }

  char* printedId = ret == ESP_OK && id != NULL ? cJSON_PrintUnformatted(id) : NULL;
  if (ret == ESP_OK && id != NULL && printedId == NULL) ret = ESP_ERR_NO_MEM;
  if (ret == ESP_OK) ret = HttpServer_SendChunk(ctx, "]},\"id\":", sizeof("]},\"id\":") - 1);
  if (ret == ESP_OK) {
    const char* idText = printedId != NULL ? printedId : "null";
    ret = HttpServer_SendChunk(ctx, idText, strlen(idText));
  }
  if (ret == ESP_OK) ret = HttpServer_SendChunk(ctx, "}", 1);
  free(printedId);
  cJSON_Delete(defs);
  cJSON_Delete(inventory);

  esp_err_t endRet = HttpServer_SendChunkDone(ctx);
  return ret == ESP_OK ? endRet : ret;
}

static void Route_McpScriptRespond(HttpServer_Context* ctx, Script_Status st, Script_Result* sr, void* userCtx)
{
  cJSON* id = (cJSON*)userCtx;
  cJSON* toolResult = NULL;
  if (st != SCRIPT_OK) {
    toolResult = Route_McpToolResultErr(sr->reason);
    Script_ResultFree(sr);
  } else {
    cJSON* payload = cJSON_CreateObject();
    if (sr->result != NULL) {
      cJSON_AddItemToObject(payload, "result", sr->result);
      sr->result = NULL;
    } else {
      cJSON_AddNullToObject(payload, "result");
    }
    cJSON_AddStringToObject(payload, "output", sr->output ? sr->output : "");
    cJSON_AddNumberToObject(payload, "calls", (double)sr->calls);
    cJSON_AddNumberToObject(payload, "elapsed", (double)sr->elapsedUs);
    Script_ResultFree(sr);
    toolResult = Route_McpToolResultOk(payload);
  }
  Route_McpSendJsonRpc(ctx, 200, Route_McpJsonRpcResult(id, toolResult));
  cJSON_Delete(id);
}

/**
 * Validate run_script args. On failure returns an error tool result (caller owns).
 * On success returns NULL and fills out params.
 */
static cJSON* Route_McpParseRunScriptArgs(cJSON* args, const char** scriptOut, uint32_t* maxCallsOut,
                                          uint64_t* timeoutUsOut, const char** lockIdOut)
{
  *scriptOut = NULL;
  *maxCallsOut = 0;
  *timeoutUsOut = 0;
  *lockIdOut = NULL;

  if (args == NULL) return Route_McpToolResultErr("arguments are required.");
  cJSON* scriptItem = cJSON_GetObjectItem(args, "script");
  if (!cJSON_IsString(scriptItem) || scriptItem->valuestring == NULL) {
    return Route_McpToolResultErr("script must be a non-empty string.");
  }
  const char* script = scriptItem->valuestring;
  if (script[0] == '\0') {
    return Route_McpToolResultErr("script must be a non-empty string.");
  }

  uint32_t maxCalls = 0;
  uint64_t timeoutUs = 0;
  cJSON* maxCallsItem = cJSON_GetObjectItem(args, "maxCalls");
  if (maxCallsItem != NULL) {
    if (!cJSON_IsNumber(maxCallsItem) || maxCallsItem->valuedouble < 1 ||
        maxCallsItem->valuedouble > (double)CONFIG_SCRIPT_MAX_CALLS) {
      char reason[128];
      snprintf(reason, sizeof(reason), "maxCalls must be an integer from 1 to %u.",
               (unsigned)CONFIG_SCRIPT_MAX_CALLS);
      return Route_McpToolResultErr(reason);
    }
    maxCalls = (uint32_t)maxCallsItem->valuedouble;
  }
  cJSON* timeoutItem = cJSON_GetObjectItem(args, "timeout");
  if (timeoutItem != NULL) {
    if (!cJSON_IsNumber(timeoutItem) || timeoutItem->valuedouble < 1 ||
        timeoutItem->valuedouble > (double)CONFIG_SCRIPT_MAX_TIMEOUT_US) {
      char reason[160];
      snprintf(reason, sizeof(reason), "timeout must be an integer from 1 to %llu microseconds.",
               (unsigned long long)CONFIG_SCRIPT_MAX_TIMEOUT_US);
      return Route_McpToolResultErr(reason);
    }
    timeoutUs = (uint64_t)timeoutItem->valuedouble;
  }

  const char* lockId = NULL;
  cJSON* lockIdItem = cJSON_GetObjectItem(args, "lockId");
  if (cJSON_IsString(lockIdItem) && lockIdItem->valuestring != NULL && lockIdItem->valuestring[0] != '\0') {
    lockId = lockIdItem->valuestring;
  }

  *scriptOut = script;
  *maxCallsOut = maxCalls;
  *timeoutUsOut = timeoutUs;
  *lockIdOut = lockId;
  return NULL;
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
  cJSON_AddStringToObject(info, "version", esp_app_get_description()->version);
  cJSON_AddItemToObject(result, "serverInfo", info);
  cJSON_AddStringToObject(
      result, "instructions",
      "SAIHub: logical pins 0-7, UART ids from list_uarts, times in microseconds, optional lockId for contested "
      "resources. Use plural pin tools (configure_pins, get_pin_levels, …) and always pass pins even for one pin "
      "(pins:[1]). Power tools use rail=3v3|5v. UART tools use configure_uart, uart_transmit, uart_receive, uart_flush. "
      "create_lock resources use type pin with pins, type power with rails, or type uart with ids. "
      "For multi-step on-device work, use run_script with a Lua script that calls the same tools.");
  return result;
}

static cJSON* Route_McpHandleToolsCall(HttpServer_Context* ctx, cJSON* params)
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

  char from[48];
  snprintf(from, sizeof(from), "mcp/%s", HttpServer_GetFrom(ctx));
  ToolCall_Result tr = ToolCall_Invoke(from, name, args);
  cJSON* out = !tr.ok ? Route_McpToolResultErr(tr.reason) : Route_McpToolResultOk(tr.payload);
  if (ownedArgs) cJSON_Delete(args);
  return out;
}

static esp_err_t Route_McpDispatchTrace(HttpServer_Context* ctx, cJSON* id, cJSON* params)
{
  if (!cJSON_IsObject(params)) {
    return Route_McpSendJsonRpc(ctx, 200, Route_McpJsonRpcResult(id, Route_McpToolResultErr("params are required.")));
  }
  cJSON* args = cJSON_GetObjectItem(params, "arguments");
  if (args != NULL && !cJSON_IsObject(args)) {
    return Route_McpSendJsonRpc(ctx, 200,
                                Route_McpJsonRpcResult(id, Route_McpToolResultErr("arguments must be an object.")));
  }
  if (args == NULL) args = cJSON_CreateObject();
  bool ownedArgs = cJSON_GetObjectItem(params, "arguments") == NULL;
  GpioCtrl_TraceEvent* events = NULL;
  size_t eventCount = 0;
  ToolCall_Result result = ToolCall_TraceCapture(args, &events, &eventCount);
  if (ownedArgs) cJSON_Delete(args);
  if (!result.ok) {
    return Route_McpSendJsonRpc(ctx, 200, Route_McpJsonRpcResult(id, Route_McpToolResultErr(result.reason)));
  }

  esp_err_t ret = HttpServer_SendChunkBegin(ctx, 200, "application/json", NULL);
  if (ret == ESP_OK) ret = HttpServer_SendChunk(
      ctx, "{\"jsonrpc\":\"2.0\",\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"{\\\"events\\\":[", sizeof("{\"jsonrpc\":\"2.0\",\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"{\\\"events\\\":[") - 1);
  char item[160];
  for (size_t i = 0; ret == ESP_OK && i < eventCount; i++) {
    int len = snprintf(item, sizeof(item),
                       "%s{\\\"pin\\\":%d,\\\"edge\\\":\\\"%s\\\",\\\"level\\\":%d,\\\"time\\\":%lld}",
                       i == 0 ? "" : ",", events[i].pin, events[i].edge, events[i].level, (long long)events[i].time);
    if (len < 0 || (size_t)len >= sizeof(item)) {
      ret = ESP_ERR_INVALID_SIZE;
      break;
    }
    ret = HttpServer_SendChunk(ctx, item, (size_t)len);
  }
  if (ret == ESP_OK) ret = HttpServer_SendChunk(ctx, "]}\"}]},\"id\":", sizeof("]}\"}]},\"id\":") - 1);
  char* printedId = ret == ESP_OK && id != NULL ? cJSON_PrintUnformatted(id) : NULL;
  if (ret == ESP_OK && id != NULL && printedId == NULL) ret = ESP_ERR_NO_MEM;
  if (ret == ESP_OK) ret = HttpServer_SendChunk(ctx, printedId != NULL ? printedId : "null", printedId != NULL ? strlen(printedId) : 4);
  if (ret == ESP_OK) ret = HttpServer_SendChunk(ctx, "}", 1);
  free(printedId);
  free(events);
  esp_err_t endRet = HttpServer_SendChunkDone(ctx);
  return ret == ESP_OK ? endRet : ret;
}

static esp_err_t Route_McpDispatchRunScript(HttpServer_Context* ctx, cJSON* id, cJSON* params)
{
  if (!cJSON_IsObject(params)) {
    return Route_McpSendJsonRpc(ctx, 200, Route_McpJsonRpcResult(id, Route_McpToolResultErr("params are required.")));
  }
  cJSON* args = cJSON_GetObjectItem(params, "arguments");
  if (args != NULL && !cJSON_IsObject(args)) {
    return Route_McpSendJsonRpc(ctx, 200,
                                Route_McpJsonRpcResult(id, Route_McpToolResultErr("arguments must be an object.")));
  }

  const char* script = NULL;
  uint32_t maxCalls = 0;
  uint64_t timeoutUs = 0;
  const char* lockId = NULL;
  cJSON* parseErr = Route_McpParseRunScriptArgs(args, &script, &maxCalls, &timeoutUs, &lockId);
  if (parseErr != NULL) {
    return Route_McpSendJsonRpc(ctx, 200, Route_McpJsonRpcResult(id, parseErr));
  }

  TOOL_CALL_LOG("mcp/%s run_script(maxCalls=%u, timeout=%llu, lockId=%s, script_len=%u)", HttpServer_GetFrom(ctx), (unsigned)maxCalls,
                (unsigned long long)timeoutUs, lockId != NULL ? lockId : "", (unsigned)strlen(script));

  /* Heap-copy id: respond frees it after the JSON-RPC envelope is sent. */
  cJSON* idCopy = id != NULL ? cJSON_Duplicate(id, 1) : NULL;
  /* Script source is copied inside Script_RunAsync before this returns. */
  return Script_RunAsync(ctx, script, maxCalls, timeoutUs, lockId, Route_McpScriptRespond, idCopy);
}

static esp_err_t Route_McpDispatch(HttpServer_Context* ctx, cJSON* msg)
{
  cJSON* jsonrpc = cJSON_GetObjectItem(msg, "jsonrpc");
  if (!cJSON_IsString(jsonrpc) || strcmp(jsonrpc->valuestring, "2.0") != 0) {
    return Route_McpSendJsonRpc(ctx, 400, Route_McpJsonRpcError(NULL, -32600, "Invalid Request"));
  }

  cJSON* id = cJSON_GetObjectItem(msg, "id");
  cJSON* methodItem = cJSON_GetObjectItem(msg, "method");
  bool hasMethod = cJSON_IsString(methodItem) && methodItem->valuestring != NULL;
  bool hasId = (id != NULL && !cJSON_IsNull(id));

  /* Client JSON-RPC response (has id, no method) or notification (method, no id) */
  if (!hasMethod) {
    if (!hasId) {
      return Route_McpSendJsonRpc(ctx, 400, Route_McpJsonRpcError(NULL, -32600, "Invalid Request"));
    }
    return Route_McpSendStatus(ctx, 202, NULL, NULL);
  }

  const char* method = methodItem->valuestring;
  if (!hasId) {
    /* notification */
    if (strcmp(method, "notifications/initialized") == 0 || strcmp(method, "notifications/cancelled") == 0) {
      return Route_McpSendStatus(ctx, 202, NULL, NULL);
    }
    return Route_McpSendStatus(ctx, 202, NULL, NULL);
  }

  Lock_SweepExpired();
  cJSON* params = cJSON_GetObjectItem(msg, "params");

  if (strcmp(method, "initialize") == 0) {
    return Route_McpSendJsonRpc(ctx, 200, Route_McpJsonRpcResult(id, Route_McpHandleInitialize(params)));
  }
  if (strcmp(method, "ping") == 0) {
    return Route_McpSendJsonRpc(ctx, 200, Route_McpJsonRpcResult(id, cJSON_CreateObject()));
  }
  if (strcmp(method, "tools/list") == 0) {
    return Route_McpSendToolsList(ctx, id);
  }
  if (strcmp(method, "tools/call") == 0) {
    if (cJSON_IsObject(params)) {
      cJSON* nameItem = cJSON_GetObjectItem(params, "name");
      if (cJSON_IsString(nameItem) && nameItem->valuestring != NULL) {
        if (strcmp(nameItem->valuestring, "run_script") == 0) return Route_McpDispatchRunScript(ctx, id, params);
        if (strcmp(nameItem->valuestring, "trace_pins") == 0) return Route_McpDispatchTrace(ctx, id, params);
      }
    }
    return Route_McpSendJsonRpc(ctx, 200, Route_McpJsonRpcResult(id, Route_McpHandleToolsCall(ctx, params)));
  }

  return Route_McpSendJsonRpc(ctx, 200, Route_McpJsonRpcError(id, -32601, "Method not found"));
}

static esp_err_t Route_McpPostHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);

  if (!Route_McpOriginOk(ctx)) {
    return Route_McpSendStatus(ctx, 403, NULL, NULL);
  }
  if (!Route_McpAcceptOk(ctx)) {
    return Route_McpSendJsonRpc(ctx, 406,
                                Route_McpJsonRpcError(NULL, -32600, "Accept must include application/json or text/event-stream"));
  }
  if (!HttpServer_HasJsonContentType(ctx)) {
    return Route_McpSendJsonRpc(ctx, 415, Route_McpJsonRpcError(NULL, -32600, "Content-Type must be application/json"));
  }
  if (!Route_McpProtocolVersionOk(ctx, false)) {
    return Route_McpSendJsonRpc(ctx, 400, Route_McpJsonRpcError(NULL, -32600, "Unsupported MCP-Protocol-Version"));
  }

  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(ctx, &perr);
  if (body == NULL) {
    return Route_McpSendJsonRpc(ctx, 400, Route_McpJsonRpcError(NULL, -32700, "Parse error"));
  }
  if (cJSON_IsArray(body)) {
    cJSON_Delete(body);
    return Route_McpSendJsonRpc(ctx, 400, Route_McpJsonRpcError(NULL, -32600, "JSON-RPC batch not supported"));
  }
  if (!cJSON_IsObject(body)) {
    cJSON_Delete(body);
    return Route_McpSendJsonRpc(ctx, 400, Route_McpJsonRpcError(NULL, -32600, "Invalid Request"));
  }

  esp_err_t ret = Route_McpDispatch(ctx, body);
  cJSON_Delete(body);
  return ret;
}

static esp_err_t Route_McpGetHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  if (!Route_McpOriginOk(ctx)) {
    return Route_McpSendStatus(ctx, 403, NULL, NULL);
  }
  return Route_McpSendStatus(ctx, 405, NULL, NULL);
}

static esp_err_t Route_McpDeleteHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  if (!Route_McpOriginOk(ctx)) {
    return Route_McpSendStatus(ctx, 403, NULL, NULL);
  }
  return Route_McpSendStatus(ctx, 405, NULL, NULL);
}

static esp_err_t Route_McpOptionsHandler(HttpServer_Context* ctx)
{
  return HttpServer_SendOptions(ctx);
}

static const HttpServer_Route uris[] = {
    {.uri = "/mcp", .method = HTTP_SERVER_POST, .handler = Route_McpPostHandler},
    {.uri = "/mcp", .method = HTTP_SERVER_GET, .handler = Route_McpGetHandler},
    {.uri = "/mcp", .method = HTTP_SERVER_DELETE, .handler = Route_McpDeleteHandler},
    {.uri = "/mcp", .method = HTTP_SERVER_OPTIONS, .handler = Route_McpOptionsHandler},
};

esp_err_t Route_McpRegister(void)
{
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(HttpServer_RegisterRoutes(uris, TOOL_GET_ARRAY_LENGTH(uris)), "register mcp uri failed");
  return ESP_OK;
}
