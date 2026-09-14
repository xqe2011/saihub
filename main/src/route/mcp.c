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
#include <esp_http_server.h>
#include <esp_log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* tag = "SAIHUB-Mcp";

extern const uint8_t mcp_json_start[] asm("_binary_mcp_json_start");
extern const uint8_t mcp_json_end[] asm("_binary_mcp_json_end");

#define MCP_PROTOCOL_VERSION "2025-06-18"

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

static bool Route_McpJsonHasRef(const cJSON* node)
{
  if (node == NULL) return false;
  if (cJSON_IsObject(node) && cJSON_GetObjectItemCaseSensitive((cJSON*)node, "$ref") != NULL) {
    return true;
  }
  if (cJSON_IsObject(node) || cJSON_IsArray(node)) {
    const cJSON* child = NULL;
    cJSON_ArrayForEach(child, node) {
      if (Route_McpJsonHasRef(child)) return true;
    }
  }
  return false;
}

static cJSON* Route_McpToolsListResult(void)
{
  size_t len = (size_t)(mcp_json_end - mcp_json_start);
  cJSON* result = cJSON_ParseWithLength((const char*)mcp_json_start, len);
  if (result == NULL) {
    ESP_LOGE(tag, "mcp.json parse failed");
    result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "tools", cJSON_CreateArray());
    return result;
  }

  /* MCP clients treat each tool inputSchema as its own JSON Schema document. */
  cJSON* defs = cJSON_DetachItemFromObjectCaseSensitive(result, "$defs");
  cJSON* tools = cJSON_GetObjectItemCaseSensitive(result, "tools");
  if (defs != NULL && cJSON_IsArray(tools)) {
    cJSON* tool = NULL;
    cJSON_ArrayForEach(tool, tools) {
      cJSON* schema = cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
      if (!cJSON_IsObject(schema) || !Route_McpJsonHasRef(schema)) continue;
      cJSON* copy = cJSON_Duplicate(defs, 1);
      if (copy != NULL) cJSON_AddItemToObject(schema, "$defs", copy);
    }
  }
  cJSON_Delete(defs);
  return result;
}

static cJSON* Route_McpCallRunScript(cJSON* args)
{
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

  Script_Result sr;
  Script_Status st = Script_Run(script, maxCalls, timeoutUs, lockId, &sr);
  if (st != SCRIPT_OK) {
    cJSON* err = Route_McpToolResultErr(sr.reason);
    Script_ResultFree(&sr);
    return err;
  }
  cJSON* payload = cJSON_CreateObject();
  if (sr.result != NULL) {
    cJSON_AddItemToObject(payload, "result", sr.result);
    sr.result = NULL;
  } else {
    cJSON_AddNullToObject(payload, "result");
  }
  cJSON_AddStringToObject(payload, "output", sr.output ? sr.output : "");
  cJSON_AddNumberToObject(payload, "calls", (double)sr.calls);
  cJSON_AddNumberToObject(payload, "elapsed", (double)sr.elapsedUs);
  Script_ResultFree(&sr);
  return Route_McpToolResultOk(payload);
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
      "Power tools use rail=3v3|5v. create_lock resources use type pin with pins, or type power with rails. "
      "For multi-step on-device work, use run_script with a Lua script that calls the same tools.");
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
  if (strcmp(name, "run_script") == 0) {
    out = Route_McpCallRunScript(args);
  } else {
    ToolCall_Result tr = ToolCall_Invoke(name, args);
    if (!tr.ok) {
      out = Route_McpToolResultErr(tr.reason);
    } else {
      out = Route_McpToolResultOk(tr.payload);
    }
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
