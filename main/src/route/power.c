/**
 * @name Power routes
 * @file power.c
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
#include <string.h>

static const char* tag = "SAIHUB-Http";

static Lock_Kind Route_PowerLockKind(GpioCtrl_PowerRail rail)
{
  return rail == GPIO_CTRL_POWER_3V3 ? LOCK_KIND_POWER_3V3 : LOCK_KIND_POWER_5V;
}

static bool Route_PowerParseRail(const char* uri, GpioCtrl_PowerRail* railOut)
{
  if (strcmp(uri, "/power/3v3") == 0) {
    *railOut = GPIO_CTRL_POWER_3V3;
    return true;
  }
  if (strcmp(uri, "/power/5v") == 0) {
    *railOut = GPIO_CTRL_POWER_5V;
    return true;
  }
  return false;
}

static esp_err_t Route_PowerGetHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Lock_SweepExpired();

  GpioCtrl_PowerRail rail;
  if (!Route_PowerParseRail(req->uri, &rail)) {
    return HttpServer_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }

  char lockId[64];
  char reason[256];
  int st = HttpServer_LockStatus(req, Route_PowerLockKind(rail), 0, LOCK_METHOD_READ, lockId, sizeof(lockId), reason,
                           sizeof(reason));
  if (st) return HttpServer_SendError(req, st, reason);

  bool enable = false;
  if (GpioCtrl_GetPowerEnable(rail, &enable) != ESP_OK) {
    return HttpServer_SendError(req, 500, "internal");
  }
  Lock_Touch(lockId[0] ? lockId : NULL);
  cJSON* root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "enable", enable);
  cJSON_AddNumberToObject(root, "time", (double)HttpServer_NowUs());
  return HttpServer_SendJson(req, 200, root);
}

static esp_err_t Route_PowerPostHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Lock_SweepExpired();

  GpioCtrl_PowerRail rail;
  if (!Route_PowerParseRail(req->uri, &rail)) {
    return HttpServer_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }

  if (!HttpServer_HasJsonContentType(req)) {
    return HttpServer_SendError(req, 415, "Content-Type must be application/json.");
  }

  char lockId[64];
  char reason[256];
  int st = HttpServer_LockStatus(req, Route_PowerLockKind(rail), 0, LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason,
                           sizeof(reason));
  if (st) return HttpServer_SendError(req, st, reason);

  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(req, &perr);
  if (body == NULL) return HttpServer_SendError(req, 400, "invalid_json");

  cJSON* enableItem = cJSON_GetObjectItem(body, "enable");
  if (!cJSON_IsBool(enableItem)) {
    cJSON_Delete(body);
    return HttpServer_SendError(req, 400, "enable must be boolean.");
  }
  bool enable = cJSON_IsTrue(enableItem);
  cJSON_Delete(body);

  if (GpioCtrl_SetPowerEnable(rail, enable) != ESP_OK) {
    return HttpServer_SendError(req, 500, "internal");
  }
  Lock_Touch(lockId[0] ? lockId : NULL);
  return HttpServer_SendEmpty(req, 204);
}

static const httpd_uri_t uris[] = {
    {.uri = "/power/3v3", .method = HTTP_GET, .handler = Route_PowerGetHandler},
    {.uri = "/power/3v3", .method = HTTP_POST, .handler = Route_PowerPostHandler},
    {.uri = "/power/5v", .method = HTTP_GET, .handler = Route_PowerGetHandler},
    {.uri = "/power/5v", .method = HTTP_POST, .handler = Route_PowerPostHandler},
};

esp_err_t Route_PowerRegister(httpd_handle_t server)
{
  for (size_t i = 0; i < TOOL_GET_ARRAY_LENGTH(uris); i++) {
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(httpd_register_uri_handler(server, &uris[i]), "register power uri failed");
  }
  return ESP_OK;
}
