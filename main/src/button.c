/**
 * @name Pairing button module
 * @file button.c
 * @author xqe2011
 */
#include "button.h"

#include "config.h"
#include "tool.h"
#include "wifi.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* tag = "SAIHUB-Button";

#define BUTTON_DEBOUNCE_US 50000
#define BUTTON_POLL_MS 20

static int64_t lastChangeUs = 0;
static int stableLevel = 1;
static int lastRawLevel = 1;

static void Button_Task(void* arg)
{
  (void)arg;
  while (true) {
    int raw = gpio_get_level(CONFIG_GPIO_BUTTON_PIN);
    int64_t now = esp_timer_get_time();
    if (raw != lastRawLevel) {
      lastRawLevel = raw;
      lastChangeUs = now;
    } else if (raw != stableLevel && (now - lastChangeUs) >= BUTTON_DEBOUNCE_US) {
      stableLevel = raw;
      if (stableLevel == 0) {
        if (Wifi_IsPairing()) {
          ESP_LOGI(tag, "Button: stop pairing");
          Wifi_StopPairing();
        } else {
          ESP_LOGI(tag, "Button: start pairing");
          Wifi_StartPairing();
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
  }
}

esp_err_t Button_Init(void)
{
  gpio_config_t io = {
      .pin_bit_mask = 1ULL << CONFIG_GPIO_BUTTON_PIN,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(gpio_config(&io), "button gpio config failed");
  stableLevel = gpio_get_level(CONFIG_GPIO_BUTTON_PIN);
  lastRawLevel = stableLevel;
  lastChangeUs = esp_timer_get_time();

  BaseType_t ok = xTaskCreate(Button_Task, "btn", 3072, NULL, 5, NULL);
  TOOL_CHECK_OR_LOG_RETURN(ok != pdPASS, "button task create failed");
  return ESP_OK;
}
