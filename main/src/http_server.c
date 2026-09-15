/**
 * @name HTTP server module
 * @file http_server.c
 * @author xqe2011
 */
#include "http_server.h"

#include "config.h"
#include "gpio_ctrl.h"
#include "ntp.h"
#include "route.h"
#include "tool.h"
#include "wifi.h"

#include <esp_http_server.h>
#include <esp_log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static const char* tag = "SAIHUB-Http";
static httpd_handle_t server = NULL;
static bool pairingServer = false;

int64_t HttpServer_NowUs(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
}

const char* HttpServer_MethodName(int method)
{
  switch (method) {
    case HTTP_GET:
      return "GET";
    case HTTP_POST:
      return "POST";
    case HTTP_PUT:
      return "PUT";
    case HTTP_DELETE:
      return "DELETE";
    case HTTP_PATCH:
      return "PATCH";
    case HTTP_OPTIONS:
      return "OPTIONS";
    default:
      return "?";
  }
}

void HttpServer_LogCall(httpd_req_t* req)
{
  char lockId[64] = {0};
  httpd_req_get_hdr_value_str(req, "X-Lock-Id", lockId, sizeof(lockId));
  if (lockId[0] != '\0') {
    ESP_LOGI(tag, "Call %s %s content_len=%d X-Lock-Id=%s", HttpServer_MethodName(req->method), req->uri, req->content_len,
             lockId);
  } else {
    ESP_LOGI(tag, "Call %s %s content_len=%d", HttpServer_MethodName(req->method), req->uri, req->content_len);
  }
}

void HttpServer_SetCors(httpd_req_t* req)
{
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, PATCH, OPTIONS");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Headers",
                     "Content-Type, Accept, X-Lock-Id, MCP-Protocol-Version, Mcp-Session-Id");
  httpd_resp_set_hdr(req, "Access-Control-Expose-Headers", "X-Lock-Id, Mcp-Session-Id, MCP-Protocol-Version");
  httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
}

esp_err_t HttpServer_SendOptions(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  HttpServer_SetCors(req);
  httpd_resp_set_status(req, "204 No Content");
  return httpd_resp_send(req, NULL, 0);
}

esp_err_t HttpServer_SendError(httpd_req_t* req, int status, const char* reason)
{
  ESP_LOGW(tag, "Resp %s %s -> %d %s", HttpServer_MethodName(req->method), req->uri, status, reason ? reason : "");
  HttpServer_SetCors(req);
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "reason", reason ? reason : "internal");
  char* printed = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (printed == NULL) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"reason\":\"internal\"}", HTTPD_RESP_USE_STRLEN);
  }
  char statusStr[64];
  snprintf(statusStr, sizeof(statusStr), "%d ", status);
  switch (status) {
    case 400:
      httpd_resp_set_status(req, "400 Bad Request");
      break;
    case 404:
      httpd_resp_set_status(req, "404 Not Found");
      break;
    case 405:
      httpd_resp_set_status(req, "405 Method Not Allowed");
      break;
    case 409:
      httpd_resp_set_status(req, "409 Conflict");
      break;
    case 412:
      httpd_resp_set_status(req, "412 Precondition Failed");
      break;
    case 415:
      httpd_resp_set_status(req, "415 Unsupported Media Type");
      break;
    case 422:
      httpd_resp_set_status(req, "422 Unprocessable Entity");
      break;
    case 423:
      httpd_resp_set_status(req, "423 Locked");
      break;
    case 500:
      httpd_resp_set_status(req, "500 Internal Server Error");
      break;
    default:
      httpd_resp_set_status(req, statusStr);
      break;
  }
  httpd_resp_set_type(req, "application/json");
  esp_err_t ret = httpd_resp_send(req, printed, strlen(printed));
  free(printed);
  return ret;
}

