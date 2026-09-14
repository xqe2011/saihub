#include "main.h"

#include "gpio_ctrl.h"
#include "http_server.h"
#include "lock.h"
#include "ntp.h"
#include "nvs.h"
#include "script.h"
#include "wifi.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* tag = "SAIHUB-Main";

static void Main_OnWifiDisconnected(void)
{
  ESP_LOGW(tag, "WiFi disconnected; stopping HTTP");
  HttpServer_Stop();
}

static void Main_OnWifiConnected(void)
{
  ESP_LOGI(tag, "WiFi connected; syncing NTP before HTTP");
  if (Ntp_SyncAndWait() != ESP_OK) {
    ESP_LOGE(tag, "NTP sync failed; HTTP not started");
    return;
  }
  if (HttpServer_Start() != ESP_OK) {
    ESP_LOGE(tag, "HTTP start failed");
  }
}

void app_main(void)
{
  bool someModuleFailed = false;
  MAIN_LOAD_MODULE(Nvs_Init(), "Nvs");
  MAIN_LOAD_MODULE(GpioCtrl_Init(), "Gpio");
  MAIN_LOAD_MODULE(Lock_Init(), "Lock");
  MAIN_LOAD_MODULE(Script_Init(), "Script");
  MAIN_LOAD_MODULE(Ntp_Init(), "Ntp");
  MAIN_LOAD_MODULE(HttpServer_Init(), "HttpServer");
  MAIN_LOAD_MODULE(Wifi_RegisterConnectedCallback(Main_OnWifiConnected), "WifiConnectedCb");
  MAIN_LOAD_MODULE(Wifi_RegisterDisconnectedCallback(Main_OnWifiDisconnected), "WifiDisconnectedCb");
  MAIN_LOAD_MODULE(Wifi_Init(), "Wifi");

  if (someModuleFailed) {
    ESP_LOGE(tag, "Some modules failed to initialize");
  }

  while (true) {
    Lock_SweepExpired();
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
