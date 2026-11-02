/**
 * @name SPI routes
 * @file spi.c
 * @author xqe2011
 */
#include "route.h"

#include "http_server.h"
#include "lock.h"
#include "spi_ctrl.h"
#include "tool.h"

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* tag = "SAIHub-Spi";

static int Route_ParseSpiPathId(const char* uri, const char* suffix, int* idOut)
{
  const char* prefix = "/spi/";
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
  if (!SpiCtrl_IsValidId(*idOut)) return -3;
  return 0;
}

static esp_err_t Route_SpiIdError(HttpServer_Context* ctx, int pr)
{
  if (pr == -3) {
    char reason[128];
    const char* p = strstr(HttpServer_GetUri(ctx), "/spi/");
    long raw = p ? strtol(p + 5, NULL, 10) : -1;
    snprintf(reason, sizeof(reason), "SPI %ld does not exist. Use an id from GET /spi/.", raw);
    return HttpServer_SendError(ctx, 404, reason);
  }
  return HttpServer_SendError(ctx, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
}

static void Route_AddOptionalPin(cJSON* pins, const char* key, int pin)
{
  if (pin < 0) cJSON_AddNullToObject(pins, key);
  else cJSON_AddNumberToObject(pins, key, pin);
}

static cJSON* Route_SpiConfigToJson(int id, const SpiCtrl_Config* cfg)
{
  cJSON* item = cJSON_CreateObject();
  cJSON_AddNumberToObject(item, "id", id);
  cJSON_AddBoolToObject(item, "enable", cfg->enable);
  cJSON_AddStringToObject(item, "role", SpiCtrl_RoleToString(cfg->role));
  cJSON_AddNumberToObject(item, "cpol", cfg->cpol);
  cJSON_AddNumberToObject(item, "cpha", cfg->cpha);
  cJSON_AddStringToObject(item, "bitOrder", SpiCtrl_BitOrderToString(cfg->bitOrder));
  if (cfg->role == SPI_CTRL_ROLE_MASTER) cJSON_AddNumberToObject(item, "clockHz", cfg->clockHz);
  cJSON* pins = cJSON_CreateObject();
  Route_AddOptionalPin(pins, "mosi", cfg->mosiPin);
  Route_AddOptionalPin(pins, "miso", cfg->misoPin);
  Route_AddOptionalPin(pins, "clk", cfg->clkPin);
  Route_AddOptionalPin(pins, "cs", cfg->csPin);
  cJSON_AddItemToObject(item, "pins", pins);
  return item;
}

static int Route_ParseOptionalPin(cJSON* pinsObj, const char* key, int* pinOut, char* reason, size_t reasonLen)
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

static esp_err_t Route_ParseSpiConfigBody(cJSON* body, SpiCtrl_Config* cfg, char* reason, size_t reasonLen)
{
  cJSON* enableItem = cJSON_GetObjectItem(body, "enable");
  cJSON* roleItem = cJSON_GetObjectItem(body, "role");
  cJSON* cpolItem = cJSON_GetObjectItem(body, "cpol");
  cJSON* cphaItem = cJSON_GetObjectItem(body, "cpha");
  cJSON* bitItem = cJSON_GetObjectItem(body, "bitOrder");
  cJSON* clockItem = cJSON_GetObjectItem(body, "clockHz");
  cJSON* pinsItem = cJSON_GetObjectItem(body, "pins");

  if (!cJSON_IsBool(enableItem)) {
    snprintf(reason, reasonLen, "enable must be boolean.");
    return ESP_ERR_INVALID_ARG;
  }
  if (!cJSON_IsString(roleItem) || !SpiCtrl_RoleFromString(roleItem->valuestring, &cfg->role)) {
    snprintf(reason, reasonLen, "role must be one of: master, slave.");
    return ESP_ERR_INVALID_ARG;
  }
  if (!cJSON_IsNumber(cpolItem) || (cpolItem->valueint != 0 && cpolItem->valueint != 1)) {
    snprintf(reason, reasonLen, "cpol must be 0 or 1.");
    return ESP_ERR_INVALID_ARG;
  }
  if (!cJSON_IsNumber(cphaItem) || (cphaItem->valueint != 0 && cphaItem->valueint != 1)) {
    snprintf(reason, reasonLen, "cpha must be 0 or 1.");
    return ESP_ERR_INVALID_ARG;
  }
  if (!cJSON_IsString(bitItem) || !SpiCtrl_BitOrderFromString(bitItem->valuestring, &cfg->bitOrder)) {
    snprintf(reason, reasonLen, "bitOrder must be one of: msb, lsb.");
    return ESP_ERR_INVALID_ARG;
  }
  if (cfg->role == SPI_CTRL_ROLE_SLAVE) {
    if (clockItem != NULL) {
      snprintf(reason, reasonLen, "clockHz is not supported when role is slave.");
      return ESP_ERR_INVALID_ARG;
    }
    cfg->clockHz = 0;
  } else if (!cJSON_IsNumber(clockItem)) {
    snprintf(reason, reasonLen, "clockHz must be an integer from %d to %d.", CONFIG_SPI_CLOCK_HZ_MIN, CONFIG_SPI_CLOCK_HZ_MAX);
    return ESP_ERR_INVALID_ARG;
  } else {
    cfg->clockHz = clockItem->valueint;
  }
  if (!cJSON_IsObject(pinsItem)) {
    snprintf(reason, reasonLen, "pins must be an object with mosi, miso, clk, and cs.");
    return ESP_ERR_INVALID_ARG;
  }
  cfg->enable = cJSON_IsTrue(enableItem);
  cfg->cpol = cpolItem->valueint;
  cfg->cpha = cphaItem->valueint;
  if (Route_ParseOptionalPin(pinsItem, "mosi", &cfg->mosiPin, reason, reasonLen) != 0) return ESP_ERR_INVALID_ARG;
  if (Route_ParseOptionalPin(pinsItem, "miso", &cfg->misoPin, reason, reasonLen) != 0) return ESP_ERR_INVALID_ARG;
  if (Route_ParseOptionalPin(pinsItem, "clk", &cfg->clkPin, reason, reasonLen) != 0) return ESP_ERR_INVALID_ARG;
  if (Route_ParseOptionalPin(pinsItem, "cs", &cfg->csPin, reason, reasonLen) != 0) return ESP_ERR_INVALID_ARG;
  return ESP_OK;
}

static esp_err_t Route_ParseByteArray(cJSON* item, const char* field, int index, uint8_t* out, size_t* lenOut, char* reason,
                                      size_t reasonLen)
{
  if (!cJSON_IsArray(item)) {
    snprintf(reason, reasonLen, "mapping[%d].%s must be an array of integers 0-255.", index, field);
    return ESP_ERR_INVALID_ARG;
  }
  int n = cJSON_GetArraySize(item);
  if (n > CONFIG_SPI_MAX_TRANSFER_BYTES) {
    snprintf(reason, reasonLen, "mapping[%d].%s must contain at most %u bytes.", index, field,
             (unsigned)CONFIG_SPI_MAX_TRANSFER_BYTES);
    return ESP_ERR_INVALID_ARG;
  }
  for (int i = 0; i < n; i++) {
    cJSON* v = cJSON_GetArrayItem(item, i);
    if (!cJSON_IsNumber(v) || v->valueint < 0 || v->valueint > 255) {
      snprintf(reason, reasonLen, "mapping[%d].%s[%d] must be an integer from 0 to 255.", index, field, i);
      return ESP_ERR_INVALID_ARG;
    }
    out[i] = (uint8_t)v->valueint;
  }
  *lenOut = (size_t)n;
  return ESP_OK;
}

static esp_err_t Route_ParseSlaveMap(cJSON* body, uint8_t* packed, size_t* usedOut, char* reason, size_t reasonLen)
{
  *usedOut = 0;
  cJSON* mapping = cJSON_GetObjectItem(body, "mapping");
  if (!cJSON_IsArray(mapping)) {
    snprintf(reason, reasonLen, "mapping must be an array.");
    return ESP_ERR_INVALID_ARG;
  }
  int n = cJSON_GetArraySize(mapping);
  size_t used = 0;
  for (int i = 0; i < n; i++) {
    cJSON* entry = cJSON_GetArrayItem(mapping, i);
    if (!cJSON_IsObject(entry)) {
      snprintf(reason, reasonLen, "mapping[%d] must be an object.", i);
      return ESP_ERR_INVALID_ARG;
    }
    cJSON* typeItem = cJSON_GetObjectItem(entry, "type");
    SpiCtrl_MapType type;
    if (!cJSON_IsString(typeItem) || typeItem->valuestring == NULL) {
      snprintf(reason, reasonLen, "mapping[%d].type must be one of: exact, prefix.", i);
      return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(typeItem->valuestring, "exact") == 0) type = SPI_CTRL_MAP_EXACT;
    else if (strcmp(typeItem->valuestring, "prefix") == 0) type = SPI_CTRL_MAP_PREFIX;
    else {
      snprintf(reason, reasonLen, "mapping[%d].type must be one of: exact, prefix.", i);
      return ESP_ERR_INVALID_ARG;
    }
    uint8_t mosi[CONFIG_SPI_MAX_TRANSFER_BYTES];
    uint8_t miso[CONFIG_SPI_MAX_TRANSFER_BYTES];
    size_t mosiLen = 0;
    size_t misoLen = 0;
    if (Route_ParseByteArray(cJSON_GetObjectItem(entry, "mosi"), "mosi", i, mosi, &mosiLen, reason, reasonLen) != ESP_OK) {
      return ESP_ERR_INVALID_ARG;
    }
    if (Route_ParseByteArray(cJSON_GetObjectItem(entry, "miso"), "miso", i, miso, &misoLen, reason, reasonLen) != ESP_OK) {
      return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = SpiCtrl_PackMapAppend(packed, CONFIG_SPI_SLAVE_MAP_BYTES, &used, i, type, mosi, mosiLen, miso, misoLen,
                                          reason, reasonLen);
    if (err != ESP_OK) return err;
  }
  *usedOut = used;
  return ESP_OK;
}

static esp_err_t Route_SpiConfigStatus(esp_err_t ret, HttpServer_Context* ctx, const char* reason)
{
  if (ret == ESP_ERR_INVALID_ARG) return HttpServer_SendError(ctx, 400, reason);
  if (ret == ESP_ERR_INVALID_STATE) return HttpServer_SendError(ctx, 409, reason);
  return HttpServer_SendError(ctx, 500, reason[0] ? reason : "internal");
}

static esp_err_t Route_SpiCallStatus(esp_err_t ret, HttpServer_Context* ctx, const char* reason)
{
  if (ret == ESP_ERR_INVALID_ARG || ret == ESP_ERR_INVALID_SIZE) return HttpServer_SendError(ctx, 400, reason);
  if (ret == ESP_ERR_INVALID_STATE) return HttpServer_SendError(ctx, 422, reason);
  return HttpServer_SendError(ctx, 500, reason[0] ? reason : "internal");
}

static esp_err_t Route_SpiListHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  TOOL_CALL_LOG("rest/%s %s %s", HttpServer_GetFrom(ctx), HttpServer_MethodName(HttpServer_GetMethod(ctx)),
                HttpServer_GetUri(ctx));
  Lock_SweepExpired();
  cJSON* root = cJSON_CreateObject();
  cJSON* arr = cJSON_CreateArray();
  int count = SpiCtrl_GetCount();
  for (int i = 0; i < count; i++) {
    SpiCtrl_Config cfg;
    SpiCtrl_GetConfig(i, &cfg);
    cJSON_AddItemToArray(arr, Route_SpiConfigToJson(i, &cfg));
  }
  cJSON_AddItemToObject(root, "spis", arr);
  cJSON_AddNumberToObject(root, "time", (double)HttpServer_NowUs());
  return HttpServer_SendJson(ctx, 200, root);
}

static esp_err_t Route_SpiPostConfigHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  Lock_SweepExpired();
  int id = 0;
  int pr = Route_ParseSpiPathId(HttpServer_GetUri(ctx), "/config", &id);
  if (pr != 0) return Route_SpiIdError(ctx, pr);
  if (!HttpServer_HasJsonContentType(ctx)) return HttpServer_SendError(ctx, 415, "Content-Type must be application/json.");

  char lockId[64];
  char reason[256];
  int st = HttpServer_LockStatus(ctx, LOCK_KIND_SPI, id, LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return HttpServer_SendError(ctx, st, reason);

  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(ctx, &perr);
  if (body == NULL) return HttpServer_SendError(ctx, 400, "invalid_json");
  SpiCtrl_Config cfg;
  memset(&cfg, 0, sizeof(cfg));
  if (Route_ParseSpiConfigBody(body, &cfg, reason, sizeof(reason)) != ESP_OK) {
    cJSON_Delete(body);
    return HttpServer_SendError(ctx, 400, reason);
  }
  cJSON_Delete(body);
  esp_err_t ret = SpiCtrl_SetConfig(id, &cfg, reason, sizeof(reason));
  if (ret != ESP_OK) return Route_SpiConfigStatus(ret, ctx, reason);
  Lock_Touch(lockId);
  return HttpServer_SendEmpty(ctx, 204);
}

static esp_err_t Route_SpiPostTransceiveHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  Lock_SweepExpired();
  int id = 0;
  int pr = Route_ParseSpiPathId(HttpServer_GetUri(ctx), "/master/transceive", &id);
  if (pr != 0) return Route_SpiIdError(ctx, pr);
  if (!HttpServer_HasJsonContentType(ctx)) return HttpServer_SendError(ctx, 415, "Content-Type must be application/json.");

  char lockId[64];
  char reason[256];
  int st = HttpServer_LockStatus(ctx, LOCK_KIND_SPI, id, LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return HttpServer_SendError(ctx, st, reason);

  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(ctx, &perr);
  if (body == NULL) return HttpServer_SendError(ctx, 400, "invalid_json");
  cJSON* dataItem = cJSON_GetObjectItem(body, "data");
  if (!cJSON_IsArray(dataItem)) {
    cJSON_Delete(body);
    return HttpServer_SendError(ctx, 400, "data must be an array of integers 0-255.");
  }
  int n = cJSON_GetArraySize(dataItem);
  if (n > CONFIG_SPI_MAX_TRANSFER_BYTES) {
    cJSON_Delete(body);
    snprintf(reason, sizeof(reason), "data is too large. Send at most %u bytes per request.",
             (unsigned)CONFIG_SPI_MAX_TRANSFER_BYTES);
    return HttpServer_SendError(ctx, 400, reason);
  }
  uint8_t* tx = NULL;
  uint8_t* rx = NULL;
  if (n > 0) {
    tx = malloc((size_t)n);
    rx = malloc((size_t)n);
    if (tx == NULL || rx == NULL) {
      free(tx);
      free(rx);
      cJSON_Delete(body);
      return HttpServer_SendError(ctx, 500, "internal");
    }
    for (int i = 0; i < n; i++) {
      cJSON* v = cJSON_GetArrayItem(dataItem, i);
      if (!cJSON_IsNumber(v) || v->valueint < 0 || v->valueint > 255) {
        free(tx);
        free(rx);
        cJSON_Delete(body);
        snprintf(reason, sizeof(reason), "data[%d] must be an integer from 0 to 255.", i);
        return HttpServer_SendError(ctx, 400, reason);
      }
      tx[i] = (uint8_t)v->valueint;
    }
  }
  cJSON_Delete(body);
  size_t rxLen = 0;
  esp_err_t ret = SpiCtrl_MasterTransceive(id, tx, (size_t)n, rx, &rxLen, reason, sizeof(reason));
  if (ret != ESP_OK) {
    free(tx);
    free(rx);
    return Route_SpiCallStatus(ret, ctx, reason);
  }
  cJSON* root = cJSON_CreateObject();
  cJSON* data = cJSON_CreateArray();
  for (size_t i = 0; i < rxLen; i++) cJSON_AddItemToArray(data, cJSON_CreateNumber(rx[i]));
  free(tx);
  free(rx);
  cJSON_AddItemToObject(root, "data", data);
  cJSON_AddNumberToObject(root, "time", (double)HttpServer_NowUs());
  Lock_Touch(lockId);
  return HttpServer_SendJson(ctx, 200, root);
}

static esp_err_t Route_SpiGetSlaveReceiveHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  Lock_SweepExpired();
  int id = 0;
  int pr = Route_ParseSpiPathId(HttpServer_GetUri(ctx), "/slave/receive", &id);
  if (pr != 0) return Route_SpiIdError(ctx, pr);

  char lockId[64];
  char reason[256];
  int st = HttpServer_LockStatus(ctx, LOCK_KIND_SPI, id, LOCK_METHOD_READ, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return HttpServer_SendError(ctx, st, reason);
  esp_err_t ret = SpiCtrl_CheckSlaveReceive(id, reason, sizeof(reason));
  if (ret != ESP_OK) return Route_SpiCallStatus(ret, ctx, reason);

  uint8_t* storage = malloc(CONFIG_SPI_SLAVE_EVENT_BYTES);
  if (storage == NULL) return HttpServer_SendError(ctx, 500, "internal");
  size_t len = SpiCtrl_TakeSlaveEvents(id, storage, CONFIG_SPI_SLAVE_EVENT_BYTES);
  cJSON* root = cJSON_CreateObject();
  cJSON* events = cJSON_CreateArray();
  size_t offset = 0;
  SpiCtrl_Event ev;
  while (SpiCtrl_NextSlaveEvent(storage, len, &offset, &ev)) {
    cJSON* item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "startTime", (double)ev.startUs);
    cJSON_AddNumberToObject(item, "endTime", (double)ev.endUs);
    cJSON* receive = cJSON_CreateArray();
    for (uint16_t i = 0; i < ev.recvLen; i++) cJSON_AddItemToArray(receive, cJSON_CreateNumber(ev.recv[i]));
    cJSON* send = cJSON_CreateArray();
    for (uint16_t i = 0; i < ev.sendLen; i++) cJSON_AddItemToArray(send, cJSON_CreateNumber(ev.send[i]));
    cJSON_AddItemToObject(item, "receive", receive);
    cJSON_AddItemToObject(item, "send", send);
    cJSON_AddItemToArray(events, item);
  }
  free(storage);
  cJSON_AddItemToObject(root, "events", events);
  cJSON_AddNumberToObject(root, "time", (double)HttpServer_NowUs());
  Lock_Touch(lockId);
  return HttpServer_SendJson(ctx, 200, root);
}