esp_err_t HttpServer_SendJson(httpd_req_t* req, int status, cJSON* root)
{
  ESP_LOGI(tag, "Resp %s %s -> %d", HttpServer_MethodName(req->method), req->uri, status);
  HttpServer_SetCors(req);
  char* printed = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (printed == NULL) {
    return HttpServer_SendError(req, 500, "internal");
  }
  switch (status) {
    case 200:
      httpd_resp_set_status(req, "200 OK");
      break;
    case 201:
      httpd_resp_set_status(req, "201 Created");
      break;
    default:
      httpd_resp_set_status(req, "200 OK");
      break;
  }
  httpd_resp_set_type(req, "application/json");
  esp_err_t ret = httpd_resp_send(req, printed, strlen(printed));
  free(printed);
  return ret;
}

esp_err_t HttpServer_SendEmpty(httpd_req_t* req, int status)
{
  ESP_LOGI(tag, "Resp %s %s -> %d", HttpServer_MethodName(req->method), req->uri, status);
  HttpServer_SetCors(req);
  if (status == 204) {
    httpd_resp_set_status(req, "204 No Content");
  } else {
    httpd_resp_set_status(req, "200 OK");
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, NULL, 0);
}

bool HttpServer_HasJsonContentType(httpd_req_t* req)
{
  char type[64] = {0};
  if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK) {
    return false;
  }
  return strstr(type, "application/json") != NULL;
}

void HttpServer_GetLockHeader(httpd_req_t* req, char* out, size_t outLen)
{
  out[0] = '\0';
  httpd_req_get_hdr_value_str(req, "X-Lock-Id", out, outLen);
}

void HttpServer_FormatPinRange(char* out, size_t outLen)
{
  int maxPin = GpioCtrl_GetLogicalCount() - 1;
  if (maxPin < 0) maxPin = 0;
  snprintf(out, outLen, "0 to %d", maxPin);
}

int HttpServer_LockStatusId(const char* lockId, Lock_Kind kind, int pin, uint8_t methods, char* reasonOut,
                            size_t reasonLen)
{
  const char* hdr = (lockId && lockId[0]) ? lockId : NULL;
  esp_err_t ret = Lock_CheckAccess(kind, pin, methods, hdr);
  if (ret == ESP_ERR_NOT_FOUND) {
    snprintf(reasonOut, reasonLen,
             "X-Lock-Id is missing or is not a known lock. Send a current lock id from POST /lock, or omit the "
             "header only if the resource is unlocked.");
    return 412;
  }
  if (ret == ESP_ERR_INVALID_STATE) {
    const char* methodName = (methods & LOCK_METHOD_WRITE) ? "write" : "read";
    if (kind == LOCK_KIND_GPIO) {
      snprintf(reasonOut, reasonLen,
               "Pin %d %s is locked by another lock. Send header X-Lock-Id with the holding lock's id, DELETE that "
               "lock, or wait until it expires.",
               pin, methodName);
    } else {
      snprintf(reasonOut, reasonLen,
               "Power %s %s is locked by another lock. Send header X-Lock-Id with the holding lock's id, DELETE that "
               "lock, or wait until it expires.",
               Lock_KindToString(kind), methodName);
    }
    return 423;
  }
  return 0;
}

int HttpServer_LockStatus(httpd_req_t* req, Lock_Kind kind, int pin, uint8_t methods, char* lockIdBuf, size_t lockIdLen,
                    char* reasonOut, size_t reasonLen)
{
  HttpServer_GetLockHeader(req, lockIdBuf, lockIdLen);
  return HttpServer_LockStatusId(lockIdBuf, kind, pin, methods, reasonOut, reasonLen);
}

