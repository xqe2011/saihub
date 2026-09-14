#include "main.h"

#include "button.h"
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
  if (Wifi_IsPairing()) {
    ESP_LOGW(tag, "WiFi disconnected during pairing; keeping portal up");
    return;
  }
  ESP_LOGW(tag, "WiFi disconnected; stopping HTTP");
  HttpServer_Stop();
}

static void Main_OnWifiConnected(void)
{
  if (Wifi_IsPairing()) {
    ESP_LOGI(tag, "WiFi connected while pairing; portal remains until AP closes");
    return;
  }
  ESP_LOGI(tag, "WiFi connected; syncing NTP before HTTP");
  if (Ntp_SyncAndWait() != ESP_OK) {
    ESP_LOGE(tag, "NTP sync failed; HTTP not started");
    return;
  }
  if (HttpServer_Start() != ESP_OK) {
    ESP_LOGE(tag, "HTTP start failed");
  }
}

static void Main_OnPairingStarted(void)
{
  ESP_LOGI(tag, "Pairing started; switching to portal HTTP");
  HttpServer_Stop();
  if (HttpServer_StartPairing() != ESP_OK) {
    ESP_LOGE(tag, "Portal HTTP start failed");
  }
}

static void Main_OnPairingStopped(void)
{
  ESP_LOGI(tag, "Pairing stopped; closing portal HTTP");
  HttpServer_Stop();

  char ip[16] = {0};
  char reason[96] = {0};
  Wifi_PairState state = Wifi_GetPairStatus(ip, sizeof(ip), reason, sizeof(reason));
  /* Success path fires connected callback next (NTP + API). Cancel-while-online restores API. */
  if (Wifi_IsConnected() && state != WIFI_PAIR_STATE_CONNECTED) {
    ESP_LOGI(tag, "Restoring API HTTP after pairing cancel");
    if (HttpServer_Start() != ESP_OK) {
      ESP_LOGE(tag, "HTTP start failed after pairing cancel");
    }
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
  MAIN_LOAD_MODULE(Wifi_RegisterPairingStartedCallback(Main_OnPairingStarted), "WifiPairingStartedCb");
  MAIN_LOAD_MODULE(Wifi_RegisterPairingStoppedCallback(Main_OnPairingStopped), "WifiPairingStoppedCb");
  MAIN_LOAD_MODULE(Wifi_Init(), "Wifi");
  MAIN_LOAD_MODULE(Button_Init(), "Button");

  if (someModuleFailed) {
    ESP_LOGE(tag, "Some modules failed to initialize");
  }

  while (true) {
    Lock_SweepExpired();
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
