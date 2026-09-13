/**
 * @name HTTP server module
 * @file http_server.c
 * @author xqe2011
 */
#include "http_server.h"

#include "gpio_ctrl.h"
#include "route.h"
#include "tool.h"

#include <esp_http_server.h>
#include <esp_log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static const char* tag = "SAIHUB-Http";
static httpd_handle_t server = NULL;

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

esp_err_t HttpServer_SendError(httpd_req_t* req, int status, const char* reason)
{
  ESP_LOGW(tag, "Resp %s %s -> %d %s", HttpServer_MethodName(req->method), req->uri, status, reason ? reason : "");
  cJSON* root = cJSON_CreateObject();
  cJSON* err = cJSON_CreateObject();
  cJSON_AddStringToObject(err, "reason", reason ? reason : "internal");
  cJSON_AddItemToObject(root, "error", err);
  char* printed = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (printed == NULL) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"error\":{\"reason\":\"internal\"}}", HTTPD_RESP_USE_STRLEN);
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

int HttpServer_LockStatus(httpd_req_t* req, Lock_Kind kind, int pin, uint8_t methods, char* lockIdBuf, size_t lockIdLen,
                    char* reasonOut, size_t reasonLen)
{
  HttpServer_GetLockHeader(req, lockIdBuf, lockIdLen);
  const char* hdr = lockIdBuf[0] ? lockIdBuf : NULL;
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
  return HttpServer_SendError(req, 404, "This URL does not exist. Read GET /openapi.json for the available paths.");
}

esp_err_t HttpServer_Init(void)
{
  return ESP_OK;
}

esp_err_t HttpServer_Stop(void)
{
  if (server) {
    httpd_stop(server);
    server = NULL;
    ESP_LOGI(tag, "HTTP server stopped");
  }
  return ESP_OK;
}

esp_err_t HttpServer_Start(void)
{
  if (server) {
    return ESP_OK;
  }
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.max_uri_handlers = 32;
  config.lru_purge_enable = true;
  config.recv_wait_timeout = 65;
  config.send_wait_timeout = 65;

  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(httpd_start(&server, &config), "httpd_start failed");
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(Route_OpenApiRegister(server), "openapi routes failed");
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(Route_PinRegister(server), "pin routes failed");
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(Route_LockRegister(server), "lock routes failed");
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(Route_PowerRegister(server), "power routes failed");
  httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, HttpServer_NotFoundHandler);
  ESP_LOGI(tag, "HTTP server started on port 80");
  return ESP_OK;
}