esp_err_t HttpServer_ParsePinConfigBody(cJSON* body, GpioCtrl_Mode* modeOut, bool* openDrainOut, bool* pullUpOut,
                                        bool* pullDownOut, char* reason, size_t reasonLen)
{
  static const char* modeHint = "disable, digitalInput, digitalOutput, digitalInputOutput, pwmOutput";
  cJSON* modeItem = cJSON_GetObjectItem(body, "mode");
  cJSON* pullUpItem = cJSON_GetObjectItem(body, "pullUp");
  cJSON* pullDownItem = cJSON_GetObjectItem(body, "pullDown");
  cJSON* openDrainItem = cJSON_GetObjectItem(body, "openDrain");
  GpioCtrl_Mode mode;
  if (!cJSON_IsString(modeItem) || !GpioCtrl_ModeFromString(modeItem->valuestring, &mode)) {
    snprintf(reason, reasonLen, "mode is invalid. Use one of: %s.", modeHint);
    return ESP_ERR_INVALID_ARG;
  }
  if (!cJSON_IsBool(pullUpItem) || !cJSON_IsBool(pullDownItem)) {
    snprintf(reason, reasonLen, "pullUp and pullDown must be boolean.");
    return ESP_ERR_INVALID_ARG;
  }
  bool openDrain = false;
  if (openDrainItem != NULL) {
    if (!cJSON_IsBool(openDrainItem)) {
      snprintf(reason, reasonLen, "openDrain must be boolean.");
      return ESP_ERR_INVALID_ARG;
    }
    openDrain = cJSON_IsTrue(openDrainItem);
  }
  if (openDrain && (mode == GPIO_CTRL_MODE_DISABLE || mode == GPIO_CTRL_MODE_DIGITAL_INPUT)) {
    snprintf(reason, reasonLen, "openDrain cannot be true when mode is disable or digitalInput.");
    return ESP_ERR_INVALID_ARG;
  }
  *modeOut = mode;
  *openDrainOut = openDrain;
  *pullUpOut = cJSON_IsTrue(pullUpItem);
  *pullDownOut = cJSON_IsTrue(pullDownItem);
  return ESP_OK;
}

int HttpServer_ParsePathPin(const char* uri, const char* prefix, const char* suffix, int* pinOut)
{
  size_t prefixLen = strlen(prefix);
  if (strncmp(uri, prefix, prefixLen) != 0) return -1;
  const char* p = uri + prefixLen;
  if (*p < '0' || *p > '9') return -2;
  char* end = NULL;
  long pin = strtol(p, &end, 10);
  if (end == p) return -2;
  if (suffix) {
    size_t sl = strlen(suffix);
    if (strncmp(end, suffix, sl) != 0) return -2;
    if (end[sl] != '\0' && end[sl] != '?') return -2;
  } else if (*end != '\0' && *end != '?') {
    return -2;
  }
  *pinOut = (int)pin;
  if (!GpioCtrl_IsValidLogicalPin(*pinOut)) return -3;
  return 0;
}

esp_err_t HttpServer_ParsePinsArray(cJSON* root, int* pins, size_t maxPins, size_t* countOut, char* reason, size_t reasonLen)
{
  char range[32];
  HttpServer_FormatPinRange(range, sizeof(range));
  cJSON* arr = cJSON_GetObjectItem(root, "pins");
  if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) == 0) {
    snprintf(reason, reasonLen, "pins must be a non-empty array of integers from %s.", range);
    return ESP_ERR_INVALID_ARG;
  }
  size_t n = (size_t)cJSON_GetArraySize(arr);
  if (n > maxPins) {
    snprintf(reason, reasonLen, "pins array is too long.");
    return ESP_ERR_INVALID_ARG;
  }
  for (size_t i = 0; i < n; i++) {
    cJSON* item = cJSON_GetArrayItem(arr, (int)i);
    if (!cJSON_IsNumber(item)) {
      snprintf(reason, reasonLen, "pins[%u] must be an integer from %s.", (unsigned)i, range);
      return ESP_ERR_INVALID_ARG;
    }
    int pin = item->valueint;
    if (!GpioCtrl_IsValidLogicalPin(pin)) {
      snprintf(reason, reasonLen, "pins[%u] (%d) is invalid. Use a pin from %s.", (unsigned)i, pin, range);
      return ESP_ERR_INVALID_ARG;
    }
    pins[i] = pin;
  }
  *countOut = n;
  return ESP_OK;
}

