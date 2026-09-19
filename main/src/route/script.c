/**
 * @name Script routes
 * @file script.c
 * @author xqe2011
 */
#include "route.h"

#include "config.h"
#include "http_server.h"
#include "lock.h"
#include "script.h"
#include "tool.h"

#include <cJSON.h>
#include <stdio.h>
#include <string.h>

static const char* tag = "SAIHUB-Script";

static void Route_ScriptRespond(HttpServer_Context* ctx, Script_Status st, Script_Result* sr, void* userCtx)
{
  (void)userCtx;
  if (st != SCRIPT_OK) {
    int status = sr->httpStatus ? sr->httpStatus : 400;
    HttpServer_SendError(ctx, status, sr->reason);
    Script_ResultFree(sr);
    return;
  }

  cJSON* root = cJSON_CreateObject();
  if (sr->result != NULL) {
    cJSON_AddItemToObject(root, "result", sr->result);
    sr->result = NULL;
  } else {
    cJSON_AddNullToObject(root, "result");
  }
  cJSON_AddStringToObject(root, "output", sr->output ? sr->output : "");
  cJSON_AddNumberToObject(root, "calls", (double)sr->calls);
  cJSON_AddNumberToObject(root, "elapsed", (double)sr->elapsedUs);
  Script_ResultFree(sr);
  HttpServer_SendJson(ctx, 200, root);
}

static esp_err_t Route_ScriptPostHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  Lock_SweepExpired();

  if (!HttpServer_HasJsonContentType(ctx)) {
    return HttpServer_SendError(ctx, 415, "Content-Type must be application/json.");
  }

  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(ctx, &perr);
  if (body == NULL) return HttpServer_SendError(ctx, 400, "invalid_json");

  cJSON* scriptItem = cJSON_GetObjectItem(body, "script");
  if (!cJSON_IsString(scriptItem) || scriptItem->valuestring == NULL || scriptItem->valuestring[0] == '\0') {
    cJSON_Delete(body);
    return HttpServer_SendError(ctx, 400, "script must be a non-empty string.");
  }

  uint32_t maxCalls = 0;
  uint64_t timeoutUs = 0;
  cJSON* maxCallsItem = cJSON_GetObjectItem(body, "maxCalls");
  if (maxCallsItem != NULL) {
    if (!cJSON_IsNumber(maxCallsItem) || maxCallsItem->valuedouble < 1 ||
        maxCallsItem->valuedouble > (double)CONFIG_SCRIPT_MAX_CALLS) {
      cJSON_Delete(body);
      char reason[128];
      snprintf(reason, sizeof(reason), "maxCalls must be an integer from 1 to %u.",
               (unsigned)CONFIG_SCRIPT_MAX_CALLS);
      return HttpServer_SendError(ctx, 400, reason);
    }
    maxCalls = (uint32_t)maxCallsItem->valuedouble;
  }
  cJSON* timeoutItem = cJSON_GetObjectItem(body, "timeout");
  if (timeoutItem != NULL) {
    if (!cJSON_IsNumber(timeoutItem) || timeoutItem->valuedouble < 1 ||
        timeoutItem->valuedouble > (double)CONFIG_SCRIPT_MAX_TIMEOUT_US) {
      cJSON_Delete(body);
      char reason[160];
      snprintf(reason, sizeof(reason), "timeout must be an integer from 1 to %llu microseconds.",
               (unsigned long long)CONFIG_SCRIPT_MAX_TIMEOUT_US);
      return HttpServer_SendError(ctx, 400, reason);
    }
    timeoutUs = (uint64_t)timeoutItem->valuedouble;
  }

  char lockId[64] = {0};
  HttpServer_GetLockHeader(ctx, lockId, sizeof(lockId));

  /* Script_RunAsync copies the source; body can be freed immediately after. */
  const char* script = scriptItem->valuestring;
  esp_err_t ret =
      Script_RunAsync(ctx, script, maxCalls, timeoutUs, lockId[0] ? lockId : NULL, Route_ScriptRespond, NULL);
  cJSON_Delete(body);
  return ret;
}

static const HttpServer_Route uris[] = {
    {.uri = "/script", .method = HTTP_SERVER_POST, .handler = Route_ScriptPostHandler},
};

esp_err_t Route_ScriptRegister(void)
{
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(HttpServer_RegisterRoutes(uris, TOOL_GET_ARRAY_LENGTH(uris)), "register script uri failed");
  (void)tag;
  return ESP_OK;
}
