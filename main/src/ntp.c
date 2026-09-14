#include "ntp.h"

#include "config.h"
#include "tool.h"
#include "wifi.h"

#include <esp_log.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/time.h>
#include <time.h>

static const char* tag = "SAIHUB-Ntp";
static bool isSynced = false;
static volatile bool wantSync = false;
static TaskHandle_t syncTask = NULL;
static Ntp_SyncedCallback syncedCallbacks[4];

bool Ntp_IsSynced(void)
{
  if (isSynced) return true;
  time_t now = 0;
  time(&now);
  struct tm tmNow = {0};
  gmtime_r(&now, &tmNow);
  if (tmNow.tm_year + 1900 >= 2024) {
    isSynced = true;
  }
  return isSynced;
}

esp_err_t Ntp_RegisterSyncedCallback(Ntp_SyncedCallback callback)
{
  TOOL_REGISTER_CALLBACK(syncedCallbacks, callback, "synced");
}

static void Ntp_TimeSyncNotification(struct timeval* tv)
{
  (void)tv;
  isSynced = true;
  ESP_LOGI(tag, "Time synchronized");
}

static void Ntp_StartClient(void)
{
  isSynced = false;
  /* Must run after Wifi_Init / esp_netif_init — setoperatingmode uses tcpip_callback.
   * Mode/server may only be set while the client is stopped (reconnect after pairing). */
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, CONFIG_NTP_SERVER);
  esp_sntp_set_time_sync_notification_cb(Ntp_TimeSyncNotification);
  esp_sntp_init();
}

static void Ntp_SyncTask(void* arg)
{
  (void)arg;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!wantSync) continue;

    Ntp_StartClient();
    int elapsedMs = 0;
    while (wantSync) {
      if (Ntp_IsSynced()) {
        ESP_LOGI(tag, "NTP ready");
        if (wantSync) TOOL_EXECUTE_CALLBACKS(syncedCallbacks);
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      elapsedMs += 200;
      if (elapsedMs > CONFIG_NTP_TIMEOUT_MS) {
        ESP_LOGW(tag, "NTP sync timeout; retrying");
        Ntp_StartClient();
        elapsedMs = 0;
      }
    }
  }
}

static void Ntp_OnWifiConnected(void)
{
  if (Wifi_IsPairing()) {
    ESP_LOGI(tag, "WiFi connected while pairing; NTP waits until AP closes");
    return;
  }
  ESP_LOGI(tag, "WiFi connected; starting NTP");
  if (Ntp_Start() != ESP_OK) {
    ESP_LOGE(tag, "NTP start failed");
  }
}

static void Ntp_OnWifiDisconnected(void)
{
  Ntp_Stop();
}

static void Ntp_OnPairingStarted(void)
{
  Ntp_Stop();
}

esp_err_t Ntp_Init(void)
{
  isSynced = false;
  wantSync = false;
  BaseType_t ok = xTaskCreate(Ntp_SyncTask, "ntp", 4096, NULL, 5, &syncTask);
  TOOL_CHECK_OR_LOG_RETURN(ok != pdPASS, "ntp task create failed");
  TOOL_CHECK_ESP_OK_OR_RETURN(Wifi_RegisterConnectedCallback(Ntp_OnWifiConnected));
  TOOL_CHECK_ESP_OK_OR_RETURN(Wifi_RegisterDisconnectedCallback(Ntp_OnWifiDisconnected));
  TOOL_CHECK_ESP_OK_OR_RETURN(Wifi_RegisterPairingStartedCallback(Ntp_OnPairingStarted));
  return ESP_OK;
}

esp_err_t Ntp_Start(void)
{
  if (syncTask == NULL) return ESP_FAIL;
  wantSync = true;
  xTaskNotifyGive(syncTask);
  return ESP_OK;
}

void Ntp_Stop(void)
{
  wantSync = false;
}
