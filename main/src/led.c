/**
 * @name Status indicator
 * @file led.c
 * @author xqe2011
 */
#include "led.h"

#include "cloud.h"
#include "config.h"
#include "tool.h"
#include "wifi.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <stdbool.h>
#include <stddef.h>

static const char* tag = "SAIHub-Led";

typedef enum {
  LED_MODE_OFF = 0,
  LED_MODE_PAIR,
  LED_MODE_CONNECTING,
  LED_MODE_WIFI,
  LED_MODE_CLOUD,
} Led_Mode;

typedef struct {
  bool on;
  int ms;
} Led_Edge;

static const Led_Edge pairEdges[] = {
    {true, CONFIG_LED_SLOW_MS},
    {false, CONFIG_LED_SLOW_MS},
};
static const Led_Edge connectingEdges[] = {
    {true, CONFIG_LED_FAST_MS},
    {false, CONFIG_LED_FAST_MS},
    {true, CONFIG_LED_FAST_MS},
    {false, CONFIG_LED_FAST_MS},
    {true, CONFIG_LED_FAST_MS},
    {false, CONFIG_LED_PAUSE_MS},
};
static const Led_Edge wifiEdges[] = {
    {true, CONFIG_LED_FAST_MS},
    {false, CONFIG_LED_FAST_MS},
    {true, CONFIG_LED_FAST_MS},
    {false, CONFIG_LED_PAUSE_MS},
};
static const Led_Edge cloudEdges[] = {
    {true, CONFIG_LED_FAST_MS},
    {false, CONFIG_LED_PAUSE_MS},
};

static esp_timer_handle_t ledTimer;
static Led_Mode latchedMode = LED_MODE_OFF;
static size_t edgeIndex = 0;

static Led_Mode Led_CurrentMode(void)
{
  if (Wifi_IsPairing()) return LED_MODE_PAIR;
  if (!Wifi_IsConnected()) return Wifi_HasCredentials() ? LED_MODE_CONNECTING : LED_MODE_OFF;
  Cloud_Status status;
  Cloud_GetStatus(&status);
  return status.connected ? LED_MODE_CLOUD : LED_MODE_WIFI;
}

static void Led_Timer(void* arg)
{
  (void)arg;
  Led_Mode mode = Led_CurrentMode();
  if (mode != latchedMode) {
    latchedMode = mode;
    edgeIndex = 0;
  }
  size_t count = 0;
  const Led_Edge* edges = NULL;
  switch (mode) {
    case LED_MODE_PAIR: count = 2; edges = pairEdges; break;
    case LED_MODE_CONNECTING: count = 6; edges = connectingEdges; break;
    case LED_MODE_WIFI: count = 4; edges = wifiEdges; break;
    case LED_MODE_CLOUD: count = 2; edges = cloudEdges; break;
    default: break;
  }
  if (edges == NULL) {
    gpio_set_level(CONFIG_LED_PIN, 0);
    return;
  }
  const Led_Edge* edge = &edges[edgeIndex];
  gpio_set_level(CONFIG_LED_PIN, edge->on);
  edgeIndex = (edgeIndex + 1) % count;
  esp_timer_start_once(ledTimer, (uint64_t)edge->ms * 1000ULL);
}

static void Led_OnState(void)
{
  if (ledTimer == NULL || Led_CurrentMode() == latchedMode) return;
  esp_timer_stop(ledTimer);
  esp_timer_start_once(ledTimer, 1000);
}

esp_err_t Led_Init(void)
{
  gpio_config_t io = {
      .pin_bit_mask = 1ULL << CONFIG_LED_PIN,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_set_level(CONFIG_LED_PIN, 0);
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(gpio_config(&io), "led gpio config failed");
  gpio_set_level(CONFIG_LED_PIN, 0);

  esp_timer_create_args_t args = {
      .callback = Led_Timer,
      .name = "led",
  };
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(esp_timer_create(&args, &ledTimer), "led timer create failed");
  TOOL_CHECK_ESP_OK_OR_RETURN(Wifi_RegisterConnectedCallback(Led_OnState));
  TOOL_CHECK_ESP_OK_OR_RETURN(Wifi_RegisterDisconnectedCallback(Led_OnState));
  TOOL_CHECK_ESP_OK_OR_RETURN(Wifi_RegisterConnectingCallback(Led_OnState));
  TOOL_CHECK_ESP_OK_OR_RETURN(Wifi_RegisterPairingStartedCallback(Led_OnState));
  TOOL_CHECK_ESP_OK_OR_RETURN(Wifi_RegisterPairingStoppedCallback(Led_OnState));
  TOOL_CHECK_ESP_OK_OR_RETURN(Cloud_RegisterConnectedCallback(Led_OnState));
  TOOL_CHECK_ESP_OK_OR_RETURN(Cloud_RegisterDisconnectedCallback(Led_OnState));
  Led_OnState();
  ESP_LOGI(tag, "status indicator ready pin=%d", CONFIG_LED_PIN);
  return ESP_OK;
}