static esp_err_t HttpServer_ParseLockMethods(cJSON* methods, uint8_t* bitsOut, char* reason, size_t reasonLen)
{
  if (!cJSON_IsArray(methods) || cJSON_GetArraySize(methods) == 0) {
    snprintf(reason, reasonLen, "Each resource.method entry must be read or write.");
    return ESP_ERR_INVALID_ARG;
  }
  uint8_t bits = 0;
  for (int m = 0; m < cJSON_GetArraySize(methods); m++) {
    cJSON* mv = cJSON_GetArrayItem(methods, m);
    if (!cJSON_IsString(mv)) {
      snprintf(reason, reasonLen, "Each resource.method entry must be read or write.");
      return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(mv->valuestring, "read") == 0)
      bits |= LOCK_METHOD_READ;
    else if (strcmp(mv->valuestring, "write") == 0)
      bits |= LOCK_METHOD_WRITE;
    else {
      snprintf(reason, reasonLen, "method is invalid. Each resource.method entry must be read or write.");
      return ESP_ERR_INVALID_ARG;
    }
  }
  *bitsOut = bits;
  return ESP_OK;
}

static cJSON* HttpServer_MethodsToJson(uint8_t methods)
{
  cJSON* methodsArr = cJSON_CreateArray();
  if (methods & LOCK_METHOD_READ) cJSON_AddItemToArray(methodsArr, cJSON_CreateString("read"));
  if (methods & LOCK_METHOD_WRITE) cJSON_AddItemToArray(methodsArr, cJSON_CreateString("write"));
  return methodsArr;
}

