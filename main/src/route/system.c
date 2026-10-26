/**
 * @name System routes
 * @file system.c
 * @author xqe2011
 */
#include "route.h"

#include "http_server.h"
#include "system.h"
#include "tool.h"

#include <cJSON.h>
#include <string.h>

static const char* Route_SystemOtaStateName(System_OtaState state)
{
  switch (state) {
    case SYSTEM_OTA_UPGRADING: return "upgrading";
    case SYSTEM_OTA_FAILED: return "failed";
    default: return "idle";
  }
}

static cJSON* Route_SystemInfoJson(void)
{
  System_Info info;
  System_GetInfo(&info);
  cJSON* root = cJSON_CreateObject();
  if (!root) return NULL;
  cJSON_AddStringToObject(root, "digest", info.digest ? info.digest : "");
  cJSON_AddStringToObject(root, "version", info.version ? info.version : "");
  cJSON_AddStringToObject(root, "compiledDate", info.compiledDate);
  cJSON_AddStringToObject(root, "landingPageUrl", info.landingPageUrl ? info.landingPageUrl : "");
  return root;
}

static cJSON* Route_SystemOtaStatusJson(void)
{
  System_OtaStatus st;
  System_OtaGetStatus(&st);
  cJSON* root = cJSON_CreateObject();
  if (!root) return NULL;
  cJSON_AddStringToObject(root, "state", Route_SystemOtaStateName(st.state));
  if (st.progressPercent >= 0) cJSON_AddNumberToObject(root, "progressPercent", st.progressPercent);
  if (st.state == SYSTEM_OTA_FAILED && st.reason[0]) cJSON_AddStringToObject(root, "reason", st.reason);
  return root;
}

static esp_err_t Route_SystemGetHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  cJSON* root = Route_SystemInfoJson();
  if (!root) return HttpServer_SendError(ctx, 500, "internal");
  return HttpServer_SendJson(ctx, 200, root);
}

static esp_err_t Route_SystemOtaGetHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  cJSON* root = Route_SystemOtaStatusJson();
  if (!root) return HttpServer_SendError(ctx, 500, "internal");
  return HttpServer_SendJson(ctx, 200, root);
}

static esp_err_t Route_SystemOtaCheckHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  char reason[192];
  cJSON* root = System_OtaCheck(reason, sizeof(reason));
  if (!root) {
    int status = System_OtaIsUpgrading() ? 503 : 502;
    return HttpServer_SendError(ctx, status, reason[0] ? reason : "Could not check for updates.");
  }
  return HttpServer_SendJson(ctx, 200, root);
}

static esp_err_t Route_SystemOtaUpgradeHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  if (!HttpServer_HasJsonContentType(ctx)) {
    return HttpServer_SendError(ctx, 415, "Content-Type must be application/json.");
  }
  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(ctx, &perr);
  if (body == NULL) return HttpServer_SendError(ctx, 400, "invalid_json");

  cJSON* url = cJSON_GetObjectItem(body, "url");
  cJSON* sha = cJSON_GetObjectItem(body, "sha256");
  if (!cJSON_IsString(url) || !url->valuestring || !url->valuestring[0] || !cJSON_IsString(sha) ||
      !sha->valuestring) {
    cJSON_Delete(body);
    return HttpServer_SendError(ctx, 400, "url and sha256 are required.");
  }

  char reason[192];
  esp_err_t err = System_OtaUpgrade(url->valuestring, sha->valuestring, reason, sizeof(reason));
  cJSON_Delete(body);
  if (err == ESP_ERR_INVALID_ARG) return HttpServer_SendError(ctx, 400, reason);
  if (err == ESP_ERR_INVALID_STATE) return HttpServer_SendError(ctx, 503, reason);
  if (err != ESP_OK) return HttpServer_SendError(ctx, 500, reason[0] ? reason : "internal");

  cJSON* root = cJSON_CreateObject();
  if (!root) return HttpServer_SendError(ctx, 500, "internal");
  cJSON_AddBoolToObject(root, "started", true);
  return HttpServer_SendJson(ctx, 200, root);
}

static const HttpServer_Route uris[] = {
    {.uri = "/system", .method = HTTP_SERVER_GET, .handler = Route_SystemGetHandler},
    {.uri = "/system/ota", .method = HTTP_SERVER_GET, .handler = Route_SystemOtaGetHandler},
    {.uri = "/system/ota/check", .method = HTTP_SERVER_POST, .handler = Route_SystemOtaCheckHandler},
    {.uri = "/system/ota/upgrade", .method = HTTP_SERVER_POST, .handler = Route_SystemOtaUpgradeHandler},
};

esp_err_t Route_SystemRegister(void)
{
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(HttpServer_RegisterRoutes(uris, TOOL_GET_ARRAY_LENGTH(uris)),
                                  "register system uri failed");
  return ESP_OK;
}