static esp_err_t Route_SpiPostSlaveTransmitHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  Lock_SweepExpired();
  int id = 0;
  int pr = Route_ParseSpiPathId(HttpServer_GetUri(ctx), "/slave/transmit", &id);
  if (pr != 0) return Route_SpiIdError(ctx, pr);
  if (!HttpServer_HasJsonContentType(ctx)) return HttpServer_SendError(ctx, 415, "Content-Type must be application/json.");

  char lockId[64];
  char reason[256];
  int st = HttpServer_LockStatus(ctx, LOCK_KIND_SPI, id, LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return HttpServer_SendError(ctx, st, reason);

  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(ctx, &perr);
  if (body == NULL) return HttpServer_SendError(ctx, 400, "invalid_json");
  uint8_t* packed = malloc(CONFIG_SPI_SLAVE_MAP_BYTES);
  if (packed == NULL) {
    cJSON_Delete(body);
    return HttpServer_SendError(ctx, 500, "internal");
  }
  size_t used = 0;
  esp_err_t parse = Route_ParseSlaveMap(body, packed, &used, reason, sizeof(reason));
  cJSON_Delete(body);
  if (parse != ESP_OK) {
    free(packed);
    return Route_SpiCallStatus(parse, ctx, reason);
  }
  esp_err_t ret = SpiCtrl_SetSlaveMap(id, packed, used, reason, sizeof(reason));
  free(packed);
  if (ret != ESP_OK) return Route_SpiCallStatus(ret, ctx, reason);
  Lock_Touch(lockId);
  return HttpServer_SendEmpty(ctx, 204);
}

static const HttpServer_Route uris[] = {
    {.uri = "/spi/", .method = HTTP_SERVER_GET, .handler = Route_SpiListHandler},
    {.uri = "/spi/*/config", .method = HTTP_SERVER_POST, .handler = Route_SpiPostConfigHandler},
    {.uri = "/spi/*/master/transceive", .method = HTTP_SERVER_POST, .handler = Route_SpiPostTransceiveHandler},
    {.uri = "/spi/*/slave/receive", .method = HTTP_SERVER_GET, .handler = Route_SpiGetSlaveReceiveHandler},
    {.uri = "/spi/*/slave/transmit", .method = HTTP_SERVER_POST, .handler = Route_SpiPostSlaveTransmitHandler},
};

esp_err_t Route_SpiRegister(void)
{
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(HttpServer_RegisterRoutes(uris, TOOL_GET_ARRAY_LENGTH(uris)), "register spi uri failed");
  return ESP_OK;
}
