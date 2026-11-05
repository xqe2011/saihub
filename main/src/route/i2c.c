/**
 * @name I2C routes
 * @file i2c.c
 * @author xqe2011
 */
#include "route.h"

#include "http_server.h"
#include "i2c_ctrl.h"
#include "lock.h"
#include "tool.h"

#include <cJSON.h>
#include <esp_log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* tag = "SAIHub-I2c";

static int Route_ParseI2cPathId(const char* uri, const char* suffix, int* idOut)
{
  const char* prefix = "/i2c/";
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
  if (!I2cCtrl_IsValidId(*idOut)) return -3;
  return 0;
}

static esp_err_t Route_I2cIdError(HttpServer_Context* ctx, int pr)
{
  if (pr == -3) {
    char reason[128];
    const char* p = strstr(HttpServer_GetUri(ctx), "/i2c/");
    long raw = p ? strtol(p + 5, NULL, 10) : -1;
    snprintf(reason, sizeof(reason), "I2C %ld does not exist. Use an id from GET /i2c/.", raw);
    return HttpServer_SendError(ctx, 404, reason);
  }
  return HttpServer_SendError(ctx, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
}

static void Route_AddOptionalPin(cJSON* pins, const char* key, int pin)
{
  if (pin < 0) cJSON_AddNullToObject(pins, key);
  else cJSON_AddNumberToObject(pins, key, pin);
}

static cJSON* Route_I2cConfigToJson(int id, const I2cCtrl_Config* cfg)
{
  cJSON* item = cJSON_CreateObject();
  cJSON_AddNumberToObject(item, "id", id);
  cJSON_AddBoolToObject(item, "enable", cfg->enable);
  cJSON_AddStringToObject(item, "role", I2cCtrl_RoleToString(cfg->role));
  cJSON_AddBoolToObject(item, "pullUp", cfg->pullUp);
  cJSON_AddNumberToObject(item, "addressBits", cfg->addressBits);
  if (cfg->role == I2C_CTRL_ROLE_MASTER) {
    cJSON_AddNumberToObject(item, "clockHz", cfg->clockHz);
    cJSON_AddNumberToObject(item, "sclWaitUs", cfg->sclWaitUs);
  }
  if (cfg->role == I2C_CTRL_ROLE_SLAVE) cJSON_AddNumberToObject(item, "address", cfg->address);
  cJSON* pins = cJSON_CreateObject();
  Route_AddOptionalPin(pins, "sda", cfg->sdaPin);
  Route_AddOptionalPin(pins, "scl", cfg->sclPin);
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

static esp_err_t Route_ParseI2cConfigBody(cJSON* body, I2cCtrl_Config* cfg, char* reason, size_t reasonLen)
{
  cJSON* enableItem = cJSON_GetObjectItem(body, "enable");
  cJSON* roleItem = cJSON_GetObjectItem(body, "role");
  cJSON* clockItem = cJSON_GetObjectItem(body, "clockHz");
  cJSON* addressItem = cJSON_GetObjectItem(body, "address");
  cJSON* addressBitsItem = cJSON_GetObjectItem(body, "addressBits");
  cJSON* pullUpItem = cJSON_GetObjectItem(body, "pullUp");
  cJSON* sclWaitItem = cJSON_GetObjectItem(body, "sclWaitUs");
  cJSON* pinsItem = cJSON_GetObjectItem(body, "pins");
  memset(cfg, 0, sizeof(*cfg));
  if (!cJSON_IsBool(enableItem)) {
    snprintf(reason, reasonLen, "enable must be boolean.");
    return ESP_ERR_INVALID_ARG;
  }
  if (!cJSON_IsString(roleItem) || !I2cCtrl_RoleFromString(roleItem->valuestring, &cfg->role)) {
    snprintf(reason, reasonLen, "role must be one of: master, slave.");
    return ESP_ERR_INVALID_ARG;
  }
  if (!cJSON_IsBool(pullUpItem)) {
    snprintf(reason, reasonLen, "pullUp must be boolean.");
    return ESP_ERR_INVALID_ARG;
  }
  if (!cJSON_IsNumber(addressBitsItem) || (addressBitsItem->valueint != 7 && addressBitsItem->valueint != 10)) {
    snprintf(reason, reasonLen, "addressBits must be 7 or 10.");
    return ESP_ERR_INVALID_ARG;
  }
  cfg->pullUp = cJSON_IsTrue(pullUpItem);
  cfg->addressBits = addressBitsItem->valueint;
  if (cfg->role == I2C_CTRL_ROLE_SLAVE) {
    if (clockItem != NULL) {
      snprintf(reason, reasonLen, "clockHz is not supported when role is slave.");
      return ESP_ERR_INVALID_ARG;
    }
    if (sclWaitItem != NULL) {
      snprintf(reason, reasonLen, "sclWaitUs is not supported when role is slave.");
      return ESP_ERR_INVALID_ARG;
    }
    if (!cJSON_IsNumber(addressItem)) {
      snprintf(reason, reasonLen, "address must be an integer from 0 to %d.", cfg->addressBits == 10 ? 1023 : 127);
      return ESP_ERR_INVALID_ARG;
    }
    cfg->clockHz = 0;
    cfg->sclWaitUs = 0;
    cfg->address = addressItem->valueint;
  } else {
    if (addressItem != NULL) {
      snprintf(reason, reasonLen, "address is not supported when role is master.");
      return ESP_ERR_INVALID_ARG;
    }
    if (!cJSON_IsNumber(clockItem)) {
      snprintf(reason, reasonLen, "clockHz must be an integer from %d to %d.", CONFIG_I2C_CLOCK_HZ_MIN,
               CONFIG_I2C_CLOCK_HZ_MAX);
      return ESP_ERR_INVALID_ARG;
    }
    if (!cJSON_IsNumber(sclWaitItem)) {
      snprintf(reason, reasonLen, "sclWaitUs must be an integer from 0 to %d.", CONFIG_I2C_SCL_WAIT_US_MAX);
      return ESP_ERR_INVALID_ARG;
    }
    cfg->clockHz = clockItem->valueint;
    cfg->sclWaitUs = sclWaitItem->valueint;
    cfg->address = 0;
  }
  if (!cJSON_IsObject(pinsItem)) {
    snprintf(reason, reasonLen, "pins must be an object with sda and scl.");
    return ESP_ERR_INVALID_ARG;
  }
  cfg->enable = cJSON_IsTrue(enableItem);
  if (Route_ParseOptionalPin(pinsItem, "sda", &cfg->sdaPin, reason, reasonLen) != 0) return ESP_ERR_INVALID_ARG;
  if (Route_ParseOptionalPin(pinsItem, "scl", &cfg->sclPin, reason, reasonLen) != 0) return ESP_ERR_INVALID_ARG;
  return ESP_OK;
}

static esp_err_t Route_ParseByteArray(cJSON* item, const char* field, int index, uint8_t* out, size_t* lenOut, char* reason,
                                      size_t reasonLen)
{
  if (!cJSON_IsString(item) || item->valuestring == NULL) {
    snprintf(reason, reasonLen, "mapping[%d].%s must be an even-length hex string.", index, field);
    return ESP_ERR_INVALID_ARG;
  }
  size_t chars = strlen(item->valuestring);
  if ((chars & 1U) != 0) {
    snprintf(reason, reasonLen, "mapping[%d].%s must be an even-length hex string.", index, field);
    return ESP_ERR_INVALID_ARG;
  }
  if (chars / 2 > CONFIG_I2C_MAX_TRANSFER_BYTES) {
    snprintf(reason, reasonLen, "mapping[%d].%s must contain at most %u bytes.", index, field,
             (unsigned)CONFIG_I2C_MAX_TRANSFER_BYTES);
    return ESP_ERR_INVALID_ARG;
  }
  if (!Tool_HexDecode(item->valuestring, out, CONFIG_I2C_MAX_TRANSFER_BYTES, lenOut)) {
    snprintf(reason, reasonLen, "mapping[%d].%s must be an even-length hex string.", index, field);
    return ESP_ERR_INVALID_ARG;
  }
  return ESP_OK;
}

static esp_err_t Route_ParseSlaveMap(cJSON* body, uint8_t* packed, size_t* usedOut, char* reason, size_t reasonLen)
{
  cJSON* mapping = cJSON_GetObjectItem(body, "mapping");
  if (!cJSON_IsArray(mapping)) {
    snprintf(reason, reasonLen, "mapping must be an array.");
    return ESP_ERR_INVALID_ARG;
  }
  size_t used = 0;
  int n = cJSON_GetArraySize(mapping);
  for (int i = 0; i < n; i++) {
    cJSON* entry = cJSON_GetArrayItem(mapping, i);
    if (!cJSON_IsObject(entry)) {
      snprintf(reason, reasonLen, "mapping[%d] must be an object.", i);
      return ESP_ERR_INVALID_ARG;
    }
    cJSON* typeItem = cJSON_GetObjectItem(entry, "type");
    I2cCtrl_MapType type;
    if (!cJSON_IsString(typeItem) || typeItem->valuestring == NULL) {
      snprintf(reason, reasonLen, "mapping[%d].type must be one of: exact, prefix.", i);
      return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(typeItem->valuestring, "exact") == 0) type = I2C_CTRL_MAP_EXACT;
    else if (strcmp(typeItem->valuestring, "prefix") == 0) type = I2C_CTRL_MAP_PREFIX;
    else {
      snprintf(reason, reasonLen, "mapping[%d].type must be one of: exact, prefix.", i);
      return ESP_ERR_INVALID_ARG;
    }
    uint8_t receive[CONFIG_I2C_MAX_TRANSFER_BYTES];
    uint8_t send[CONFIG_I2C_MAX_TRANSFER_BYTES];
    size_t receiveLen = 0;
    size_t sendLen = 0;
    if (Route_ParseByteArray(cJSON_GetObjectItem(entry, "receive"), "receive", i, receive, &receiveLen, reason, reasonLen) !=
        ESP_OK) {
      return ESP_ERR_INVALID_ARG;
    }
    if (Route_ParseByteArray(cJSON_GetObjectItem(entry, "send"), "send", i, send, &sendLen, reason, reasonLen) != ESP_OK) {
      return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = I2cCtrl_PackMapAppend(packed, CONFIG_I2C_SLAVE_MAP_BYTES, &used, i, type, receive, receiveLen, send,
                                          sendLen, reason, reasonLen);
    if (err != ESP_OK) return err;
  }
  *usedOut = used;
  return ESP_OK;
}

static esp_err_t Route_I2cConfigStatus(esp_err_t ret, HttpServer_Context* ctx, const char* reason)
{
  if (ret == ESP_ERR_INVALID_ARG) return HttpServer_SendError(ctx, 400, reason);
  if (ret == ESP_ERR_INVALID_STATE) return HttpServer_SendError(ctx, 409, reason);
  return HttpServer_SendError(ctx, 500, reason[0] ? reason : "internal");
}

static esp_err_t Route_I2cCallStatus(esp_err_t ret, HttpServer_Context* ctx, const char* reason)
{
  if (ret == ESP_ERR_INVALID_ARG || ret == ESP_ERR_INVALID_SIZE) return HttpServer_SendError(ctx, 400, reason);
  if (ret == ESP_ERR_INVALID_STATE || ret == ESP_ERR_TIMEOUT || ret == ESP_ERR_NOT_FOUND ||
      ret == ESP_ERR_INVALID_RESPONSE) {
    return HttpServer_SendError(ctx, 422, reason);
  }
  return HttpServer_SendError(ctx, 500, reason[0] ? reason : "internal");
}

static esp_err_t Route_I2cSendMasterData(HttpServer_Context* ctx, const uint8_t* data, size_t len)
{
  cJSON* root = cJSON_CreateObject();
  char hex[CONFIG_I2C_MAX_TRANSFER_BYTES * 2 + 1];
  if (len > 0 && data == NULL) {
    cJSON_Delete(root);
    return HttpServer_SendError(ctx, 500, "internal");
  }
  Tool_HexEncode(data, len, hex);
  cJSON_AddStringToObject(root, "data", hex);
  cJSON_AddNumberToObject(root, "time", (double)HttpServer_NowUs());
  return HttpServer_SendJson(ctx, 200, root);
}

static esp_err_t Route_I2cSendSlaveEvents(HttpServer_Context* ctx, const uint8_t* storage, size_t len)
{
  esp_err_t ret = HttpServer_SendChunkBegin(ctx, 200, "application/json", NULL);
  if (ret == ESP_OK) ret = HttpServer_SendChunk(ctx, "{\"events\":[", sizeof("{\"events\":[") - 1);
  size_t offset = 0;
  I2cCtrl_Event ev;
  unsigned count = 0;
  char hex[CONFIG_I2C_MAX_TRANSFER_BYTES * 2 + 1];
  char head[80];
  while (ret == ESP_OK && I2cCtrl_NextSlaveEvent(storage, len, &offset, &ev)) {
    int n = snprintf(head, sizeof(head), "%s{\"startTime\":%lld,\"endTime\":%lld,\"receive\":\"",
                     count == 0 ? "" : ",", (long long)ev.startUs, (long long)ev.endUs);
    if (n < 0 || (size_t)n >= sizeof(head)) {
      ret = ESP_ERR_INVALID_SIZE;
      break;
    }
    ret = HttpServer_SendChunk(ctx, head, (size_t)n);
    if (ret == ESP_OK) {
      Tool_HexEncode(ev.recv, ev.recvLen, hex);
      ret = HttpServer_SendChunk(ctx, hex, ev.recvLen * 2);
    }
    if (ret == ESP_OK) ret = HttpServer_SendChunk(ctx, "\",\"send\":\"", sizeof("\",\"send\":\"") - 1);
    if (ret == ESP_OK) {
      Tool_HexEncode(ev.send, ev.sendLen, hex);
      ret = HttpServer_SendChunk(ctx, hex, ev.sendLen * 2);
    }
    if (ret == ESP_OK) ret = HttpServer_SendChunk(ctx, "\"}", 2);
    count++;
  }
  if (ret == ESP_OK) {
    char tail[48];
    int n = snprintf(tail, sizeof(tail), "],\"time\":%lld}", (long long)HttpServer_NowUs());
    if (n < 0 || (size_t)n >= sizeof(tail)) ret = ESP_ERR_INVALID_SIZE;
    else ret = HttpServer_SendChunk(ctx, tail, (size_t)n);
  }
  esp_err_t endRet = HttpServer_SendChunkDone(ctx);
  ESP_LOGI(tag, "Resp rest/%s %s %s -> 200 i2c_events=%u", HttpServer_GetFrom(ctx),
           HttpServer_MethodName(HttpServer_GetMethod(ctx)), HttpServer_GetUri(ctx), count);
  return ret == ESP_OK ? endRet : ret;
}

static esp_err_t Route_I2cListHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  TOOL_CALL_LOG("rest/%s %s %s", HttpServer_GetFrom(ctx), HttpServer_MethodName(HttpServer_GetMethod(ctx)),
                HttpServer_GetUri(ctx));
  Lock_SweepExpired();
  cJSON* root = cJSON_CreateObject();
  cJSON* arr = cJSON_CreateArray();
  int count = I2cCtrl_GetCount();
  for (int i = 0; i < count; i++) {
    I2cCtrl_Config cfg;
    I2cCtrl_GetConfig(i, &cfg);
    cJSON_AddItemToArray(arr, Route_I2cConfigToJson(i, &cfg));
  }
  cJSON_AddItemToObject(root, "i2cs", arr);
  cJSON_AddNumberToObject(root, "time", (double)HttpServer_NowUs());
  return HttpServer_SendJson(ctx, 200, root);
}

static esp_err_t Route_I2cPostConfigHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  Lock_SweepExpired();
  int id = 0;
  int pr = Route_ParseI2cPathId(HttpServer_GetUri(ctx), "/config", &id);
  if (pr != 0) return Route_I2cIdError(ctx, pr);
  if (!HttpServer_HasJsonContentType(ctx)) return HttpServer_SendError(ctx, 415, "Content-Type must be application/json.");

  char lockId[64];
  char reason[256];
  int st = HttpServer_LockStatus(ctx, LOCK_KIND_I2C, id, LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return HttpServer_SendError(ctx, st, reason);

  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(ctx, &perr);
  if (body == NULL) return HttpServer_SendError(ctx, 400, "invalid_json");
  I2cCtrl_Config cfg;
  memset(&cfg, 0, sizeof(cfg));
  if (Route_ParseI2cConfigBody(body, &cfg, reason, sizeof(reason)) != ESP_OK) {
    cJSON_Delete(body);
    return HttpServer_SendError(ctx, 400, reason);
  }
  cJSON_Delete(body);
  esp_err_t ret = I2cCtrl_SetConfig(id, &cfg, reason, sizeof(reason));
  if (ret != ESP_OK) return Route_I2cConfigStatus(ret, ctx, reason);
  Lock_Touch(lockId);
  return HttpServer_SendEmpty(ctx, 204);
}

static esp_err_t Route_I2cPostTransceiveHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  Lock_SweepExpired();
  int id = 0;
  int pr = Route_ParseI2cPathId(HttpServer_GetUri(ctx), "/master/transceive", &id);
  if (pr != 0) return Route_I2cIdError(ctx, pr);
  if (!HttpServer_HasJsonContentType(ctx)) return HttpServer_SendError(ctx, 415, "Content-Type must be application/json.");

  char lockId[64];
  char reason[256];
  int st = HttpServer_LockStatus(ctx, LOCK_KIND_I2C, id, LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return HttpServer_SendError(ctx, st, reason);

  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(ctx, &perr);
  if (body == NULL) return HttpServer_SendError(ctx, 400, "invalid_json");
  cJSON* addressItem = cJSON_GetObjectItem(body, "address");
  cJSON* writeItem = cJSON_GetObjectItem(body, "write");
  cJSON* readLenItem = cJSON_GetObjectItem(body, "readLen");
  cJSON* timeoutItem = cJSON_GetObjectItem(body, "timeoutMs");
  if (!cJSON_IsNumber(addressItem)) {
    cJSON_Delete(body);
    return HttpServer_SendError(ctx, 400, "address must be an integer.");
  }
  if (!cJSON_IsString(writeItem) || writeItem->valuestring == NULL || (strlen(writeItem->valuestring) & 1U) != 0) {
    cJSON_Delete(body);
    return HttpServer_SendError(ctx, 400, "write must be an even-length hex string.");
  }
  if (!cJSON_IsNumber(readLenItem) || readLenItem->valueint < 0 ||
      readLenItem->valueint > (int)CONFIG_I2C_MAX_TRANSFER_BYTES) {
    cJSON_Delete(body);
    snprintf(reason, sizeof(reason), "readLen must be an integer from 0 to %u.", (unsigned)CONFIG_I2C_MAX_TRANSFER_BYTES);
    return HttpServer_SendError(ctx, 400, reason);
  }
  int timeoutMs = -1;
  if (timeoutItem != NULL) {
    if (!cJSON_IsNumber(timeoutItem)) {
      cJSON_Delete(body);
      snprintf(reason, sizeof(reason), "timeoutMs must be an integer from %d to %d.", CONFIG_I2C_TRANSFER_TIMEOUT_MS_MIN,
               CONFIG_I2C_TRANSFER_TIMEOUT_MS_MAX);
      return HttpServer_SendError(ctx, 400, reason);
    }
    timeoutMs = timeoutItem->valueint;
  }
  int address = addressItem->valueint;
  size_t n = strlen(writeItem->valuestring) / 2;
  if (n > CONFIG_I2C_MAX_TRANSFER_BYTES) {
    cJSON_Delete(body);
    snprintf(reason, sizeof(reason), "write is too large. Send at most %u bytes per request.",
             (unsigned)CONFIG_I2C_MAX_TRANSFER_BYTES);
    return HttpServer_SendError(ctx, 400, reason);
  }
  size_t readLen = (size_t)readLenItem->valueint;
  uint8_t* tx = NULL;
  uint8_t* rx = NULL;
  if (n > 0) {
    tx = malloc(n);
    if (tx == NULL) {
      cJSON_Delete(body);
      return HttpServer_SendError(ctx, 500, "internal");
    }
    size_t decoded = 0;
    if (!Tool_HexDecode(writeItem->valuestring, tx, n, &decoded)) {
      free(tx);
      cJSON_Delete(body);
      return HttpServer_SendError(ctx, 400, "write must be an even-length hex string.");
    }
  }
  if (readLen > 0) {
    rx = malloc(readLen);
    if (rx == NULL) {
      free(tx);
      cJSON_Delete(body);
      return HttpServer_SendError(ctx, 500, "internal");
    }
  }
  cJSON_Delete(body);
  size_t rxLen = 0;
  esp_err_t ret = I2cCtrl_MasterTransceive(id, address, tx, n, rx, readLen, timeoutMs, &rxLen, reason, sizeof(reason));
  free(tx);
  if (ret != ESP_OK) {
    free(rx);
    return Route_I2cCallStatus(ret, ctx, reason);
  }
  if (rxLen == 0) {
    free(rx);
    rx = NULL;
  }
  Lock_Touch(lockId);
  esp_err_t sent = Route_I2cSendMasterData(ctx, rx, rxLen);
  free(rx);
  return sent;
}

static esp_err_t Route_I2cGetSlaveReceiveHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  Lock_SweepExpired();
  int id = 0;
  int pr = Route_ParseI2cPathId(HttpServer_GetUri(ctx), "/slave/receive", &id);
  if (pr != 0) return Route_I2cIdError(ctx, pr);

  char lockId[64];
  char reason[256];
  int st = HttpServer_LockStatus(ctx, LOCK_KIND_I2C, id, LOCK_METHOD_READ, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return HttpServer_SendError(ctx, st, reason);
  esp_err_t ret = I2cCtrl_CheckSlaveReceive(id, reason, sizeof(reason));
  if (ret != ESP_OK) return Route_I2cCallStatus(ret, ctx, reason);

  uint8_t* storage = malloc(CONFIG_I2C_SLAVE_EVENT_BYTES);
  if (storage == NULL) return HttpServer_SendError(ctx, 500, "internal");
  size_t len = I2cCtrl_TakeSlaveEvents(id, storage, CONFIG_I2C_SLAVE_EVENT_BYTES);
  Lock_Touch(lockId);
  esp_err_t sent = Route_I2cSendSlaveEvents(ctx, storage, len);
  free(storage);
  return sent;
}

static esp_err_t Route_I2cPostSlaveTransmitHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  Lock_SweepExpired();
  int id = 0;
  int pr = Route_ParseI2cPathId(HttpServer_GetUri(ctx), "/slave/transmit", &id);
  if (pr != 0) return Route_I2cIdError(ctx, pr);
  if (!HttpServer_HasJsonContentType(ctx)) return HttpServer_SendError(ctx, 415, "Content-Type must be application/json.");

  char lockId[64];
  char reason[256];
  int st = HttpServer_LockStatus(ctx, LOCK_KIND_I2C, id, LOCK_METHOD_WRITE, lockId, sizeof(lockId), reason, sizeof(reason));
  if (st) return HttpServer_SendError(ctx, st, reason);

  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(ctx, &perr);
  if (body == NULL) return HttpServer_SendError(ctx, 400, "invalid_json");
  uint8_t* packed = malloc(CONFIG_I2C_SLAVE_MAP_BYTES);
  if (packed == NULL) {
    cJSON_Delete(body);
    return HttpServer_SendError(ctx, 500, "internal");
  }
  size_t used = 0;
  esp_err_t parse = Route_ParseSlaveMap(body, packed, &used, reason, sizeof(reason));
  cJSON_Delete(body);
  if (parse != ESP_OK) {
    free(packed);
    return Route_I2cCallStatus(parse, ctx, reason);
  }
  esp_err_t ret = I2cCtrl_SetSlaveMap(id, packed, used, reason, sizeof(reason));
  free(packed);
  if (ret != ESP_OK) return Route_I2cCallStatus(ret, ctx, reason);
  Lock_Touch(lockId);
  return HttpServer_SendEmpty(ctx, 204);
}

static const HttpServer_Route uris[] = {
    {.uri = "/i2c/", .method = HTTP_SERVER_GET, .handler = Route_I2cListHandler},
    {.uri = "/i2c/*/config", .method = HTTP_SERVER_POST, .handler = Route_I2cPostConfigHandler},
    {.uri = "/i2c/*/master/transceive", .method = HTTP_SERVER_POST, .handler = Route_I2cPostTransceiveHandler},
    {.uri = "/i2c/*/slave/receive", .method = HTTP_SERVER_GET, .handler = Route_I2cGetSlaveReceiveHandler},
    {.uri = "/i2c/*/slave/transmit", .method = HTTP_SERVER_POST, .handler = Route_I2cPostSlaveTransmitHandler},
};

esp_err_t Route_I2cRegister(void)
{
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(HttpServer_RegisterRoutes(uris, TOOL_GET_ARRAY_LENGTH(uris)), "register i2c uri failed");
  return ESP_OK;
}