esp_err_t HttpServer_ParseLockResources(cJSON* resourcesArr, Lock_Resource* out, size_t maxOut, size_t* countOut,
                                        char* reason, size_t reasonLen)
{
  if (!cJSON_IsArray(resourcesArr) || cJSON_GetArraySize(resourcesArr) == 0) {
    snprintf(reason, reasonLen, "resources must be a non-empty array.");
    return ESP_ERR_INVALID_ARG;
  }
  char range[32];
  HttpServer_FormatPinRange(range, sizeof(range));
  size_t count = 0;
  int groupCount = cJSON_GetArraySize(resourcesArr);
  for (int g = 0; g < groupCount; g++) {
    cJSON* item = cJSON_GetArrayItem(resourcesArr, g);
    if (!cJSON_IsObject(item)) {
      snprintf(reason, reasonLen, "resources[%d] must be an object.", g);
      return ESP_ERR_INVALID_ARG;
    }
    cJSON* typeItem = cJSON_GetObjectItem(item, "type");
    cJSON* pinsItem = cJSON_GetObjectItem(item, "pins");
    cJSON* railsItem = cJSON_GetObjectItem(item, "rails");
    cJSON* methodsItem = cJSON_GetObjectItem(item, "method");
    if (!cJSON_IsString(typeItem) || typeItem->valuestring == NULL) {
      snprintf(reason, reasonLen, "resources[%d].type must be pin or power.", g);
      return ESP_ERR_INVALID_ARG;
    }
    uint8_t bits = 0;
    if (HttpServer_ParseLockMethods(methodsItem, &bits, reason, reasonLen) != ESP_OK) return ESP_ERR_INVALID_ARG;

    if (strcmp(typeItem->valuestring, "pin") == 0) {
      if (railsItem != NULL) {
        snprintf(reason, reasonLen, "resources[%d] with type pin must not include rails.", g);
        return ESP_ERR_INVALID_ARG;
      }
      if (!cJSON_IsArray(pinsItem) || cJSON_GetArraySize(pinsItem) == 0) {
        snprintf(reason, reasonLen, "resources[%d].pins must be a non-empty array of integers from %s.", g, range);
        return ESP_ERR_INVALID_ARG;
      }
      int pinN = cJSON_GetArraySize(pinsItem);
      for (int p = 0; p < pinN; p++) {
        if (count >= maxOut) {
          snprintf(reason, reasonLen, "resources expand to too many lock entries (max %u).", (unsigned)maxOut);
          return ESP_ERR_INVALID_ARG;
        }
        cJSON* pinVal = cJSON_GetArrayItem(pinsItem, p);
        if (!cJSON_IsNumber(pinVal) || !GpioCtrl_IsValidLogicalPin(pinVal->valueint)) {
          snprintf(reason, reasonLen, "resources[%d].pins[%d] is invalid. Use a pin from %s.", g, p, range);
          return ESP_ERR_INVALID_ARG;
        }
        out[count].kind = LOCK_KIND_GPIO;
        out[count].pin = pinVal->valueint;
        out[count].methods = bits;
        count++;
      }
    } else if (strcmp(typeItem->valuestring, "power") == 0) {
      if (pinsItem != NULL) {
        snprintf(reason, reasonLen, "resources[%d] with type power must not include pins.", g);
        return ESP_ERR_INVALID_ARG;
      }
      if (!cJSON_IsArray(railsItem) || cJSON_GetArraySize(railsItem) == 0) {
        snprintf(reason, reasonLen, "resources[%d].rails must be a non-empty array of 3v3 or 5v.", g);
        return ESP_ERR_INVALID_ARG;
      }
      int railN = cJSON_GetArraySize(railsItem);
      for (int r = 0; r < railN; r++) {
        if (count >= maxOut) {
          snprintf(reason, reasonLen, "resources expand to too many lock entries (max %u).", (unsigned)maxOut);
          return ESP_ERR_INVALID_ARG;
        }
        cJSON* railVal = cJSON_GetArrayItem(railsItem, r);
        Lock_Kind kind;
        if (!cJSON_IsString(railVal) || !Lock_PowerFromString(railVal->valuestring, &kind)) {
          snprintf(reason, reasonLen, "resources[%d].rails[%d] is invalid. Use one of: 3v3, 5v.", g, r);
          return ESP_ERR_INVALID_ARG;
        }
        out[count].kind = kind;
        out[count].pin = 0;
        out[count].methods = bits;
        count++;
      }
    } else {
      snprintf(reason, reasonLen, "resources[%d].type must be pin or power.", g);
      return ESP_ERR_INVALID_ARG;
    }
  }
  *countOut = count;
  return ESP_OK;
}

cJSON* HttpServer_SerializeLockResources(const Lock_Resource* resources, size_t count)
{
  cJSON* resArr = cJSON_CreateArray();
  if (resArr == NULL || resources == NULL || count == 0) return resArr;
  bool used[CONFIG_LOCK_MAX_RESOURCES];
  memset(used, 0, sizeof(used));
  if (count > CONFIG_LOCK_MAX_RESOURCES) count = CONFIG_LOCK_MAX_RESOURCES;

  for (size_t i = 0; i < count; i++) {
    if (used[i]) continue;
    uint8_t methods = resources[i].methods;
    cJSON* r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "method", HttpServer_MethodsToJson(methods));

    if (resources[i].kind == LOCK_KIND_GPIO) {
      cJSON_AddStringToObject(r, "type", "pin");
      cJSON* pins = cJSON_CreateArray();
      for (size_t j = i; j < count; j++) {
        if (used[j]) continue;
        if (resources[j].kind != LOCK_KIND_GPIO || resources[j].methods != methods) continue;
        cJSON_AddItemToArray(pins, cJSON_CreateNumber(resources[j].pin));
        used[j] = true;
      }
      cJSON_AddItemToObject(r, "pins", pins);
    } else {
      cJSON_AddStringToObject(r, "type", "power");
      cJSON* rails = cJSON_CreateArray();
      for (size_t j = i; j < count; j++) {
        if (used[j]) continue;
        if (resources[j].kind == LOCK_KIND_GPIO || resources[j].methods != methods) continue;
        cJSON_AddItemToArray(rails, cJSON_CreateString(Lock_KindToString(resources[j].kind)));
        used[j] = true;
      }
      cJSON_AddItemToObject(r, "rails", rails);
    }
    cJSON_AddItemToArray(resArr, r);
  }
  return resArr;
}

