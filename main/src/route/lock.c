/**
 * @name Lock routes
 * @file lock.c
 * @author xqe2011
 */
#include "route.h"

#include "api.pb.h"
#include "api_conv.h"
#include "config.h"
#include "http_server.h"
#include "lock.h"
#include "tool.h"

#include <cJSON.h>
#include <esp_http_server.h>
#include <stdio.h>
#include <string.h>

static const char* tag = "SAIHUB-Lock";

static esp_err_t Route_LockCreateHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Lock_SweepExpired();
  if (!HttpServer_HasApiContentType(req)) {
    return HttpServer_SendError(req, 415, HttpServer_UnsupportedMediaTypeReason());
  }

  Lock_Resource res[CONFIG_LOCK_MAX_RESOURCES];
  size_t count = 0;
  char reason[192];

  if (HttpServer_HasProtobufContentType(req)) {
    saihub_api_LockCreateBody body = saihub_api_LockCreateBody_init_zero;
    esp_err_t dr = HttpServer_DecodePb(req, saihub_api_LockCreateBody_fields, &body);
    if (dr != ESP_OK) return HttpServer_SendError(req, 400, "invalid_protobuf");
    if (ApiConv_ParseLockResources(body.resources, body.resources_count, res, CONFIG_LOCK_MAX_RESOURCES, &count, reason,
                                   sizeof(reason)) != ESP_OK) {
      return HttpServer_SendError(req, 400, reason);
    }
  } else {
    esp_err_t perr = ESP_OK;
    cJSON* body = HttpServer_ParseBody(req, &perr);
    if (body == NULL) return HttpServer_SendError(req, 400, "invalid_json");

    cJSON* resources = cJSON_GetObjectItem(body, "resources");
    if (HttpServer_ParseLockResources(resources, res, CONFIG_LOCK_MAX_RESOURCES, &count, reason, sizeof(reason)) !=
        ESP_OK) {
      cJSON_Delete(body);
      return HttpServer_SendError(req, 400, reason);
    }
    cJSON_Delete(body);
  }

  Lock_Entry created;
  Lock_Conflict conflict;
  memset(&conflict, 0, sizeof(conflict));
  esp_err_t cret = Lock_Create(res, count, &created, &conflict);
  if (cret == ESP_ERR_INVALID_STATE) {
    HttpServer_FormatLockConflict(&conflict, reason, sizeof(reason));
    return HttpServer_SendError(req, 409, reason);
  }
  if (cret != ESP_OK) {
    return HttpServer_SendError(req, 500, "internal");
  }

  int64_t now = Lock_NowUs();
  if (HttpServer_HasProtobufContentType(req)) {
    saihub_api_Lock msg = saihub_api_Lock_init_zero;
    snprintf(msg.id, sizeof(msg.id), "%s", created.id);
    msg.ttl = created.expiresAtUs - now;
    msg.expiresAt = created.expiresAtUs;
    if (!ApiConv_SerializeLockResources(created.resources, created.resourceCount, &msg)) {
      return HttpServer_SendError(req, 500, "internal");
    }
    return HttpServer_SendPb(req, 201, saihub_api_Lock_fields, &msg);
  }

  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "id", created.id);
  cJSON_AddNumberToObject(root, "ttl", (double)(created.expiresAtUs - now));
  cJSON_AddNumberToObject(root, "expiresAt", (double)created.expiresAtUs);
  cJSON_AddItemToObject(root, "resources", HttpServer_SerializeLockResources(created.resources, created.resourceCount));
  return HttpServer_SendJson(req, 201, root);
}

static esp_err_t Route_LockRenewHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Lock_SweepExpired();
  const char* prefix = "/lock/";
  if (strncmp(req->uri, prefix, strlen(prefix)) != 0) {
    return HttpServer_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  const char* id = req->uri + strlen(prefix);
  if (id[0] == '\0') {
    return HttpServer_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  TOOL_CALL_LOG("rest %s %s id=%s", HttpServer_MethodName(req->method), req->uri, id);
  Lock_Entry entry;
  if (Lock_Renew(id, &entry) != ESP_OK) {
    char reason[160];
    snprintf(reason, sizeof(reason),
             "Lock id %s does not exist. Create a lock with POST /lock (see GET /openapi.json).", id);
    return HttpServer_SendError(req, 404, reason);
  }
  int64_t now = Lock_NowUs();
  if (HttpServer_HasProtobufContentType(req)) {
    saihub_api_LockRenewResponse msg = saihub_api_LockRenewResponse_init_zero;
    snprintf(msg.id, sizeof(msg.id), "%s", entry.id);
    msg.ttl = entry.expiresAtUs - now;
    msg.expiresAt = entry.expiresAtUs;
    return HttpServer_SendPb(req, 200, saihub_api_LockRenewResponse_fields, &msg);
  }
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "id", entry.id);
  cJSON_AddNumberToObject(root, "ttl", (double)(entry.expiresAtUs - now));
  cJSON_AddNumberToObject(root, "expiresAt", (double)entry.expiresAtUs);
  return HttpServer_SendJson(req, 200, root);
}

static esp_err_t Route_LockDeleteHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Lock_SweepExpired();
  const char* prefix = "/lock/";
  if (strncmp(req->uri, prefix, strlen(prefix)) != 0) {
    return HttpServer_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  const char* id = req->uri + strlen(prefix);
  TOOL_CALL_LOG("rest %s %s id=%s", HttpServer_MethodName(req->method), req->uri, id);
  Lock_Delete(id); /* always 204 */
  return HttpServer_SendEmpty(req, 204);
}

static const httpd_uri_t uris[] = {
    {.uri = "/lock", .method = HTTP_POST, .handler = Route_LockCreateHandler},
    {.uri = "/lock/*", .method = HTTP_PUT, .handler = Route_LockRenewHandler},
    {.uri = "/lock/*", .method = HTTP_DELETE, .handler = Route_LockDeleteHandler},
};

esp_err_t Route_LockRegister(httpd_handle_t server)
{
  for (size_t i = 0; i < TOOL_GET_ARRAY_LENGTH(uris); i++) {
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(httpd_register_uri_handler(server, &uris[i]), "register lock uri failed");
  }
  return ESP_OK;
}
