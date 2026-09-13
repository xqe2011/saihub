/**
 * @name GPIO control module
 * @file gpio_ctrl.c
 * @author xqe2011
 */
#include "gpio_ctrl.h"

#include "config.h"
#include "tool.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>
#include <sys/time.h>

static const char* tag = "SAIHUB-Gpio";

typedef struct {
  GpioCtrl_Mode mode;
  bool pullUp;
  bool pullDown;
  bool interruptActive;
} GpioPinRuntime;

typedef struct {
  int logicalPin;
  int64_t timeUs;
  int level;
  bool raising;
} TraceIsrEvent;

static int logicalToHw[] = CONFIG_GPIO_LOGICAL_TO_HW;
static GpioPinRuntime pins[TOOL_GET_ARRAY_LENGTH(logicalToHw)];
static bool powerEnable3v3 = false;
static bool powerEnable5v = false;
static QueueHandle_t traceQueue;
static volatile bool traceActive = false;
static GpioCtrl_Edge traceEdge = GPIO_CTRL_EDGE_BOTH;

bool GpioCtrl_IsValidLogicalPin(int pin)
{
  return pin >= 0 && pin < TOOL_GET_ARRAY_LENGTH(logicalToHw);
}

int GpioCtrl_GetLogicalCount(void)
{
  return TOOL_GET_ARRAY_LENGTH(logicalToHw);
}

static int GpioCtrl_Hw(int logicalPin)
{
  return logicalToHw[logicalPin];
}

static int GpioCtrl_PowerHw(GpioCtrl_PowerRail rail)
{
  if (rail == GPIO_CTRL_POWER_3V3) return CONFIG_GPIO_POWER_3V3_PIN;
  return CONFIG_GPIO_POWER_5V_PIN;
}

const char* GpioCtrl_ModeToString(GpioCtrl_Mode mode)
{
  switch (mode) {
    case GPIO_CTRL_MODE_DISABLE:
      return "disable";
    case GPIO_CTRL_MODE_INPUT:
      return "input";
    case GPIO_CTRL_MODE_OUTPUT:
      return "output";
    case GPIO_CTRL_MODE_OUTPUT_OPEN_DRAIN:
      return "output_open_drain";
    case GPIO_CTRL_MODE_INPUT_OUTPUT:
      return "input_output";
    case GPIO_CTRL_MODE_INPUT_OUTPUT_OPEN_DRAIN:
      return "input_output_open_drain";
    default:
      return "disable";
  }
}

bool GpioCtrl_ModeFromString(const char* s, GpioCtrl_Mode* out)
{
  if (s == NULL || out == NULL) return false;
  if (strcmp(s, "disable") == 0) {
    *out = GPIO_CTRL_MODE_DISABLE;
    return true;
  }
  if (strcmp(s, "input") == 0) {
    *out = GPIO_CTRL_MODE_INPUT;
    return true;
  }
  if (strcmp(s, "output") == 0) {
    *out = GPIO_CTRL_MODE_OUTPUT;
    return true;
  }
  if (strcmp(s, "output_open_drain") == 0) {
    *out = GPIO_CTRL_MODE_OUTPUT_OPEN_DRAIN;
    return true;
  }
  if (strcmp(s, "input_output") == 0) {
    *out = GPIO_CTRL_MODE_INPUT_OUTPUT;
    return true;
  }
  if (strcmp(s, "input_output_open_drain") == 0) {
    *out = GPIO_CTRL_MODE_INPUT_OUTPUT_OPEN_DRAIN;
    return true;
  }
  return false;
}

bool GpioCtrl_EdgeFromString(const char* s, GpioCtrl_Edge* out)
{
  if (s == NULL || out == NULL) return false;
  if (strcmp(s, "raising") == 0) {
    *out = GPIO_CTRL_EDGE_RAISING;
    return true;
  }
  if (strcmp(s, "falling") == 0) {
    *out = GPIO_CTRL_EDGE_FALLING;
    return true;
  }
  if (strcmp(s, "both") == 0) {
    *out = GPIO_CTRL_EDGE_BOTH;
    return true;
  }
  return false;
}

