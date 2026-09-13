#include "ntp.h"

#include "config.h"
#include "tool.h"

#include <esp_log.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/time.h>
#include <time.h>

static const char* tag = "SAIHUB-Ntp";
static bool isSynced = false;

bool Ntp_IsSynced(void)
{
  return isSynced;
}

static void Ntp_TimeSyncNotification(struct timeval* tv)
{
  (void)tv;
  isSynced = true;
  ESP_LOGI(tag, "Time synchronized");
}

esp_err_t Ntp_Init(void)
{
  /* SNTP APIs need the tcpip stack (esp_netif_init). Configure in Ntp_SyncAndWait. */
  isSynced = false;
  return ESP_OK;
}

esp_err_t Ntp_SyncAndWait(void)
{
  isSynced = false;
  /* Must run after Wifi_Init / esp_netif_init — setoperatingmode uses tcpip_callback. */
  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, CONFIG_NTP_SERVER);
  esp_sntp_set_time_sync_notification_cb(Ntp_TimeSyncNotification);
  if (esp_sntp_enabled()) {
    esp_sntp_restart();
  } else {
    esp_sntp_init();
  }

  int64_t startMs = (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
  while (!isSynced) {
    int64_t nowMs = (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (nowMs - startMs > CONFIG_NTP_WAIT_TIMEOUT_MS) {
      ESP_LOGE(tag, "NTP sync timeout");
      return ESP_ERR_TIMEOUT;
    }
    /* Also accept if wall clock looks sane (year >= 2024) */
    time_t now = 0;
    time(&now);
    struct tm tmNow = {0};
    gmtime_r(&now, &tmNow);
    if (tmNow.tm_year + 1900 >= 2024) {
      isSynced = true;
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  ESP_LOGI(tag, "NTP ready");
  return ESP_OK;
}
