/**
 * @name CMSIS-DAP routes
 * @file dap.c
 * @author xqe2011
 */
#include "route.h"

#include "dap_ctrl.h"
#include "http_server.h"
#include "lock.h"
#include "tool.h"

#include <cJSON.h>
#include <stdio.h>
#include <string.h>

static const char* tag = "SAIHub-Dap";

static const char* notRunning = "CMSIS-DAP is not running. POST /dap/config with enable true first.";

static int Route_ParseOptionalPin(cJSON* pinsObj, const char* key, int* pinOut, bool required, char* reason,
                                  size_t reasonLen)
{
  cJSON* item = cJSON_GetObjectItem(pinsObj, key);
  if (item == NULL || cJSON_IsNull(item)) {
    if (required) {
      snprintf(reason, reasonLen, "pins.%s is required.", key);
      return -1;
    }
    *pinOut = -1;
    return 0;
  }
  if (!cJSON_IsNumber(item)) {
    snprintf(reason, reasonLen, "pins.%s must be an integer pin%s.", key, required ? "" : ", null, or omitted");
    return -1;
  }
  *pinOut = item->valueint;
  return 0;
}

static esp_err_t Route_ParseConfigBody(cJSON* body, DapCtrl_Config* cfg, char* reason, size_t reasonLen)
{
  cJSON* enableItem = cJSON_GetObjectItem(body, "enable");
  cJSON* pinsItem = cJSON_GetObjectItem(body, "pins");
  if (!cJSON_IsBool(enableItem)) {
    snprintf(reason, reasonLen, "enable must be boolean.");
    return ESP_ERR_INVALID_ARG;
  }
  if (!cJSON_IsObject(pinsItem)) {
    snprintf(reason, reasonLen, "pins must be an object with swclk, swdio, and optional nreset.");
    return ESP_ERR_INVALID_ARG;
  }
  if (Route_ParseOptionalPin(pinsItem, "swclk", &cfg->swclkPin, true, reason, reasonLen) != 0 ||
      Route_ParseOptionalPin(pinsItem, "swdio", &cfg->swdioPin, true, reason, reasonLen) != 0 ||
      Route_ParseOptionalPin(pinsItem, "nreset", &cfg->nresetPin, false, reason, reasonLen) != 0) {
    return ESP_ERR_INVALID_ARG;
  }
  cfg->enable = cJSON_IsTrue(enableItem);
  return ESP_OK;
}

static int Route_CheckPinLocks(HttpServer_Context* ctx, const DapCtrl_Config* cfg, char* lockId, size_t lockIdLen,
                               char* reason, size_t reasonLen)
{
  int pins[3];
  size_t count = 0;
  pins[count++] = cfg->swclkPin;
  pins[count++] = cfg->swdioPin;
  if (cfg->nresetPin >= 0) pins[count++] = cfg->nresetPin;
  DapCtrl_Config current;
  DapCtrl_GetConfig(&current);
  int status = 0;
  for (size_t i = 0; i < count; i++) {
    int st = HttpServer_LockStatus(ctx, LOCK_KIND_GPIO, pins[i], LOCK_METHOD_WRITE, lockId, lockIdLen, reason, reasonLen);
    /* Reconfiguration must be allowed through the pins already reserved by DAP itself. */
    bool owned = current.enable &&
                 (pins[i] == current.swclkPin || pins[i] == current.swdioPin || pins[i] == current.nresetPin);
    if (st == 423 && owned) st = 0;
    if (st) status = st;
  }
  return status;
}

static void Route_DapAddPin(cJSON* pins, const char* key, int pin)
{
  if (pin < 0) cJSON_AddNullToObject(pins, key);
  else cJSON_AddNumberToObject(pins, key, pin);
}

static esp_err_t Route_DapGetConfigHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  DapCtrl_Config cfg;
  if (DapCtrl_GetConfig(&cfg) != ESP_OK) return HttpServer_SendError(ctx, 500, "internal");
  cJSON* root = cJSON_CreateObject();
  cJSON* pins = cJSON_CreateObject();
  if (root == NULL || pins == NULL) {
    cJSON_Delete(root);
    cJSON_Delete(pins);
    return HttpServer_SendError(ctx, 500, "internal");
  }
  cJSON_AddBoolToObject(root, "enable", cfg.enable);
  Route_DapAddPin(pins, "swclk", cfg.swclkPin);
  Route_DapAddPin(pins, "swdio", cfg.swdioPin);
  Route_DapAddPin(pins, "nreset", cfg.nresetPin);
  cJSON_AddItemToObject(root, "pins", pins);
  return HttpServer_SendJson(ctx, 200, root);
}

static esp_err_t Route_DapStatusHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  DapCtrl_Status status;
  if (DapCtrl_GetStatus(&status) != ESP_OK) return HttpServer_SendError(ctx, 503, notRunning);
  cJSON* root = cJSON_CreateObject();
  if (root == NULL) return HttpServer_SendError(ctx, 500, "internal");
  cJSON_AddStringToObject(root, "prompt", status.prompt ? status.prompt : "");
  cJSON_AddNumberToObject(root, "port", status.port);
  cJSON_AddStringToObject(root, "ip", status.ip);
  return HttpServer_SendJson(ctx, 200, root);
}

static esp_err_t Route_DapPostConfigHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  Lock_SweepExpired();
  if (!HttpServer_HasJsonContentType(ctx)) {
    return HttpServer_SendError(ctx, 415, "Content-Type must be application/json.");
  }

  char reason[256];
  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(ctx, &perr);
  if (body == NULL) return HttpServer_SendError(ctx, 400, "invalid_json");

  DapCtrl_Config cfg;
  memset(&cfg, 0, sizeof(cfg));
  if (Route_ParseConfigBody(body, &cfg, reason, sizeof(reason)) != ESP_OK) {
    cJSON_Delete(body);
    return HttpServer_SendError(ctx, 400, reason);
  }
  cJSON_Delete(body);

  char lockId[64];
  int st = Route_CheckPinLocks(ctx, &cfg, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return HttpServer_SendError(ctx, st, reason);

  esp_err_t ret = DapCtrl_SetConfig(&cfg, reason, sizeof(reason));
  if (ret == ESP_ERR_INVALID_ARG) return HttpServer_SendError(ctx, 400, reason);
  if (ret == ESP_ERR_INVALID_STATE) return HttpServer_SendError(ctx, 409, reason);
  if (ret != ESP_OK) return HttpServer_SendError(ctx, 500, reason[0] ? reason : "internal");

  Lock_Touch(lockId);
  return HttpServer_SendEmpty(ctx, 204);
}

static const HttpServer_Route uris[] = {
    {.uri = "/dap/status", .method = HTTP_SERVER_GET, .handler = Route_DapStatusHandler},
    {.uri = "/dap/config", .method = HTTP_SERVER_GET, .handler = Route_DapGetConfigHandler},
    {.uri = "/dap/config", .method = HTTP_SERVER_POST, .handler = Route_DapPostConfigHandler},
};

esp_err_t Route_DapRegister(void)
{
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(HttpServer_RegisterRoutes(uris, TOOL_GET_ARRAY_LENGTH(uris)), "register dap uri failed");
  return ESP_OK;
}
