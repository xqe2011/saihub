#include "main.h"

#include "button.h"
#include "buzzer.h"
#include "cloud.h"
#include "gpio_ctrl.h"
#include "http_server.h"
#include "pwm_ctrl.h"
#include "lock.h"
#include "ntp.h"
#include "nvs.h"
#include "script.h"
#include "spi_ctrl.h"
#include "uart_ctrl.h"
#include "wifi.h"

#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <inttypes.h>

static const char* tag = "SAIHub-Main";

void app_main(void)
{
  bool someModuleFailed = false;
  MAIN_LOAD_MODULE(Nvs_Init(), "Nvs");
  MAIN_LOAD_MODULE(Cloud_Init(), "Cloud");
  MAIN_LOAD_MODULE(PwmCtrl_Init(), "Pwm");
  MAIN_LOAD_MODULE(GpioCtrl_Init(), "Gpio");
  MAIN_LOAD_MODULE(Buzzer_Init(), "Buzzer");
  MAIN_LOAD_MODULE(Lock_Init(), "Lock");
  MAIN_LOAD_MODULE(UartCtrl_Init(), "Uart");
  MAIN_LOAD_MODULE(SpiCtrl_Init(), "Spi");
  MAIN_LOAD_MODULE(Script_Init(), "Script");
  MAIN_LOAD_MODULE(Ntp_Init(), "Ntp");
  MAIN_LOAD_MODULE(HttpServer_Init(), "HttpServer");
  MAIN_LOAD_MODULE(Wifi_Init(), "Wifi");
  MAIN_LOAD_MODULE(Button_Init(), "Button");

  if (someModuleFailed) {
    ESP_LOGE(tag, "Some modules failed to initialize");
  }

  TickType_t lastMemoryLog = xTaskGetTickCount();
  while (true) {
    Lock_SweepExpired();
    TickType_t now = xTaskGetTickCount();
    if ((TickType_t)(now - lastMemoryLog) >= pdMS_TO_TICKS(5000)) {
      lastMemoryLog = now;
      ESP_LOGI(tag, "Memory: free=%" PRIu32 " bytes, min=%" PRIu32 " bytes",
               esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
