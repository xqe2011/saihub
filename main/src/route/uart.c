/**
 * @name UART routes
 * @file uart.c
 * @author xqe2011
 */
#include "route.h"

#include "config.h"
#include "http_server.h"
#include "lock.h"
#include "tool.h"
#include "uart_ctrl.h"

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* tag = "SAIHub-Uart";

static int Route_ParseUartPathId(const char* uri, const char* suffix, int* idOut)
{
  const char* prefix = "/uart/";
  if (strncmp(uri, prefix, strlen(prefix)) != 0) return -1;
  const char* p = uri + strlen(prefix);
  if (*p < '0' || *p > '9') return -2;
  char* end = NULL;
  long raw = strtol(p, &end, 10);
  if (end == p) return -2;
  if (suffix) {
    size_t sl = strlen(suffix);
    if (strncmp(end, suffix, sl) != 0) return -2;
    if (end[sl] != '\0' && end[sl] != '?') return -2;
  } else if (*end != '\0' && *end != '?') {
    return -2;
  }
  *idOut = (int)raw;
  if (!UartCtrl_IsValidId(*idOut)) return -3;
  return 0;
}

static esp_err_t Route_UartIdError(HttpServer_Context* ctx, int pr)
{
  if (pr == -2) {
    return HttpServer_SendError(ctx, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
  }
  if (pr == -3) {
    char reason[128];
    const char* p = strstr(HttpServer_GetUri(ctx), "/uart/");
    long raw = p ? strtol(p + 6, NULL, 10) : -1;
    snprintf(reason, sizeof(reason), "UART %ld does not exist. Use an id from GET /uart/.", raw);
    return HttpServer_SendError(ctx, 404, reason);
  }
  return HttpServer_SendError(ctx, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
}

static void Route_UartAddOptionalPin(cJSON* pins, const char* key, int pin)
{
  if (pin < 0) {
    cJSON_AddNullToObject(pins, key);
  } else {
    cJSON_AddNumberToObject(pins, key, pin);
  }
}

static bool Route_UartIsEnabled(int id, char* reason, size_t reasonLen)
{
  UartCtrl_Config cfg;
  if (UartCtrl_GetConfig(id, &cfg) != ESP_OK || !cfg.enable) {
    snprintf(reason, reasonLen, "UART %d is not enabled. POST /uart/%d/config with enable true first.", id, id);
    return false;
  }
  return true;
}

static cJSON* Route_UartConfigToJson(int id, const UartCtrl_Config* cfg)
{
  cJSON* item = cJSON_CreateObject();
  cJSON_AddNumberToObject(item, "id", id);
  cJSON_AddBoolToObject(item, "enable", cfg->enable);
  cJSON_AddStringToObject(item, "mode", UartCtrl_ModeToString(cfg->mode));
  cJSON_AddNumberToObject(item, "baudRate", cfg->baudRate);
  cJSON_AddNumberToObject(item, "dataBits", cfg->dataBits);
  cJSON_AddStringToObject(item, "parity", UartCtrl_ParityToString(cfg->parity));
  cJSON_AddNumberToObject(item, "stopBits", cfg->stopBits);
  cJSON_AddStringToObject(item, "encoding", UartCtrl_EncodingToString(cfg->encoding));
  cJSON* pins = cJSON_CreateObject();
  Route_UartAddOptionalPin(pins, "rx", cfg->rxPin);
  Route_UartAddOptionalPin(pins, "tx", cfg->txPin);
  Route_UartAddOptionalPin(pins, "rts", cfg->rtsPin);
  Route_UartAddOptionalPin(pins, "dtr", cfg->dtrPin);
  cJSON_AddItemToObject(item, "pins", pins);
  return item;
}

static int Route_ParseUartOptionalPin(cJSON* pinsObj, const char* key, int* pinOut, char* reason, size_t reasonLen)
{
  cJSON* item = cJSON_GetObjectItem(pinsObj, key);
  if (item == NULL || cJSON_IsNull(item)) {
    *pinOut = -1;
    return 0;
  }
  if (!cJSON_IsNumber(item)) {
    snprintf(reason, reasonLen, "pins.%s must be an integer pin, null, or omitted.", key);
    return -1;
  }
  *pinOut = item->valueint;
  return 0;
}

static bool Route_UartRfc2217Busy(int id, char* reason, size_t reasonLen)
{
  UartCtrl_Config cfg;
  if (UartCtrl_GetConfig(id, &cfg) != ESP_OK || !cfg.enable || cfg.mode != UART_CTRL_MODE_RFC2217) return false;
  snprintf(reason, reasonLen,
           "UART %d mode is rfc2217. Data and control lines are owned by the TCP bridge. POST /uart/%d/config with mode native to use this API.",
           id, id);
  return true;
}

static esp_err_t Route_ParseUartConfigBody(int id, cJSON* body, UartCtrl_Config* cfg, char* reason, size_t reasonLen)
{
  cJSON* enableItem = cJSON_GetObjectItem(body, "enable");
  cJSON* modeItem = cJSON_GetObjectItem(body, "mode");
  cJSON* baudItem = cJSON_GetObjectItem(body, "baudRate");
  cJSON* dataBitsItem = cJSON_GetObjectItem(body, "dataBits");
  cJSON* parityItem = cJSON_GetObjectItem(body, "parity");
  cJSON* stopBitsItem = cJSON_GetObjectItem(body, "stopBits");
  cJSON* encodingItem = cJSON_GetObjectItem(body, "encoding");
  cJSON* pinsItem = cJSON_GetObjectItem(body, "pins");

  if (!cJSON_IsBool(enableItem)) {
    snprintf(reason, reasonLen, "enable must be boolean.");
    return ESP_ERR_INVALID_ARG;
  }
  if (modeItem == NULL) {
    cfg->mode = UART_CTRL_MODE_NATIVE;
  } else if (!cJSON_IsString(modeItem) || !UartCtrl_ModeFromString(modeItem->valuestring, &cfg->mode)) {
    snprintf(reason, reasonLen, "mode must be one of: native, rfc2217.");
    return ESP_ERR_INVALID_ARG;
  }
  if (!cJSON_IsObject(pinsItem)) {
    snprintf(reason, reasonLen, "pins must be an object with optional rx, tx, rts, and dtr.");
    return ESP_ERR_INVALID_ARG;
  }
  cfg->enable = cJSON_IsTrue(enableItem);
  if (Route_ParseUartOptionalPin(pinsItem, "rx", &cfg->rxPin, reason, reasonLen) != 0) return ESP_ERR_INVALID_ARG;
  if (Route_ParseUartOptionalPin(pinsItem, "tx", &cfg->txPin, reason, reasonLen) != 0) return ESP_ERR_INVALID_ARG;
  if (Route_ParseUartOptionalPin(pinsItem, "rts", &cfg->rtsPin, reason, reasonLen) != 0) return ESP_ERR_INVALID_ARG;
  if (Route_ParseUartOptionalPin(pinsItem, "dtr", &cfg->dtrPin, reason, reasonLen) != 0) return ESP_ERR_INVALID_ARG;

  if (cfg->mode == UART_CTRL_MODE_RFC2217) {
    if (baudItem != NULL || dataBitsItem != NULL || parityItem != NULL || stopBitsItem != NULL || encodingItem != NULL) {
      snprintf(reason, reasonLen, "mode rfc2217 only accepts enable, pins, and mode.");
      return ESP_ERR_INVALID_ARG;
    }
    UartCtrl_Config current;
    if (UartCtrl_GetConfig(id, &current) != ESP_OK) return ESP_FAIL;
    cfg->baudRate = current.baudRate;
    cfg->dataBits = current.dataBits;
    cfg->parity = current.parity;
    cfg->stopBits = current.stopBits;
    cfg->encoding = current.encoding;
    return ESP_OK;
  }

  if (!cJSON_IsNumber(baudItem)) {
    snprintf(reason, reasonLen, "baudRate must be a positive integer.");
    return ESP_ERR_INVALID_ARG;
  }
  if (!cJSON_IsNumber(dataBitsItem)) {
    snprintf(reason, reasonLen, "dataBits must be 5, 6, 7, or 8.");
    return ESP_ERR_INVALID_ARG;
  }
  if (!cJSON_IsString(parityItem) || !UartCtrl_ParityFromString(parityItem->valuestring, &cfg->parity)) {
    snprintf(reason, reasonLen, "parity must be one of: none, even, odd.");
    return ESP_ERR_INVALID_ARG;
  }
  if (!cJSON_IsNumber(stopBitsItem)) {
    snprintf(reason, reasonLen, "stopBits must be 1 or 2.");
    return ESP_ERR_INVALID_ARG;
  }
  if (!cJSON_IsString(encodingItem) || !UartCtrl_EncodingFromString(encodingItem->valuestring, &cfg->encoding)) {
    snprintf(reason, reasonLen, "encoding must be one of: utf8, byte.");
    return ESP_ERR_INVALID_ARG;
  }

  cfg->baudRate = baudItem->valueint;
  cfg->dataBits = dataBitsItem->valueint;
  cfg->stopBits = stopBitsItem->valueint;
  return ESP_OK;
}

static esp_err_t Route_ParseTransmitData(cJSON* body, UartCtrl_Encoding encoding, uint8_t** dataOut, size_t* lenOut,
                                         char* reason, size_t reasonLen)
{
  *dataOut = NULL;
  *lenOut = 0;
  cJSON* dataItem = cJSON_GetObjectItem(body, "data");
  if (encoding == UART_CTRL_ENCODING_UTF8) {
    if (!cJSON_IsString(dataItem) || dataItem->valuestring == NULL) {
      snprintf(reason, reasonLen, "data must be a UTF-8 string when encoding is utf8.");
      return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(dataItem->valuestring);
    if (len > CONFIG_UART_MAX_PAYLOAD_BYTES) {
      snprintf(reason, reasonLen, "data is too large. Send at most %u bytes per request.",
               (unsigned)CONFIG_UART_MAX_PAYLOAD_BYTES);
      return ESP_ERR_INVALID_SIZE;
    }
    uint8_t* buf = malloc(len ? len : 1);
    if (buf == NULL) return ESP_ERR_NO_MEM;
    if (len > 0) memcpy(buf, dataItem->valuestring, len);
    *dataOut = buf;
    *lenOut = len;
    return ESP_OK;
  }

  if (!cJSON_IsString(dataItem) || dataItem->valuestring == NULL) {
    snprintf(reason, reasonLen, "data must be an even-length hex string when encoding is byte.");
    return ESP_ERR_INVALID_ARG;
  }
  size_t chars = strlen(dataItem->valuestring);
  if ((chars & 1U) != 0) {
    snprintf(reason, reasonLen, "data must be an even-length hex string when encoding is byte.");
    return ESP_ERR_INVALID_ARG;
  }
  size_t n = chars / 2;
  if (n > CONFIG_UART_MAX_PAYLOAD_BYTES) {
    snprintf(reason, reasonLen, "data is too large. Send at most %u bytes per request.",
             (unsigned)CONFIG_UART_MAX_PAYLOAD_BYTES);
    return ESP_ERR_INVALID_SIZE;
  }
  uint8_t* buf = malloc(n > 0 ? n : 1);
  if (buf == NULL) return ESP_ERR_NO_MEM;
  size_t decoded = 0;
  if (!Tool_HexDecode(dataItem->valuestring, buf, n > 0 ? n : 1, &decoded)) {
    free(buf);
    snprintf(reason, reasonLen, "data must be an even-length hex string when encoding is byte.");
    return ESP_ERR_INVALID_ARG;
  }
  *dataOut = buf;
  *lenOut = decoded;
  return ESP_OK;
}

static cJSON* Route_EncodeReceiveData(UartCtrl_Encoding encoding, const uint8_t* data, size_t len)
{
  if (encoding == UART_CTRL_ENCODING_UTF8) {
    char* s = malloc(len + 1);
    if (s == NULL) return NULL;
    if (len > 0) memcpy(s, data, len);
    s[len] = '\0';
    cJSON* item = cJSON_CreateString(s);
    free(s);
    return item;
  }
  char* s = malloc(len * 2 + 1);
  if (s == NULL) return NULL;
  Tool_HexEncode(data, len, s);
  cJSON* item = cJSON_CreateString(s);
  free(s);
  return item;
}

static esp_err_t Route_UartListHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  TOOL_CALL_LOG("rest/%s %s %s", HttpServer_GetFrom(ctx), HttpServer_MethodName(HttpServer_GetMethod(ctx)), HttpServer_GetUri(ctx));
  Lock_SweepExpired();
  cJSON* root = cJSON_CreateObject();
  cJSON* arr = cJSON_CreateArray();
  int count = UartCtrl_GetCount();
  for (int i = 0; i < count; i++) {
    UartCtrl_Config cfg;
    UartCtrl_GetConfig(i, &cfg);
    cJSON_AddItemToArray(arr, Route_UartConfigToJson(i, &cfg));
  }
  cJSON_AddItemToObject(root, "uarts", arr);
  cJSON_AddNumberToObject(root, "time", (double)HttpServer_NowUs());
  return HttpServer_SendJson(ctx, 200, root);
}

static esp_err_t Route_UartPostConfigHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  Lock_SweepExpired();
  int id = 0;
  int pr = Route_ParseUartPathId(HttpServer_GetUri(ctx), "/config", &id);
  if (pr != 0) return Route_UartIdError(ctx, pr);

  if (!HttpServer_HasJsonContentType(ctx)) {
    return HttpServer_SendError(ctx, 415, "Content-Type must be application/json.");
  }

  char lockId[64];
  char reason[256];
  int st = HttpServer_LockStatus(ctx, LOCK_KIND_UART, id, LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return HttpServer_SendError(ctx, st, reason);

  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(ctx, &perr);
  if (body == NULL) return HttpServer_SendError(ctx, 400, "invalid_json");

  UartCtrl_Config cfg;
  memset(&cfg, 0, sizeof(cfg));
  if (Route_ParseUartConfigBody(id, body, &cfg, reason, sizeof(reason)) != ESP_OK) {
    cJSON_Delete(body);
    return HttpServer_SendError(ctx, 400, reason);
  }
  cJSON_Delete(body);

  esp_err_t ret = UartCtrl_SetConfig(id, &cfg, reason, sizeof(reason));
  if (ret == ESP_ERR_INVALID_ARG) return HttpServer_SendError(ctx, 400, reason);
  if (ret == ESP_ERR_INVALID_STATE) return HttpServer_SendError(ctx, 409, reason);
  if (ret != ESP_OK) return HttpServer_SendError(ctx, 500, reason[0] ? reason : "internal");

  Lock_Touch(lockId);
  return HttpServer_SendEmpty(ctx, 204);
}

static esp_err_t Route_UartPostTransmitHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  Lock_SweepExpired();
  int id = 0;
  int pr = Route_ParseUartPathId(HttpServer_GetUri(ctx), "/transmit", &id);
  if (pr != 0) return Route_UartIdError(ctx, pr);

  if (!HttpServer_HasJsonContentType(ctx)) {
    return HttpServer_SendError(ctx, 415, "Content-Type must be application/json.");
  }

  char lockId[64];
  char reason[256];
  int st = HttpServer_LockStatus(ctx, LOCK_KIND_UART, id, LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return HttpServer_SendError(ctx, st, reason);
  if (Route_UartRfc2217Busy(id, reason, sizeof(reason))) return HttpServer_SendError(ctx, 409, reason);
  if (!Route_UartIsEnabled(id, reason, sizeof(reason))) return HttpServer_SendError(ctx, 422, reason);

  UartCtrl_Config cfg;
  UartCtrl_GetConfig(id, &cfg);

  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(ctx, &perr);
  if (body == NULL) return HttpServer_SendError(ctx, 400, "invalid_json");

  uint8_t* data = NULL;
  size_t len = 0;
  esp_err_t parseRet = Route_ParseTransmitData(body, cfg.encoding, &data, &len, reason, sizeof(reason));
  cJSON_Delete(body);
  if (parseRet == ESP_ERR_INVALID_SIZE) return HttpServer_SendError(ctx, 400, reason);
  if (parseRet != ESP_OK) return HttpServer_SendError(ctx, 400, reason);

  esp_err_t ret = UartCtrl_Transmit(id, data, len, reason, sizeof(reason));
  free(data);
  if (ret == ESP_ERR_INVALID_STATE) return HttpServer_SendError(ctx, 422, reason);
  if (ret == ESP_ERR_INVALID_SIZE) return HttpServer_SendError(ctx, 400, reason);
  if (ret != ESP_OK) return HttpServer_SendError(ctx, 500, reason[0] ? reason : "internal");

  Lock_Touch(lockId);
  return HttpServer_SendEmpty(ctx, 204);
}

static esp_err_t Route_UartGetReceiveHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  Lock_SweepExpired();
  int id = 0;
  int pr = Route_ParseUartPathId(HttpServer_GetUri(ctx), "/receive", &id);
  if (pr != 0) return Route_UartIdError(ctx, pr);

  char lockId[64];
  char reason[256];
  int st = HttpServer_LockStatus(ctx, LOCK_KIND_UART, id, LOCK_METHOD_READ, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return HttpServer_SendError(ctx, st, reason);
  if (Route_UartRfc2217Busy(id, reason, sizeof(reason))) return HttpServer_SendError(ctx, 409, reason);
  if (!Route_UartIsEnabled(id, reason, sizeof(reason))) return HttpServer_SendError(ctx, 422, reason);

  UartCtrl_Config cfg;
  UartCtrl_GetConfig(id, &cfg);

  uint8_t* buf = malloc(CONFIG_UART_MAX_PAYLOAD_BYTES);
  if (buf == NULL) return HttpServer_SendError(ctx, 500, "internal");
  size_t n = 0;
  esp_err_t ret = UartCtrl_Receive(id, buf, CONFIG_UART_MAX_PAYLOAD_BYTES, &n, reason, sizeof(reason));
  if (ret == ESP_ERR_INVALID_STATE) {
    free(buf);
    return HttpServer_SendError(ctx, 422, reason);
  }
  if (ret != ESP_OK) {
    free(buf);
    return HttpServer_SendError(ctx, 500, reason[0] ? reason : "internal");
  }

  cJSON* root = cJSON_CreateObject();
  cJSON* data = Route_EncodeReceiveData(cfg.encoding, buf, n);
  free(buf);
  if (data == NULL) {
    cJSON_Delete(root);
    return HttpServer_SendError(ctx, 500, "internal");
  }
  cJSON_AddItemToObject(root, "data", data);
  cJSON_AddNumberToObject(root, "time", (double)HttpServer_NowUs());
  Lock_Touch(lockId);
  return HttpServer_SendJson(ctx, 200, root);
}

static esp_err_t Route_UartPostFlushHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  Lock_SweepExpired();
  int id = 0;
  int pr = Route_ParseUartPathId(HttpServer_GetUri(ctx), "/flush", &id);
  if (pr != 0) return Route_UartIdError(ctx, pr);

  char lockId[64];
  char reason[256];
  int st = HttpServer_LockStatus(ctx, LOCK_KIND_UART, id, LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return HttpServer_SendError(ctx, st, reason);
  if (Route_UartRfc2217Busy(id, reason, sizeof(reason))) return HttpServer_SendError(ctx, 409, reason);

  esp_err_t ret = UartCtrl_Flush(id, reason, sizeof(reason));
  if (ret == ESP_ERR_INVALID_STATE) return HttpServer_SendError(ctx, 422, reason);
  if (ret != ESP_OK) return HttpServer_SendError(ctx, 500, reason[0] ? reason : "internal");

  Lock_Touch(lockId);
  return HttpServer_SendEmpty(ctx, 204);
}

static esp_err_t Route_UartGetControlLine(HttpServer_Context* ctx, const char* suffix,
                                         esp_err_t (*getter)(int, bool*, char*, size_t))
{
  HttpServer_LogCall(ctx);
  Lock_SweepExpired();
  int id = 0;
  int pr = Route_ParseUartPathId(HttpServer_GetUri(ctx), suffix, &id);
  if (pr != 0) return Route_UartIdError(ctx, pr);

  char lockId[64];
  char reason[256];
  int st = HttpServer_LockStatus(ctx, LOCK_KIND_UART, id, LOCK_METHOD_READ, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return HttpServer_SendError(ctx, st, reason);
  if (Route_UartRfc2217Busy(id, reason, sizeof(reason))) return HttpServer_SendError(ctx, 409, reason);

  bool active = false;
  esp_err_t ret = getter(id, &active, reason, sizeof(reason));
  if (ret == ESP_ERR_INVALID_STATE) return HttpServer_SendError(ctx, 422, reason);
  if (ret != ESP_OK) return HttpServer_SendError(ctx, 500, reason[0] ? reason : "internal");

  cJSON* root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "active", active);
  cJSON_AddNumberToObject(root, "time", (double)HttpServer_NowUs());
  Lock_Touch(lockId);
  return HttpServer_SendJson(ctx, 200, root);
}

static esp_err_t Route_UartPostControlLine(HttpServer_Context* ctx, const char* suffix,
                                          esp_err_t (*setter)(int, bool, char*, size_t))
{
  HttpServer_LogCall(ctx);
  Lock_SweepExpired();
  int id = 0;
  int pr = Route_ParseUartPathId(HttpServer_GetUri(ctx), suffix, &id);
  if (pr != 0) return Route_UartIdError(ctx, pr);

  if (!HttpServer_HasJsonContentType(ctx)) {
    return HttpServer_SendError(ctx, 415, "Content-Type must be application/json.");
  }

  char lockId[64];
  char reason[256];
  int st = HttpServer_LockStatus(ctx, LOCK_KIND_UART, id, LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return HttpServer_SendError(ctx, st, reason);
  if (Route_UartRfc2217Busy(id, reason, sizeof(reason))) return HttpServer_SendError(ctx, 409, reason);

  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(ctx, &perr);
  if (body == NULL) return HttpServer_SendError(ctx, 400, "invalid_json");

  cJSON* activeItem = cJSON_GetObjectItem(body, "active");
  if (!cJSON_IsBool(activeItem)) {
    cJSON_Delete(body);
    return HttpServer_SendError(ctx, 400, "active must be boolean.");
  }
  bool active = cJSON_IsTrue(activeItem);
  cJSON_Delete(body);

  esp_err_t ret = setter(id, active, reason, sizeof(reason));
  if (ret == ESP_ERR_INVALID_ARG) return HttpServer_SendError(ctx, 400, reason);
  if (ret == ESP_ERR_INVALID_STATE) return HttpServer_SendError(ctx, 422, reason);
  if (ret != ESP_OK) return HttpServer_SendError(ctx, 500, reason[0] ? reason : "internal");

  Lock_Touch(lockId);
  return HttpServer_SendEmpty(ctx, 204);
}

static esp_err_t Route_UartGetRfc2217Handler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  int id = 0;
  int pr = Route_ParseUartPathId(HttpServer_GetUri(ctx), "/rfc2217", &id);
  if (pr != 0) return Route_UartIdError(ctx, pr);

  UartCtrl_Rfc2217Status status;
  if (UartCtrl_GetRfc2217Status(id, &status) != ESP_OK) {
    char reason[160];
    snprintf(reason, sizeof(reason),
             "RFC2217 is not running. POST /uart/%d/config with enable true and mode rfc2217 first.", id);
    return HttpServer_SendError(ctx, 503, reason);
  }
  cJSON* root = cJSON_CreateObject();
  if (root == NULL) return HttpServer_SendError(ctx, 500, "internal");
  cJSON_AddStringToObject(root, "prompt", status.prompt ? status.prompt : "");
  cJSON_AddNumberToObject(root, "port", status.port);
  cJSON_AddStringToObject(root, "ip", status.ip);
  return HttpServer_SendJson(ctx, 200, root);
}

static esp_err_t Route_UartGetDtrHandler(HttpServer_Context* ctx)
{
  return Route_UartGetControlLine(ctx, "/dtr", UartCtrl_GetDtrActive);
}

static esp_err_t Route_UartGetRtsHandler(HttpServer_Context* ctx)
{
  return Route_UartGetControlLine(ctx, "/rts", UartCtrl_GetRtsActive);
}

static esp_err_t Route_UartPostDtrHandler(HttpServer_Context* ctx)
{
  return Route_UartPostControlLine(ctx, "/dtr", UartCtrl_SetDtr);
}

static esp_err_t Route_UartPostRtsHandler(HttpServer_Context* ctx)
{
  return Route_UartPostControlLine(ctx, "/rts", UartCtrl_SetRts);
}

static const HttpServer_Route uris[] = {
    {.uri = "/uart/", .method = HTTP_SERVER_GET, .handler = Route_UartListHandler},
    {.uri = "/uart/*/config", .method = HTTP_SERVER_POST, .handler = Route_UartPostConfigHandler},
    {.uri = "/uart/*/rfc2217", .method = HTTP_SERVER_GET, .handler = Route_UartGetRfc2217Handler},
    {.uri = "/uart/*/transmit", .method = HTTP_SERVER_POST, .handler = Route_UartPostTransmitHandler},
    {.uri = "/uart/*/receive", .method = HTTP_SERVER_GET, .handler = Route_UartGetReceiveHandler},
    {.uri = "/uart/*/flush", .method = HTTP_SERVER_POST, .handler = Route_UartPostFlushHandler},
    {.uri = "/uart/*/dtr", .method = HTTP_SERVER_GET, .handler = Route_UartGetDtrHandler},
    {.uri = "/uart/*/dtr", .method = HTTP_SERVER_POST, .handler = Route_UartPostDtrHandler},
    {.uri = "/uart/*/rts", .method = HTTP_SERVER_GET, .handler = Route_UartGetRtsHandler},
    {.uri = "/uart/*/rts", .method = HTTP_SERVER_POST, .handler = Route_UartPostRtsHandler},
};

esp_err_t Route_UartRegister(void)
{
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(HttpServer_RegisterRoutes(uris, TOOL_GET_ARRAY_LENGTH(uris)), "register uart uri failed");
  return ESP_OK;
}