const char* GpioCtrl_EdgeToString(GpioCtrl_Edge edge)
{
  switch (edge) {
    case GPIO_CTRL_EDGE_RAISING:
      return "raising";
    case GPIO_CTRL_EDGE_FALLING:
      return "falling";
    case GPIO_CTRL_EDGE_BOTH:
      return "both";
    default:
      return "both";
  }
}

bool GpioCtrl_PowerRailFromString(const char* s, GpioCtrl_PowerRail* out)
{
  if (s == NULL || out == NULL) return false;
  if (strcmp(s, "3v3") == 0) {
    *out = GPIO_CTRL_POWER_3V3;
    return true;
  }
  if (strcmp(s, "5v") == 0) {
    *out = GPIO_CTRL_POWER_5V;
    return true;
  }
  return false;
}

const char* GpioCtrl_PowerRailToString(GpioCtrl_PowerRail rail)
{
  switch (rail) {
    case GPIO_CTRL_POWER_3V3:
      return "3v3";
    case GPIO_CTRL_POWER_5V:
      return "5v";
    default:
      return "3v3";
  }
}

static gpio_mode_t GpioCtrl_ToEspMode(GpioCtrl_Mode mode)
{
  switch (mode) {
    case GPIO_CTRL_MODE_DISABLE:
      return GPIO_MODE_DISABLE;
    case GPIO_CTRL_MODE_INPUT:
      return GPIO_MODE_INPUT;
    case GPIO_CTRL_MODE_OUTPUT:
      return GPIO_MODE_OUTPUT;
    case GPIO_CTRL_MODE_OUTPUT_OPEN_DRAIN:
      return GPIO_MODE_OUTPUT_OD;
    case GPIO_CTRL_MODE_INPUT_OUTPUT:
      return GPIO_MODE_INPUT_OUTPUT;
    case GPIO_CTRL_MODE_INPUT_OUTPUT_OPEN_DRAIN:
      return GPIO_MODE_INPUT_OUTPUT_OD;
    default:
      return GPIO_MODE_DISABLE;
  }
}

bool GpioCtrl_IsOutputCapable(int logicalPin)
{
  if (!GpioCtrl_IsValidLogicalPin(logicalPin)) return false;
  GpioCtrl_Mode mode = pins[logicalPin].mode;
  return mode == GPIO_CTRL_MODE_OUTPUT || mode == GPIO_CTRL_MODE_OUTPUT_OPEN_DRAIN ||
         mode == GPIO_CTRL_MODE_INPUT_OUTPUT || mode == GPIO_CTRL_MODE_INPUT_OUTPUT_OPEN_DRAIN;
}

static int64_t GpioCtrl_NowUs(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
}

static void IRAM_ATTR GpioCtrl_IsrHandler(void* arg)
{
  int logicalPin = (int)(intptr_t)arg;
  if (!traceActive || !GpioCtrl_IsValidLogicalPin(logicalPin)) {
    return;
  }
  int hw = GpioCtrl_Hw(logicalPin);
  int level = gpio_get_level(hw);
  bool raising = level != 0;
  if (traceEdge == GPIO_CTRL_EDGE_RAISING && !raising) return;
  if (traceEdge == GPIO_CTRL_EDGE_FALLING && raising) return;

  TraceIsrEvent ev = {
      .logicalPin = logicalPin,
      .timeUs = GpioCtrl_NowUs(),
      .level = level,
      .raising = raising,
  };
  BaseType_t hp = pdFALSE;
  xQueueSendFromISR(traceQueue, &ev, &hp);
  if (hp) {
    portYIELD_FROM_ISR();
  }
}

