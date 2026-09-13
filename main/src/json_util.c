#include "json_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

int64_t Json_NowUs(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
}

esp_err_t Json_SendError(httpd_req_t* req, int status, const char* reason)
{
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
  /* httpd expects full status phrase; use numeric + reason-ish */
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

esp_err_t Json_SendJson(httpd_req_t* req, int status, cJSON* root)
{
  char* printed = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (printed == NULL) {
    return Json_SendError(req, 500, "internal");
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

esp_err_t Json_SendEmpty(httpd_req_t* req, int status)
{
  if (status == 204) {
    httpd_resp_set_status(req, "204 No Content");
  } else {
    httpd_resp_set_status(req, "200 OK");
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, NULL, 0);
}

esp_err_t Json_ReadBody(httpd_req_t* req, char** outBuf, size_t* outLen)
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

cJSON* Json_ParseBody(httpd_req_t* req, esp_err_t* errOut)
{
  char* buf = NULL;
  size_t len = 0;
  esp_err_t ret = Json_ReadBody(req, &buf, &len);
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
