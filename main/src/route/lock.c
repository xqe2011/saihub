/**
 * @name Lock routes
 * @file lock.c
 * @author xqe2011
 */
#include "route.h"

#include "gpio_ctrl.h"
#include "http_server.h"
#include "lock.h"
#include "tool.h"

#include <cJSON.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <stdio.h>
#include <string.h>

static const char* tag = "SAIHUB-Http";

static esp_err_t Route_LockCreateHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Lock_SweepExpired();
  if (!HttpServer_HasJsonContentType(req)) {
    return HttpServer_SendError(req, 415, "Content-Type must be application/json.");
  }
  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(req, &perr);
  if (body == NULL) return HttpServer_SendError(req, 400, "invalid_json");

  cJSON* resources = cJSON_GetObjectItem(body, "resources");
  if (!cJSON_IsArray(resources) || cJSON_GetArraySize(resources) == 0) {
    cJSON_Delete(body);
    return HttpServer_SendError(req, 400, "resources must be a non-empty array.");
  }

  Lock_Resource res[LOCK_MAX_RESOURCES];
  size_t count = (size_t)cJSON_GetArraySize(resources);
  if (count > LOCK_MAX_RESOURCES) {
    cJSON_Delete(body);
    return HttpServer_SendError(req, 400, "resources array is too long.");
  }

  char range[32];
  HttpServer_FormatPinRange(range, sizeof(range));

  for (size_t i = 0; i < count; i++) {
    cJSON* item = cJSON_GetArrayItem(resources, (int)i);
    cJSON* pinItem = cJSON_GetObjectItem(item, "pin");
    cJSON* powerItem = cJSON_GetObjectItem(item, "power");
    cJSON* methods = cJSON_GetObjectItem(item, "method");

    bool hasPin = cJSON_IsNumber(pinItem);
    bool hasPower = cJSON_IsString(powerItem);
    if (hasPin == hasPower) {
      cJSON_Delete(body);
      return HttpServer_SendError(req, 400,
                            "Each resource must have exactly one of pin or power.");
    }

    Lock_Kind kind;
    int pin = 0;
    if (hasPin) {
      if (!GpioCtrl_IsValidLogicalPin(pinItem->valueint)) {
        cJSON_Delete(body);
        char reason[96];
        snprintf(reason, sizeof(reason), "resources[%u].pin is invalid. Use a pin from %s.", (unsigned)i, range);
        return HttpServer_SendError(req, 400, reason);
      }
      kind = LOCK_KIND_GPIO;
      pin = pinItem->valueint;
    } else {
      if (!Lock_PowerFromString(powerItem->valuestring, &kind)) {
        cJSON_Delete(body);
        return HttpServer_SendError(req, 400, "power is invalid. Use one of: 3v3, 5v.");
      }
    }

    if (!cJSON_IsArray(methods) || cJSON_GetArraySize(methods) == 0) {
      cJSON_Delete(body);
      return HttpServer_SendError(req, 400, "Each resource.method entry must be read or write.");
    }
    uint8_t bits = 0;
    for (int m = 0; m < cJSON_GetArraySize(methods); m++) {
      cJSON* mv = cJSON_GetArrayItem(methods, m);
      if (!cJSON_IsString(mv)) {
        cJSON_Delete(body);
        return HttpServer_SendError(req, 400, "Each resource.method entry must be read or write.");
      }
      if (strcmp(mv->valuestring, "read") == 0) bits |= LOCK_METHOD_READ;
      else if (strcmp(mv->valuestring, "write") == 0)
        bits |= LOCK_METHOD_WRITE;
      else {
        cJSON_Delete(body);
        return HttpServer_SendError(req, 400, "method is invalid. Each resource.method entry must be read or write.");
      }
    }
    res[i].kind = kind;
    res[i].pin = pin;
    res[i].methods = bits;
  }
  cJSON_Delete(body);

  Lock_Entry created;
  esp_err_t cret = Lock_Create(res, count, &created);
  if (cret == ESP_ERR_INVALID_STATE) {
    char reason[160];
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
    return HttpServer_SendError(req, 409, reason);
  }
  if (cret != ESP_OK) {
    return HttpServer_SendError(req, 500, "internal");
  }

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
  Lock_Entry entry;
  if (Lock_Renew(id, &entry) != ESP_OK) {
    char reason[160];
    snprintf(reason, sizeof(reason),
             "Lock id %s does not exist. Create a lock with POST /lock (see GET /openapi.json).", id);
    return HttpServer_SendError(req, 404, reason);
  }
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "id", entry.id);
  int64_t now = Lock_NowUs();
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
