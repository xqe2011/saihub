/**
 * @name Script routes
 * @file script.c
 * @author xqe2011
 */
#include "route.h"

#include "api.pb.h"
#include "config.h"
#include "http_server.h"
#include "lock.h"
#include "script.h"
#include "tool.h"

#include <cJSON.h>
#include <esp_http_server.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* tag = "SAIHUB-Script";

static void Route_ScriptRespond(httpd_req_t* req, Script_Status st, Script_Result* sr, void* userCtx)
{
  (void)userCtx;
  if (st != SCRIPT_OK) {
    int status = sr->httpStatus ? sr->httpStatus : 400;
    HttpServer_SendError(req, status, sr->reason);
    Script_ResultFree(sr);
    return;
  }

  if (HttpServer_HasProtobufContentType(req)) {
    saihub_api_ScriptResult* pb = calloc(1, sizeof(*pb));
    if (pb == NULL) {
      Script_ResultFree(sr);
      HttpServer_SendError(req, 500, "internal");
      return;
    }
    if (sr->result != NULL) {
      char* printed = cJSON_PrintUnformatted(sr->result);
      if (printed != NULL) {
        pb->has_result = true;
        snprintf(pb->result, sizeof(pb->result), "%s", printed);
        free(printed);
      }
    }
    snprintf(pb->output, sizeof(pb->output), "%s", sr->output ? sr->output : "");
    pb->calls = (int32_t)sr->calls;
    pb->elapsed = (int64_t)sr->elapsedUs;
    Script_ResultFree(sr);
    HttpServer_SendPb(req, 200, saihub_api_ScriptResult_fields, pb);
    free(pb);
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
  HttpServer_SendJson(req, 200, root);
}

static esp_err_t Route_ScriptPostHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Lock_SweepExpired();

  if (!HttpServer_HasApiContentType(req)) {
    return HttpServer_SendError(req, 415, HttpServer_UnsupportedMediaTypeReason());
  }

  char lockId[64] = {0};
  HttpServer_GetLockHeader(req, lockId, sizeof(lockId));

  if (HttpServer_HasProtobufContentType(req)) {
    saihub_api_ScriptBody* body = calloc(1, sizeof(*body));
    if (body == NULL) return HttpServer_SendError(req, 500, "internal");

    esp_err_t perr = HttpServer_DecodePb(req, saihub_api_ScriptBody_fields, body);
    if (perr == ESP_ERR_INVALID_ARG || perr == ESP_ERR_INVALID_SIZE) {
      free(body);
      return HttpServer_SendError(req, 400, "invalid_protobuf");
    }
    if (perr == ESP_ERR_NO_MEM) {
      free(body);
      return HttpServer_SendError(req, 500, "internal");
    }
    if (perr != ESP_OK) {
      free(body);
      return HttpServer_SendError(req, 400, "invalid_protobuf");
    }

    if (body->script[0] == '\0') {
      free(body);
      return HttpServer_SendError(req, 400, "script must be a non-empty string.");
    }

    uint32_t maxCalls = 0;
    uint64_t timeoutUs = 0;
    if (body->has_maxCalls) {
      if (body->maxCalls < 1 || body->maxCalls > (int32_t)CONFIG_SCRIPT_MAX_CALLS) {
        free(body);
        char reason[128];
        snprintf(reason, sizeof(reason), "maxCalls must be an integer from 1 to %u.",
                 (unsigned)CONFIG_SCRIPT_MAX_CALLS);
        return HttpServer_SendError(req, 400, reason);
      }
      maxCalls = (uint32_t)body->maxCalls;
    }
    if (body->has_timeout) {
      if (body->timeout < 1 || (uint64_t)body->timeout > CONFIG_SCRIPT_MAX_TIMEOUT_US) {
        free(body);
        char reason[160];
        snprintf(reason, sizeof(reason), "timeout must be an integer from 1 to %llu microseconds.",
                 (unsigned long long)CONFIG_SCRIPT_MAX_TIMEOUT_US);
        return HttpServer_SendError(req, 400, reason);
      }
      timeoutUs = (uint64_t)body->timeout;
    }

    /* Script_RunAsync copies the source; body can be freed immediately after. */
    esp_err_t ret =
        Script_RunAsync(req, body->script, maxCalls, timeoutUs, lockId[0] ? lockId : NULL, Route_ScriptRespond, NULL);
    free(body);
    return ret;
  }

  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(req, &perr);
  if (body == NULL) return HttpServer_SendError(req, 400, "invalid_json");

  cJSON* scriptItem = cJSON_GetObjectItem(body, "script");
  if (!cJSON_IsString(scriptItem) || scriptItem->valuestring == NULL || scriptItem->valuestring[0] == '\0') {
    cJSON_Delete(body);
    return HttpServer_SendError(req, 400, "script must be a non-empty string.");
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
      return HttpServer_SendError(req, 400, reason);
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
      return HttpServer_SendError(req, 400, reason);
    }
    timeoutUs = (uint64_t)timeoutItem->valuedouble;
  }

  /* Script_RunAsync copies the source; body can be freed immediately after. */
  const char* script = scriptItem->valuestring;
  esp_err_t ret =
      Script_RunAsync(req, script, maxCalls, timeoutUs, lockId[0] ? lockId : NULL, Route_ScriptRespond, NULL);
  cJSON_Delete(body);
  return ret;
}

static const httpd_uri_t uris[] = {
    {.uri = "/script", .method = HTTP_POST, .handler = Route_ScriptPostHandler},
};

esp_err_t Route_ScriptRegister(httpd_handle_t server)
{
  for (size_t i = 0; i < TOOL_GET_ARRAY_LENGTH(uris); i++) {
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(httpd_register_uri_handler(server, &uris[i]), "register script uri failed");
  }
  (void)tag;
  return ESP_OK;
}