esp_err_t HttpServer_ReadBody(httpd_req_t* req, char** outBuf, size_t* outLen)
{
  int total = req->content_len;
  if (total < 0) total = 0;
  if (total > 16 * 1024) {
    return ESP_ERR_INVALID_SIZE;
  }
  char* buf = calloc(1, (size_t)total + 1);
  if (buf == NULL) return ESP_ERR_NO_MEM;
  int received = 0;
  while (received < total) {
    int r = httpd_req_recv(req, buf + received, total - received);
    if (r <= 0) {
      free(buf);
      return ESP_FAIL;
    }
    received += r;
  }
  *outBuf = buf;
  *outLen = (size_t)received;
  return ESP_OK;
}

cJSON* HttpServer_ParseBody(httpd_req_t* req, esp_err_t* errOut)
{
  char* buf = NULL;
  size_t len = 0;
  esp_err_t ret = HttpServer_ReadBody(req, &buf, &len);
  if (ret != ESP_OK) {
    if (errOut) *errOut = ret;
    return NULL;
  }
  if (len == 0) {
    free(buf);
    if (errOut) *errOut = ESP_ERR_INVALID_ARG;
    return NULL;
  }
  /* Skip /mcp: JSON-RPC envelope is logged at tools/call instead. */
  if (strcmp(req->uri, "/mcp") != 0) {
    int n = (int)(len < 512 ? len : 512);
    TOOL_CALL_LOG("rest %s %s args=%.*s", HttpServer_MethodName(req->method), req->uri, n, buf);
  }
  cJSON* root = cJSON_Parse(buf);
  free(buf);
  if (root == NULL) {
    if (errOut) *errOut = ESP_ERR_INVALID_ARG;
    return NULL;
  }
  if (errOut) *errOut = ESP_OK;
  return root;
}

static esp_err_t HttpServer_NotFoundHandler(httpd_req_t* req, httpd_err_code_t err)
{
  (void)err;
  HttpServer_LogCall(req);
  if (pairingServer) {
    return HttpServer_SendError(req, 404, "This URL does not exist. Open /wifi/page for Wi-Fi setup.");
  }
  return HttpServer_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
}

static void HttpServer_OnNtpSynced(void)
{
  if (!Wifi_IsConnected() || Wifi_IsPairing()) return;
  if (HttpServer_Start() != ESP_OK) {
    ESP_LOGE(tag, "HTTP start failed");
  }
}

static void HttpServer_OnWifiDisconnected(void)
{
  if (Wifi_IsPairing()) {
    ESP_LOGW(tag, "WiFi disconnected during pairing; keeping portal up");
    return;
  }
  ESP_LOGW(tag, "WiFi disconnected; stopping HTTP");
  HttpServer_Stop();
}

static void HttpServer_OnPairingStarted(void)
{
  ESP_LOGI(tag, "Pairing started; switching to portal HTTP");
  HttpServer_Stop();
  if (HttpServer_StartPairing() != ESP_OK) {
    ESP_LOGE(tag, "Portal HTTP start failed");
  }
}

static void HttpServer_OnPairingStopped(void)
{
  ESP_LOGI(tag, "Pairing stopped; closing portal HTTP");
  HttpServer_Stop();

  char ip[16] = {0};
  char reason[96] = {0};
  Wifi_PairState state = Wifi_GetPairStatus(ip, sizeof(ip), reason, sizeof(reason));
  /* Success path fires connected callback next (NTP then API). Cancel-while-online restores API. */
  if (Wifi_IsConnected() && state != WIFI_PAIR_STATE_CONNECTED) {
    ESP_LOGI(tag, "Restoring API HTTP after pairing cancel");
    if (HttpServer_Start() != ESP_OK) {
      ESP_LOGE(tag, "HTTP start failed after pairing cancel");
    }
  }
}

