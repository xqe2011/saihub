/**
 * @name Pairing portal routes
 * @file portal.c
 * @author xqe2011
 */
#include "route.h"

#include "http_server.h"
#include "tool.h"
#include "wifi.h"

#include <cJSON.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <string.h>

static const char* tag = "SAIHUB-Portal";

/* EMBED_FILES "embed/portal.html.gz" → portal_html_gz (IDF uses the basename) */
extern const uint8_t portal_html_gz_start[] asm("_binary_portal_html_gz_start");
extern const uint8_t portal_html_gz_end[] asm("_binary_portal_html_gz_end");

static const char* Portal_StateString(Wifi_PairState state)
{
  switch (state) {
    case WIFI_PAIR_STATE_CONNECTING:
      return "connecting";
    case WIFI_PAIR_STATE_CONNECTED:
      return "connected";
    case WIFI_PAIR_STATE_FAILED:
      return "failed";
    case WIFI_PAIR_STATE_IDLE:
    default:
      return "idle";
  }
}

static esp_err_t Portal_GetIndexHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  HttpServer_SetCors(req);
  size_t len = (size_t)(portal_html_gz_end - portal_html_gz_start);
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, (const char*)portal_html_gz_start, len);
}

esp_err_t Route_PortalSendRedirect(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  HttpServer_SetCors(req);
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", CONFIG_WIFI_PORTAL_URL);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, "Redirecting", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t Portal_GetNetworksHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  Wifi_Network networks[24];
  size_t count = 0;
  esp_err_t err = Wifi_ScanNetworks(networks, TOOL_GET_ARRAY_LENGTH(networks), &count);
  if (err != ESP_OK) {
    return HttpServer_SendError(req, 500, "Could not scan for networks. Try again.");
  }

  cJSON* root = cJSON_CreateObject();
  cJSON* arr = cJSON_CreateArray();
  for (size_t i = 0; i < count; i++) {
    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "ssid", networks[i].ssid);
    cJSON_AddNumberToObject(item, "rssi", networks[i].rssi);
    cJSON_AddBoolToObject(item, "secure", networks[i].secure);
    cJSON_AddItemToArray(arr, item);
  }
  cJSON_AddItemToObject(root, "networks", arr);
  return HttpServer_SendJson(req, 200, root);
}

static esp_err_t Portal_PostConnectHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  if (!HttpServer_HasJsonContentType(req)) {
    return HttpServer_SendError(req, 415, "Content-Type must be application/json.");
  }

  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(req, &perr);
  if (body == NULL) {
    return HttpServer_SendError(req, 400, "Request body must be JSON with ssid and password.");
  }

  cJSON* ssidItem = cJSON_GetObjectItem(body, "ssid");
  cJSON* passwordItem = cJSON_GetObjectItem(body, "password");
  if (!cJSON_IsString(ssidItem) || ssidItem->valuestring == NULL || ssidItem->valuestring[0] == '\0') {
    cJSON_Delete(body);
    return HttpServer_SendError(req, 400, "ssid is required.");
  }
  const char* password = "";
  if (passwordItem != NULL) {
    if (!cJSON_IsString(passwordItem) || passwordItem->valuestring == NULL) {
      cJSON_Delete(body);
      return HttpServer_SendError(req, 400, "password must be a string.");
    }
    password = passwordItem->valuestring;
  }

  esp_err_t err = Wifi_ConnectWifiAsync(ssidItem->valuestring, password);
  cJSON_Delete(body);
  if (err == ESP_ERR_INVALID_ARG) {
    return HttpServer_SendError(req, 400, "SSID or password is invalid.");
  }
  if (err == ESP_ERR_INVALID_STATE) {
    return HttpServer_SendError(req, 409, "Pairing mode is not active.");
  }
  if (err != ESP_OK) {
    return HttpServer_SendError(req, 500, "Could not start connection.");
  }

  cJSON* root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", true);
  return HttpServer_SendJson(req, 200, root);
}

static esp_err_t Portal_GetStatusHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  char ip[16] = {0};
  char reason[96] = {0};
  Wifi_PairState state = Wifi_GetPairStatus(ip, sizeof(ip), reason, sizeof(reason));

  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "state", Portal_StateString(state));
  if (reason[0] != '\0') cJSON_AddStringToObject(root, "reason", reason);
  if (state == WIFI_PAIR_STATE_CONNECTED && ip[0] != '\0') cJSON_AddStringToObject(root, "ip", ip);
  return HttpServer_SendJson(req, 200, root);
}

static const httpd_uri_t portalUris[] = {
    {.uri = "/wifi/page", .method = HTTP_GET, .handler = Portal_GetIndexHandler},
    {.uri = "/wifi/networks", .method = HTTP_GET, .handler = Portal_GetNetworksHandler},
    {.uri = "/wifi/connect", .method = HTTP_POST, .handler = Portal_PostConnectHandler},
    {.uri = "/wifi/status", .method = HTTP_GET, .handler = Portal_GetStatusHandler},
    {.uri = "/*", .method = HTTP_GET, .handler = Route_PortalSendRedirect},
};

esp_err_t Route_PortalRegister(httpd_handle_t server)
{
  for (size_t i = 0; i < TOOL_GET_ARRAY_LENGTH(portalUris); i++) {
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(httpd_register_uri_handler(server, &portalUris[i]), "register portal uri failed");
  }
  ESP_LOGI(tag, "Portal routes registered");
  return ESP_OK;
}