static esp_err_t GpioCtrl_InitPowerRail(int hwPin, bool* enableOut)
{
  gpio_config_t cfg = {
      .pin_bit_mask = 1ULL << hwPin,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  TOOL_CHECK_ESP_OK_OR_RETURN(gpio_config(&cfg));
  TOOL_CHECK_ESP_OK_OR_RETURN(gpio_set_level(hwPin, 0));
  *enableOut = false;
  return ESP_OK;
}

esp_err_t GpioCtrl_Init(void)
{
  memset(pins, 0, sizeof(pins));
  traceQueue = xQueueCreate(CONFIG_GPIO_TRACE_MAX_EVENTS, sizeof(TraceIsrEvent));
  TOOL_CHECK_OR_LOG_RETURN(traceQueue == NULL, "trace queue create failed");

  TOOL_CHECK_ESP_OK_OR_RETURN(gpio_install_isr_service(0));

  for (int i = 0; i < GpioCtrl_GetLogicalCount(); i++) {
    pins[i].mode = GPIO_CTRL_MODE_DISABLE;
    int hw = GpioCtrl_Hw(i);
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << hw,
        .mode = GPIO_MODE_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    TOOL_CHECK_ESP_OK_OR_RETURN(gpio_config(&cfg));
    TOOL_CHECK_ESP_OK_OR_RETURN(gpio_isr_handler_add(hw, GpioCtrl_IsrHandler, (void*)(intptr_t)i));
  }

  TOOL_CHECK_ESP_OK_OR_RETURN(GpioCtrl_InitPowerRail(CONFIG_GPIO_POWER_3V3_PIN, &powerEnable3v3));
  TOOL_CHECK_ESP_OK_OR_RETURN(GpioCtrl_InitPowerRail(CONFIG_GPIO_POWER_5V_PIN, &powerEnable5v));
  return ESP_OK;
}

esp_err_t GpioCtrl_SetConfig(int logicalPin, GpioCtrl_Mode mode, bool pullUp, bool pullDown)
{
  if (!GpioCtrl_IsValidLogicalPin(logicalPin)) return ESP_ERR_INVALID_ARG;
  int hw = GpioCtrl_Hw(logicalPin);

  gpio_config_t cfg = {
      .pin_bit_mask = 1ULL << hw,
      .mode = GpioCtrl_ToEspMode(mode),
      .pull_up_en = pullUp ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
      .pull_down_en = pullDown ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  TOOL_CHECK_ESP_OK_OR_RETURN(gpio_config(&cfg));
  pins[logicalPin].mode = mode;
  pins[logicalPin].pullUp = pullUp;
  pins[logicalPin].pullDown = pullDown;
  pins[logicalPin].interruptActive = false;
  return ESP_OK;
}

esp_err_t GpioCtrl_GetState(int logicalPin, GpioCtrl_State* out)
{
  if (!GpioCtrl_IsValidLogicalPin(logicalPin) || out == NULL) return ESP_ERR_INVALID_ARG;
  out->mode = pins[logicalPin].mode;
  out->pullUp = pins[logicalPin].pullUp;
  out->pullDown = pins[logicalPin].pullDown;
  out->level = gpio_get_level(GpioCtrl_Hw(logicalPin));
  return ESP_OK;
}

esp_err_t GpioCtrl_GetLevel(int logicalPin, int* level)
{
  if (!GpioCtrl_IsValidLogicalPin(logicalPin) || level == NULL) return ESP_ERR_INVALID_ARG;
  *level = gpio_get_level(GpioCtrl_Hw(logicalPin));
  return ESP_OK;
}

esp_err_t GpioCtrl_SetLevel(int logicalPin, int level)
{
  if (!GpioCtrl_IsValidLogicalPin(logicalPin) || (level != 0 && level != 1)) return ESP_ERR_INVALID_ARG;
  if (!GpioCtrl_IsOutputCapable(logicalPin)) return ESP_ERR_INVALID_STATE;
  return gpio_set_level(GpioCtrl_Hw(logicalPin), level);
}

esp_err_t GpioCtrl_Pulse(int logicalPin, int level, uint64_t widthUs)
{
  if (!GpioCtrl_IsValidLogicalPin(logicalPin) || (level != 0 && level != 1)) return ESP_ERR_INVALID_ARG;
  if (widthUs == 0 || widthUs > CONFIG_GPIO_PULSE_MAX_WIDTH_US) return ESP_ERR_INVALID_ARG;
  if (!GpioCtrl_IsOutputCapable(logicalPin)) return ESP_ERR_INVALID_STATE;

  int hw = GpioCtrl_Hw(logicalPin);
  TOOL_CHECK_ESP_OK_OR_RETURN(gpio_set_level(hw, level));
  esp_rom_delay_us((uint32_t)widthUs);
  TOOL_CHECK_ESP_OK_OR_RETURN(gpio_set_level(hw, level ? 0 : 1));
  return ESP_OK;
}

esp_err_t GpioCtrl_GetPowerEnable(GpioCtrl_PowerRail rail, bool* enable)
{
  if (enable == NULL) return ESP_ERR_INVALID_ARG;
  if (rail == GPIO_CTRL_POWER_3V3) {
    *enable = powerEnable3v3;
    return ESP_OK;
  }
  if (rail == GPIO_CTRL_POWER_5V) {
    *enable = powerEnable5v;
    return ESP_OK;
  }
  return ESP_ERR_INVALID_ARG;
}

esp_err_t GpioCtrl_SetPowerEnable(GpioCtrl_PowerRail rail, bool enable)
{
  int hw = GpioCtrl_PowerHw(rail);
  TOOL_CHECK_ESP_OK_OR_RETURN(gpio_set_level(hw, enable ? 1 : 0));
  if (rail == GPIO_CTRL_POWER_3V3) {
    powerEnable3v3 = enable;
  } else if (rail == GPIO_CTRL_POWER_5V) {
    powerEnable5v = enable;
  } else {
    return ESP_ERR_INVALID_ARG;
  }
  return ESP_OK;
}

static esp_err_t GpioCtrl_EnableTraceInterrupt(int logicalPin, GpioCtrl_Edge edge)
{
  int hw = GpioCtrl_Hw(logicalPin);
  gpio_int_type_t type = GPIO_INTR_ANYEDGE;
  if (edge == GPIO_CTRL_EDGE_RAISING) type = GPIO_INTR_POSEDGE;
  if (edge == GPIO_CTRL_EDGE_FALLING) type = GPIO_INTR_NEGEDGE;
  TOOL_CHECK_ESP_OK_OR_RETURN(gpio_set_intr_type(hw, type));
  TOOL_CHECK_ESP_OK_OR_RETURN(gpio_intr_enable(hw));
  pins[logicalPin].interruptActive = true;
  return ESP_OK;
}

static esp_err_t GpioCtrl_DisableTraceInterrupt(int logicalPin)
{
  int hw = GpioCtrl_Hw(logicalPin);
  gpio_intr_disable(hw);
  gpio_set_intr_type(hw, GPIO_INTR_DISABLE);
  pins[logicalPin].interruptActive = false;
  return ESP_OK;
}

esp_err_t GpioCtrl_Trace(const int* logicalPins, size_t pinCount, GpioCtrl_Edge edge, uint64_t durationUs,
                         GpioCtrl_TraceEvent* eventsOut, size_t maxEvents, size_t* eventCountOut, bool includePin)
{
  if (logicalPins == NULL || pinCount == 0 || eventsOut == NULL || eventCountOut == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (durationUs == 0 || durationUs > CONFIG_GPIO_TRACE_MAX_DURATION_US) {
    return ESP_ERR_INVALID_ARG;
  }

  xQueueReset(traceQueue);
  traceEdge = edge;
  traceActive = true;

  for (size_t i = 0; i < pinCount; i++) {
    if (!GpioCtrl_IsValidLogicalPin(logicalPins[i])) {
      traceActive = false;
      return ESP_ERR_INVALID_ARG;
    }
    if (GpioCtrl_EnableTraceInterrupt(logicalPins[i], edge) != ESP_OK) {
      traceActive = false;
      return ESP_FAIL;
    }
  }

  int64_t endUs = esp_timer_get_time() + (int64_t)durationUs;
  size_t count = 0;
  while (esp_timer_get_time() < endUs) {
    TraceIsrEvent ev;
    TickType_t wait = pdMS_TO_TICKS(20);
    if (xQueueReceive(traceQueue, &ev, wait) == pdTRUE) {
      if (count < maxEvents) {
        eventsOut[count].pin = includePin ? ev.logicalPin : -1;
        eventsOut[count].edge = ev.raising ? "raising" : "falling";
        eventsOut[count].level = ev.level;
        eventsOut[count].time = ev.timeUs;
        count++;
      }
    }
  }

  traceActive = false;
  for (size_t i = 0; i < pinCount; i++) {
    GpioCtrl_DisableTraceInterrupt(logicalPins[i]);
  }
  /* Drain leftover */
  TraceIsrEvent dump;
  while (xQueueReceive(traceQueue, &dump, 0) == pdTRUE) {
  }

  *eventCountOut = count;
  return ESP_OK;
}
