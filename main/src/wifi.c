/**
 * @name WIFI module
 * @file wifi.c
 * @author xqe2011
 */
#include "wifi.h"

#include "config.h"
#include "nvs.h"
#include "tool.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <string.h>

static const char* tag = "SAIHUB-Wifi";

static bool allowReconnect = true;
static bool isConnected = false;
static char lastRequestConnectSSID[33] = {0};
static char lastRequestConnectPassword[65] = {0};
static Wifi_ConnectedCallback connectedCallbacks[4];
static Wifi_DisconnectedCallback disconnectedCallbacks[4];
static TimerHandle_t reconnectTimer;

bool Wifi_IsConnected(void)
{
  return isConnected;
}

esp_err_t Wifi_RegisterConnectedCallback(Wifi_ConnectedCallback callback)
{
  TOOL_REGISTER_CALLBACK(connectedCallbacks, callback, "connected");
}

esp_err_t Wifi_RegisterDisconnectedCallback(Wifi_DisconnectedCallback callback)
{
  TOOL_REGISTER_CALLBACK(disconnectedCallbacks, callback, "disconnected");
}

esp_err_t Wifi_ConnectWifi(const char* ssid, const char* password)
{
  if (ssid == NULL || password == NULL || strlen(ssid) == 0 || strlen(ssid) > 32 || strlen(password) > 64) {
    ESP_LOGE(tag, "SSID or password invalid length");
    return ESP_FAIL;
  }

  wifi_config_t config = {0};
  memcpy(config.sta.ssid, ssid, strlen(ssid));
  memcpy(config.sta.password, password, strlen(password));
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(esp_wifi_set_config(WIFI_IF_STA, &config), "set wifi config failed");

  strncpy(lastRequestConnectSSID, ssid, sizeof(lastRequestConnectSSID) - 1);
  strncpy(lastRequestConnectPassword, password, sizeof(lastRequestConnectPassword) - 1);
  allowReconnect = true;

  esp_wifi_disconnect();
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(esp_wifi_connect(), "wifi connect request failed");
  ESP_LOGI(tag, "Connecting to SSID: %s", ssid);
  return ESP_OK;
}

static void Wifi_ReconnectCallback(TimerHandle_t timer)
{
  (void)timer;
  if (allowReconnect) {
    esp_wifi_connect();
  }
}

static void Wifi_EventHandler(void* arg, esp_event_base_t eventBase, int32_t eventId, void* eventData)
{
  (void)arg;
  (void)eventData;

  if (eventBase == WIFI_EVENT) {
    if (eventId == WIFI_EVENT_STA_START) {
      char ssid[33] = {0};
      char password[65] = {0};
      if (Nvs_GetString("wifi.ssid", ssid, sizeof(ssid)) == ESP_OK &&
          Nvs_GetString("wifi.password", password, sizeof(password)) == ESP_OK) {
        Wifi_ConnectWifi(ssid, password);
      } else if (strcmp(CONFIG_WIFI_SSID, "CHANGE_ME") != 0) {
        Wifi_ConnectWifi(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD);
      } else {
        ESP_LOGW(tag, "No WiFi credentials in NVS or config.h");
      }
    } else if (eventId == WIFI_EVENT_STA_DISCONNECTED) {
      bool wasConnected = isConnected;
      isConnected = false;
      if (wasConnected) {
        TOOL_EXECUTE_CALLBACKS(disconnectedCallbacks);
      }
      if (allowReconnect) {
        xTimerStop(reconnectTimer, portMAX_DELAY);
        xTimerStart(reconnectTimer, portMAX_DELAY);
      }
    }
  }

  if (eventBase == IP_EVENT && eventId == IP_EVENT_STA_GOT_IP) {
    Nvs_SetString("wifi.ssid", lastRequestConnectSSID);
    Nvs_SetString("wifi.password", lastRequestConnectPassword);
    isConnected = true;
    ESP_LOGI(tag, "WiFi connected");
    TOOL_EXECUTE_CALLBACKS(connectedCallbacks);
  }
}

esp_err_t Wifi_Init(void)
{
  reconnectTimer = xTimerCreate("wifi-reconn", pdMS_TO_TICKS(CONFIG_WIFI_RECONNECT_INTERVAL_MS), pdFALSE, NULL,
                                  Wifi_ReconnectCallback);
  TOOL_CHECK_OR_LOG_RETURN(reconnectTimer == NULL, "create reconnect timer failed");

  TOOL_CHECK_ESP_OK_OR_RETURN(esp_netif_init());
  TOOL_CHECK_ESP_OK_OR_RETURN(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  TOOL_CHECK_ESP_OK_OR_RETURN(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &Wifi_EventHandler, NULL));
  TOOL_CHECK_ESP_OK_OR_RETURN(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &Wifi_EventHandler, NULL));

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  TOOL_CHECK_ESP_OK_OR_RETURN(esp_wifi_init(&cfg));
  TOOL_CHECK_ESP_OK_OR_RETURN(esp_wifi_set_mode(WIFI_MODE_STA));
  TOOL_CHECK_ESP_OK_OR_RETURN(esp_wifi_start());
  return ESP_OK;
}
