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

void app_main(void)
{
  bool someModuleFailed = false;
  MAIN_LOAD_MODULE(Nvs_Init(), "Nvs");
  MAIN_LOAD_MODULE(GpioCtrl_Init(), "Gpio");
  MAIN_LOAD_MODULE(Lock_Init(), "Lock");
  MAIN_LOAD_MODULE(Script_Init(), "Script");
  MAIN_LOAD_MODULE(Ntp_Init(), "Ntp");
  MAIN_LOAD_MODULE(HttpServer_Init(), "HttpServer");
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