esp_err_t HttpServer_Init(void)
{
  TOOL_CHECK_ESP_OK_OR_RETURN(Ntp_RegisterSyncedCallback(HttpServer_OnNtpSynced));
  TOOL_CHECK_ESP_OK_OR_RETURN(Wifi_RegisterDisconnectedCallback(HttpServer_OnWifiDisconnected));
  TOOL_CHECK_ESP_OK_OR_RETURN(Wifi_RegisterPairingStartedCallback(HttpServer_OnPairingStarted));
  TOOL_CHECK_ESP_OK_OR_RETURN(Wifi_RegisterPairingStoppedCallback(HttpServer_OnPairingStopped));
  return ESP_OK;
}

esp_err_t HttpServer_Stop(void)
{
  if (server) {
    httpd_stop(server);
    server = NULL;
    pairingServer = false;
    ESP_LOGI(tag, "HTTP server stopped");
  }
  return ESP_OK;
}

/* URI matcher for templates like "/pin/<id>/pwm" written with a mid-path '*'.
 * Stock httpd_uri_match_wildcard only treats a trailing star; a mid-path star is
 * literal, so those handlers never match and trailing "/pin/" + star (PUT) yields
 * 405 on GET/POST. Here: mid-path '*' = one non-empty path segment; trailing '*' =
 * remainder.
 */
static bool HttpServer_UriMatch(const char* tpl, const char* uri, size_t match_upto)
{
  const char* t = tpl;
  const char* u = uri;
  const char* u_end = uri + match_upto;

  while (*t) {
    if (*t == '*') {
      t++;
      if (*t == '\0') {
        return true;
      }
      if (u >= u_end || *u == '/') {
        return false;
      }
      while (u < u_end && *u != '/') {
        u++;
      }
      continue;
    }
    if (u >= u_end || *u != *t) {
      return false;
    }
    t++;
    u++;
  }
  return u == u_end;
}

static esp_err_t HttpServer_StartWithConfig(bool pairing)
{
  if (server) {
    if (pairingServer == pairing) return ESP_OK;
    HttpServer_Stop();
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.uri_match_fn = HttpServer_UriMatch;
  config.max_uri_handlers = 48;
  config.lru_purge_enable = true;
  config.recv_wait_timeout = 65;
  config.send_wait_timeout = 65;
  if (pairing) config.max_open_sockets = 12;

  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(httpd_start(&server, &config), "httpd_start failed");
  pairingServer = pairing;

  if (pairing) {
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(Route_PortalRegister(server), "portal routes failed");
  } else {
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(Route_ControlRegister(server), "control ui route failed");
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(Route_OpenApiRegister(server), "openapi routes failed");
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(Route_PinRegister(server), "pin routes failed");
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(Route_LockRegister(server), "lock routes failed");
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(Route_PowerRegister(server), "power routes failed");
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(Route_ScriptRegister(server), "script routes failed");
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(Route_McpRegister(server), "mcp routes failed");
  }

  static const httpd_uri_t optionsUri = {.uri = "/*", .method = HTTP_OPTIONS, .handler = HttpServer_SendOptions};
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(httpd_register_uri_handler(server, &optionsUri), "options cors route failed");
  httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, HttpServer_NotFoundHandler);
  ESP_LOGI(tag, "HTTP server started on port 80 (%s)", pairing ? "pairing" : "api");
  return ESP_OK;
}

esp_err_t HttpServer_Start(void)
{
  return HttpServer_StartWithConfig(false);
}

esp_err_t HttpServer_StartPairing(void)
{
  return HttpServer_StartWithConfig(true);
}
