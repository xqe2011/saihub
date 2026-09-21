/**
 * @name Cloud grant-secret routes
 * @file cloud.c
 * @author xqe2011
 */
#include "route.h"

#include "cloud.h"
#include "http_server.h"
#include "tool.h"

#include <string.h>

static const char* tag = "SAIHub-CloudGrant";

static esp_err_t Route_CloudGrantListHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  cJSON* root = Cloud_ListGrantSecrets();
  if (!root) return HttpServer_SendError(ctx, 500, "internal");
  return HttpServer_SendJson(ctx, 200, root);
}

static esp_err_t Route_CloudGrantDeleteHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  const char* prefix = "/cloud/grant-secrets/";
  const char* uri = HttpServer_GetUri(ctx);
  if (strncmp(uri, prefix, strlen(prefix)) != 0) {
    return HttpServer_SendError(ctx, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  char secret[65];
  size_t n = strcspn(uri + strlen(prefix), "?");
  if (n == 0 || n >= sizeof(secret)) {
    return HttpServer_SendEmpty(ctx, 204);
  }
  memcpy(secret, uri + strlen(prefix), n);
  secret[n] = '\0';
  Cloud_RevokeGrantSecret(secret);
  return HttpServer_SendEmpty(ctx, 204);
}

static const HttpServer_Route uris[] = {
    {.uri = "/cloud/grant-secrets", .method = HTTP_SERVER_GET, .handler = Route_CloudGrantListHandler},
    {.uri = "/cloud/grant-secrets/*", .method = HTTP_SERVER_DELETE, .handler = Route_CloudGrantDeleteHandler},
};

esp_err_t Route_CloudRegister(void)
{
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(HttpServer_RegisterRoutes(uris, TOOL_GET_ARRAY_LENGTH(uris)), "register cloud grant uri failed");
  return ESP_OK;
}
