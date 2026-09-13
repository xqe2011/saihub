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

static const char* tag = "Gpio";

/* Private logical (0..7) -> hardware GPIO. Never expose via API. */
static const int s_logicalToHw[CONFIG_GPIO_LOGICAL_COUNT] = {0, 1, 2, 3, 4, 5, 6, 7};

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

static GpioPinRuntime s_pins[CONFIG_GPIO_LOGICAL_COUNT];
static QueueHandle_t s_traceQueue;
static volatile bool s_traceActive = false;
static GpioCtrl_Edge s_traceEdge = GPIO_CTRL_EDGE_BOTH;

bool GpioCtrl_IsValidLogicalPin(int pin)
{
  return pin >= 0 && pin < CONFIG_GPIO_LOGICAL_COUNT;
}

static int GpioCtrl_Hw(int logicalPin)
{
  return s_logicalToHw[logicalPin];
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
  GpioCtrl_Mode mode = s_pins[logicalPin].mode;
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
  if (!s_traceActive || !GpioCtrl_IsValidLogicalPin(logicalPin)) {
    return;
  }
  int hw = GpioCtrl_Hw(logicalPin);
  int level = gpio_get_level(hw);
  bool raising = level != 0;
  if (s_traceEdge == GPIO_CTRL_EDGE_RAISING && !raising) return;
  if (s_traceEdge == GPIO_CTRL_EDGE_FALLING && raising) return;

  TraceIsrEvent ev = {
      .logicalPin = logicalPin,
      .timeUs = GpioCtrl_NowUs(),
      .level = level,
      .raising = raising,
  };
  BaseType_t hp = pdFALSE;
  xQueueSendFromISR(s_traceQueue, &ev, &hp);
  if (hp) {
    portYIELD_FROM_ISR();
  }
}

esp_err_t GpioCtrl_Init(void)
{
  memset(s_pins, 0, sizeof(s_pins));
  s_traceQueue = xQueueCreate(CONFIG_TRACE_MAX_EVENTS, sizeof(TraceIsrEvent));
  TOOL_CHECK_OR_LOG_RETURN(s_traceQueue == NULL, "trace queue create failed");

  TOOL_CHECK_ESP_OK_OR_RETURN(gpio_install_isr_service(0));

  for (int i = 0; i < CONFIG_GPIO_LOGICAL_COUNT; i++) {
    s_pins[i].mode = GPIO_CTRL_MODE_DISABLE;
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
  s_pins[logicalPin].mode = mode;
  s_pins[logicalPin].pullUp = pullUp;
  s_pins[logicalPin].pullDown = pullDown;
  s_pins[logicalPin].interruptActive = false;
  return ESP_OK;
}

esp_err_t GpioCtrl_GetState(int logicalPin, GpioCtrl_State* out)
{
  if (!GpioCtrl_IsValidLogicalPin(logicalPin) || out == NULL) return ESP_ERR_INVALID_ARG;
  out->mode = s_pins[logicalPin].mode;
  out->pullUp = s_pins[logicalPin].pullUp;
  out->pullDown = s_pins[logicalPin].pullDown;
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
  if (widthUs == 0 || widthUs > CONFIG_PULSE_MAX_WIDTH_US) return ESP_ERR_INVALID_ARG;
  if (!GpioCtrl_IsOutputCapable(logicalPin)) return ESP_ERR_INVALID_STATE;

  int hw = GpioCtrl_Hw(logicalPin);
  TOOL_CHECK_ESP_OK_OR_RETURN(gpio_set_level(hw, level));
  esp_rom_delay_us((uint32_t)widthUs);
  TOOL_CHECK_ESP_OK_OR_RETURN(gpio_set_level(hw, level ? 0 : 1));
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
  s_pins[logicalPin].interruptActive = true;
  return ESP_OK;
}

static esp_err_t GpioCtrl_DisableTraceInterrupt(int logicalPin)
{
  int hw = GpioCtrl_Hw(logicalPin);
  gpio_intr_disable(hw);
  gpio_set_intr_type(hw, GPIO_INTR_DISABLE);
  s_pins[logicalPin].interruptActive = false;
  return ESP_OK;
}

esp_err_t GpioCtrl_Trace(const int* logicalPins, size_t pinCount, GpioCtrl_Edge edge, uint64_t durationUs,
                         GpioCtrl_TraceEvent* eventsOut, size_t maxEvents, size_t* eventCountOut, bool includePin)
{
  if (logicalPins == NULL || pinCount == 0 || eventsOut == NULL || eventCountOut == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (durationUs == 0 || durationUs > CONFIG_TRACE_MAX_DURATION_US) {
    return ESP_ERR_INVALID_ARG;
  }

  xQueueReset(s_traceQueue);
  s_traceEdge = edge;
  s_traceActive = true;

  for (size_t i = 0; i < pinCount; i++) {
    if (!GpioCtrl_IsValidLogicalPin(logicalPins[i])) {
      s_traceActive = false;
      return ESP_ERR_INVALID_ARG;
    }
    if (GpioCtrl_EnableTraceInterrupt(logicalPins[i], edge) != ESP_OK) {
      s_traceActive = false;
      return ESP_FAIL;
    }
  }

  int64_t endUs = esp_timer_get_time() + (int64_t)durationUs;
  size_t count = 0;
  while (esp_timer_get_time() < endUs) {
    TraceIsrEvent ev;
    TickType_t wait = pdMS_TO_TICKS(20);
    if (xQueueReceive(s_traceQueue, &ev, wait) == pdTRUE) {
      if (count < maxEvents) {
        eventsOut[count].pin = includePin ? ev.logicalPin : -1;
        eventsOut[count].edge = ev.raising ? "raising" : "falling";
        eventsOut[count].level = ev.level;
        eventsOut[count].time = ev.timeUs;
        count++;
      }
    }
  }

  s_traceActive = false;
  for (size_t i = 0; i < pinCount; i++) {
    GpioCtrl_DisableTraceInterrupt(logicalPins[i]);
  }
  /* Drain leftover */
  TraceIsrEvent dump;
  while (xQueueReceive(s_traceQueue, &dump, 0) == pdTRUE) {
  }

  *eventCountOut = count;
  return ESP_OK;
}
