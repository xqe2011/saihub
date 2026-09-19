/**
 * @name Lock routes
 * @file lock.c
 * @author xqe2011
 */
#include "route.h"

#include "config.h"
#include "http_server.h"
#include "lock.h"
#include "tool.h"

#include <cJSON.h>
#include <stdio.h>
#include <string.h>

static const char* tag = "SAIHUB-Lock";

static esp_err_t Route_LockCreateHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  Lock_SweepExpired();
  if (!HttpServer_HasJsonContentType(ctx)) {
    return HttpServer_SendError(ctx, 415, "Content-Type must be application/json.");
  }
  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(ctx, &perr);
  if (body == NULL) return HttpServer_SendError(ctx, 400, "invalid_json");

  cJSON* resources = cJSON_GetObjectItem(body, "resources");
  Lock_Resource res[CONFIG_LOCK_MAX_RESOURCES];
  size_t count = 0;
  char reason[192];
  if (HttpServer_ParseLockResources(resources, res, CONFIG_LOCK_MAX_RESOURCES, &count, reason, sizeof(reason)) != ESP_OK) {
    cJSON_Delete(body);
    return HttpServer_SendError(ctx, 400, reason);
  }
  cJSON_Delete(body);

  Lock_Entry created;
  Lock_Conflict conflict;
  memset(&conflict, 0, sizeof(conflict));
  esp_err_t cret = Lock_Create(res, count, &created, &conflict);
  if (cret == ESP_ERR_INVALID_STATE) {
    HttpServer_FormatLockConflict(&conflict, reason, sizeof(reason));
    return HttpServer_SendError(ctx, 409, reason);
  }
  if (cret != ESP_OK) {
    return HttpServer_SendError(ctx, 500, "internal");
  }

  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "id", created.id);
  int64_t now = Lock_NowUs();
  cJSON_AddNumberToObject(root, "ttl", (double)(created.expiresAtUs - now));
  cJSON_AddNumberToObject(root, "expiresAt", (double)created.expiresAtUs);
  cJSON_AddItemToObject(root, "resources", HttpServer_SerializeLockResources(created.resources, created.resourceCount));
  return HttpServer_SendJson(ctx, 201, root);
}

static esp_err_t Route_LockRenewHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  Lock_SweepExpired();
  const char* prefix = "/lock/";
  if (strncmp(HttpServer_GetUri(ctx), prefix, strlen(prefix)) != 0) {
    return HttpServer_SendError(ctx, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  const char* id = HttpServer_GetUri(ctx) + strlen(prefix);
  if (id[0] == '\0') {
    return HttpServer_SendError(ctx, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  TOOL_CALL_LOG("rest/%s %s %s id=%s", HttpServer_GetFrom(ctx), HttpServer_MethodName(HttpServer_GetMethod(ctx)), HttpServer_GetUri(ctx), id);
  Lock_Entry entry;
  if (Lock_Renew(id, &entry) != ESP_OK) {
    char reason[160];
    snprintf(reason, sizeof(reason),
             "Lock id %s does not exist. Create a lock with POST /lock (see GET /openapi.json).", id);
    return HttpServer_SendError(ctx, 404, reason);
  }
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "id", entry.id);
  int64_t now = Lock_NowUs();
  cJSON_AddNumberToObject(root, "ttl", (double)(entry.expiresAtUs - now));
  cJSON_AddNumberToObject(root, "expiresAt", (double)entry.expiresAtUs);
  return HttpServer_SendJson(ctx, 200, root);
}

static esp_err_t Route_LockDeleteHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  Lock_SweepExpired();
  const char* prefix = "/lock/";
  if (strncmp(HttpServer_GetUri(ctx), prefix, strlen(prefix)) != 0) {
    return HttpServer_SendError(ctx, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  const char* id = HttpServer_GetUri(ctx) + strlen(prefix);
  TOOL_CALL_LOG("rest/%s %s %s id=%s", HttpServer_GetFrom(ctx), HttpServer_MethodName(HttpServer_GetMethod(ctx)), HttpServer_GetUri(ctx), id);
  Lock_Delete(id); /* always 204 */
  return HttpServer_SendEmpty(ctx, 204);
}

static const HttpServer_Route uris[] = {
    {.uri = "/lock", .method = HTTP_SERVER_POST, .handler = Route_LockCreateHandler},
    {.uri = "/lock/*", .method = HTTP_SERVER_PUT, .handler = Route_LockRenewHandler},
    {.uri = "/lock/*", .method = HTTP_SERVER_DELETE, .handler = Route_LockDeleteHandler},
};

esp_err_t Route_LockRegister(void)
{
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(HttpServer_RegisterRoutes(uris, TOOL_GET_ARRAY_LENGTH(uris)), "register lock uri failed");
  return ESP_OK;
}
