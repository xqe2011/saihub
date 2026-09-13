/**
 * @name GPIO control module
 * @file gpio_ctrl.c
 * @author xqe2011
 */
#include "gpio_ctrl.h"

#include "config.h"
#include "tool.h"

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_log.h>
#include <esp_rom_gpio.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <math.h>
#include <soc/gpio_sig_map.h>
#include <string.h>
#include <sys/time.h>

static const char* tag = "SAIHUB-Gpio";

#define GPIO_CTRL_LEDC_SPEED LEDC_LOW_SPEED_MODE
#define GPIO_CTRL_LEDC_DUTY_MAX (1U << CONFIG_GPIO_PWM_DUTY_RES_BITS)
#define GPIO_CTRL_LEDC_CHANNEL_COUNT SOC_LEDC_CHANNEL_NUM
#define GPIO_CTRL_LEDC_TIMER_COUNT SOC_LEDC_TIMER_NUM

typedef struct {
  GpioCtrl_Mode mode;
  bool openDrain;
  bool pullUp;
  bool pullDown;
  bool interruptActive;
  bool pwmActive;
  int ledcChannel; /* -1 if none */
  int ledcTimer;   /* -1 if none */
  double pwmFrequencyHz;
  double pwmDutyPercent;
} GpioPinRuntime;

typedef struct {
  int logicalPin;
  int64_t timeUs;
  int level;
  bool raising;
} TraceIsrEvent;

typedef struct {
  bool inUse;
  uint32_t frequencyHz;
  int refCount;
} LedcTimerSlot;

static int logicalToHw[] = CONFIG_GPIO_LOGICAL_TO_HW;
static GpioPinRuntime pins[TOOL_GET_ARRAY_LENGTH(logicalToHw)];
static bool channelInUse[GPIO_CTRL_LEDC_CHANNEL_COUNT];
static LedcTimerSlot timers[GPIO_CTRL_LEDC_TIMER_COUNT];
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
    case GPIO_CTRL_MODE_DIGITAL_INPUT:
      return "digitalInput";
    case GPIO_CTRL_MODE_DIGITAL_OUTPUT:
      return "digitalOutput";
    case GPIO_CTRL_MODE_DIGITAL_INPUT_OUTPUT:
      return "digitalInputOutput";
    case GPIO_CTRL_MODE_PWM_OUTPUT:
      return "pwmOutput";
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
  if (strcmp(s, "digitalInput") == 0) {
    *out = GPIO_CTRL_MODE_DIGITAL_INPUT;
    return true;
  }
  if (strcmp(s, "digitalOutput") == 0) {
    *out = GPIO_CTRL_MODE_DIGITAL_OUTPUT;
    return true;
  }
  if (strcmp(s, "digitalInputOutput") == 0) {
    *out = GPIO_CTRL_MODE_DIGITAL_INPUT_OUTPUT;
    return true;
  }
  if (strcmp(s, "pwmOutput") == 0) {
    *out = GPIO_CTRL_MODE_PWM_OUTPUT;
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

static gpio_mode_t GpioCtrl_ToEspMode(GpioCtrl_Mode mode, bool openDrain)
{
  switch (mode) {
    case GPIO_CTRL_MODE_DISABLE:
      return GPIO_MODE_DISABLE;
    case GPIO_CTRL_MODE_DIGITAL_INPUT:
      return GPIO_MODE_INPUT;
    case GPIO_CTRL_MODE_DIGITAL_OUTPUT:
      return openDrain ? GPIO_MODE_OUTPUT_OD : GPIO_MODE_OUTPUT;
    case GPIO_CTRL_MODE_DIGITAL_INPUT_OUTPUT:
      return openDrain ? GPIO_MODE_INPUT_OUTPUT_OD : GPIO_MODE_INPUT_OUTPUT;
    case GPIO_CTRL_MODE_PWM_OUTPUT:
      return openDrain ? GPIO_MODE_OUTPUT_OD : GPIO_MODE_OUTPUT;
    default:
      return GPIO_MODE_DISABLE;
  }
}

static gpio_pull_mode_t GpioCtrl_ToPullMode(bool pullUp, bool pullDown)
{
  if (pullUp && pullDown) return GPIO_PULLUP_PULLDOWN;
  if (pullUp) return GPIO_PULLUP_ONLY;
  if (pullDown) return GPIO_PULLDOWN_ONLY;
  return GPIO_FLOATING;
}

static esp_err_t GpioCtrl_SyncPad(int logicalPin)
{
  if (!GpioCtrl_IsValidLogicalPin(logicalPin)) return ESP_ERR_INVALID_ARG;
  int hw = GpioCtrl_Hw(logicalPin);
  GpioPinRuntime* p = &pins[logicalPin];
  TOOL_CHECK_ESP_OK_OR_RETURN(gpio_set_direction(hw, GpioCtrl_ToEspMode(p->mode, p->openDrain)));
  TOOL_CHECK_ESP_OK_OR_RETURN(gpio_set_pull_mode(hw, GpioCtrl_ToPullMode(p->pullUp, p->pullDown)));
  return ESP_OK;
}

bool GpioCtrl_IsOutputCapable(int logicalPin)
{
  if (!GpioCtrl_IsValidLogicalPin(logicalPin)) return false;
  GpioCtrl_Mode mode = pins[logicalPin].mode;
  return mode == GPIO_CTRL_MODE_DIGITAL_OUTPUT || mode == GPIO_CTRL_MODE_DIGITAL_INPUT_OUTPUT;
}

bool GpioCtrl_IsPwmMode(int logicalPin)
{
  if (!GpioCtrl_IsValidLogicalPin(logicalPin)) return false;
  return pins[logicalPin].mode == GPIO_CTRL_MODE_PWM_OUTPUT;
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
  gpio_reset_pin(hwPin);
  TOOL_CHECK_ESP_OK_OR_RETURN(gpio_set_direction(hwPin, GPIO_MODE_OUTPUT));
  TOOL_CHECK_ESP_OK_OR_RETURN(gpio_set_pull_mode(hwPin, GPIO_FLOATING));
  TOOL_CHECK_ESP_OK_OR_RETURN(gpio_set_level(hwPin, 0));
  *enableOut = false;
  return ESP_OK;
}

static int GpioCtrl_AllocChannel(void)
{
  for (int i = 0; i < GPIO_CTRL_LEDC_CHANNEL_COUNT; i++) {
    if (!channelInUse[i]) {
      channelInUse[i] = true;
      return i;
    }
  }
  return -1;
}

static void GpioCtrl_FreeChannel(int channel)
{
  if (channel >= 0 && channel < GPIO_CTRL_LEDC_CHANNEL_COUNT) {
    channelInUse[channel] = false;
  }
}

static int GpioCtrl_AcquireTimer(uint32_t frequencyHz)
{
  for (int i = 0; i < GPIO_CTRL_LEDC_TIMER_COUNT; i++) {
    if (timers[i].inUse && timers[i].frequencyHz == frequencyHz) {
      timers[i].refCount++;
      return i;
    }
  }
  for (int i = 0; i < GPIO_CTRL_LEDC_TIMER_COUNT; i++) {
    if (!timers[i].inUse) {
      ledc_timer_config_t cfg = {
          .speed_mode = GPIO_CTRL_LEDC_SPEED,
          .duty_resolution = (ledc_timer_bit_t)CONFIG_GPIO_PWM_DUTY_RES_BITS,
          .timer_num = (ledc_timer_t)i,
          .freq_hz = frequencyHz,
          .clk_cfg = LEDC_AUTO_CLK,
          .deconfigure = false,
      };
      if (ledc_timer_config(&cfg) != ESP_OK) {
        return -1;
      }
      timers[i].inUse = true;
      timers[i].frequencyHz = frequencyHz;
      timers[i].refCount = 1;
      return i;
    }
  }
  return -1;
}

static void GpioCtrl_ReleaseTimer(int timer)
{
  if (timer < 0 || timer >= GPIO_CTRL_LEDC_TIMER_COUNT) return;
  if (!timers[timer].inUse) return;
  if (timers[timer].refCount > 0) {
    timers[timer].refCount--;
  }
  if (timers[timer].refCount == 0) {
    ledc_timer_pause(GPIO_CTRL_LEDC_SPEED, (ledc_timer_t)timer);
    ledc_timer_config_t cfg = {
        .speed_mode = GPIO_CTRL_LEDC_SPEED,
        .timer_num = (ledc_timer_t)timer,
        .deconfigure = true,
    };
    ledc_timer_config(&cfg);
    timers[timer].inUse = false;
    timers[timer].frequencyHz = 0;
  }
}

static uint32_t GpioCtrl_DutyToTicks(double dutyPercent)
{
  if (dutyPercent <= 0.0) return 0;
  if (dutyPercent >= 100.0) return GPIO_CTRL_LEDC_DUTY_MAX;
  double ticks = (dutyPercent / 100.0) * (double)GPIO_CTRL_LEDC_DUTY_MAX;
  uint32_t rounded = (uint32_t)lround(ticks);
  if (rounded > GPIO_CTRL_LEDC_DUTY_MAX) rounded = GPIO_CTRL_LEDC_DUTY_MAX;
  return rounded;
}

static esp_err_t GpioCtrl_DetachPwm(int logicalPin)
{
  GpioPinRuntime* p = &pins[logicalPin];
  if (!p->pwmActive) return ESP_OK;

  int hw = GpioCtrl_Hw(logicalPin);
  if (p->ledcChannel >= 0) {
    ledc_stop(GPIO_CTRL_LEDC_SPEED, (ledc_channel_t)p->ledcChannel, 0);
    GpioCtrl_FreeChannel(p->ledcChannel);
  }
  if (p->ledcTimer >= 0) {
    GpioCtrl_ReleaseTimer(p->ledcTimer);
  }
  esp_rom_gpio_connect_out_signal(hw, SIG_GPIO_OUT_IDX, false, false);

  p->pwmActive = false;
  p->ledcChannel = -1;
  p->ledcTimer = -1;
  return ESP_OK;
}

static esp_err_t GpioCtrl_AttachPwm(int logicalPin, uint32_t frequencyHz, double dutyPercent)
{
  GpioPinRuntime* p = &pins[logicalPin];
  int hw = GpioCtrl_Hw(logicalPin);

  if (p->pwmActive) {
    TOOL_CHECK_ESP_OK_OR_RETURN(GpioCtrl_DetachPwm(logicalPin));
  }

  int channel = GpioCtrl_AllocChannel();
  if (channel < 0) {
    ESP_LOGW(tag, "no free LEDC channel for pin %d", logicalPin);
    return ESP_ERR_NO_MEM;
  }
  int timer = GpioCtrl_AcquireTimer(frequencyHz);
  if (timer < 0) {
    GpioCtrl_FreeChannel(channel);
    ESP_LOGW(tag, "no free LEDC timer for pin %d freq %u", logicalPin, (unsigned)frequencyHz);
    return ESP_ERR_NO_MEM;
  }

  ledc_channel_config_t ch = {
      .gpio_num = hw,
      .speed_mode = GPIO_CTRL_LEDC_SPEED,
      .channel = (ledc_channel_t)channel,
      .intr_type = LEDC_INTR_DISABLE,
      .timer_sel = (ledc_timer_t)timer,
      .duty = GpioCtrl_DutyToTicks(dutyPercent),
      .hpoint = 0,
      .flags = {.output_invert = 0},
  };
  esp_err_t err = ledc_channel_config(&ch);
  if (err != ESP_OK) {
    GpioCtrl_ReleaseTimer(timer);
    GpioCtrl_FreeChannel(channel);
    return err;
  }

  p->pwmActive = true;
  p->ledcChannel = channel;
  p->ledcTimer = timer;
  p->pwmFrequencyHz = (double)frequencyHz;
  p->pwmDutyPercent = dutyPercent;
  return ESP_OK;
}

static esp_err_t GpioCtrl_ApplyPwm(int logicalPin, uint32_t frequencyHz, double dutyPercent)
{
  GpioPinRuntime* p = &pins[logicalPin];
  if (!p->pwmActive || p->ledcChannel < 0 || p->ledcTimer < 0) {
    return GpioCtrl_AttachPwm(logicalPin, frequencyHz, dutyPercent);
  }

  if (timers[p->ledcTimer].frequencyHz != frequencyHz) {
    int oldTimer = p->ledcTimer;
    int newTimer = GpioCtrl_AcquireTimer(frequencyHz);
    if (newTimer < 0) {
      return ESP_ERR_NO_MEM;
    }
    TOOL_CHECK_ESP_OK_OR_RETURN(ledc_bind_channel_timer(GPIO_CTRL_LEDC_SPEED, (ledc_channel_t)p->ledcChannel, (ledc_timer_t)newTimer));
    GpioCtrl_ReleaseTimer(oldTimer);
    p->ledcTimer = newTimer;
  }

  TOOL_CHECK_ESP_OK_OR_RETURN(
      ledc_set_duty_and_update(GPIO_CTRL_LEDC_SPEED, (ledc_channel_t)p->ledcChannel, GpioCtrl_DutyToTicks(dutyPercent), 0));
  p->pwmFrequencyHz = (double)frequencyHz;
  p->pwmDutyPercent = dutyPercent;
  return ESP_OK;
}

esp_err_t GpioCtrl_Init(void)
{
  memset(pins, 0, sizeof(pins));
  memset(channelInUse, 0, sizeof(channelInUse));
  memset(timers, 0, sizeof(timers));
  for (int i = 0; i < GpioCtrl_GetLogicalCount(); i++) {
    pins[i].mode = GPIO_CTRL_MODE_DISABLE;
    pins[i].ledcChannel = -1;
    pins[i].ledcTimer = -1;
    pins[i].pwmFrequencyHz = (double)CONFIG_GPIO_PWM_DEFAULT_FREQ_HZ;
    pins[i].pwmDutyPercent = 0.0;
  }

  traceQueue = xQueueCreate(CONFIG_GPIO_TRACE_MAX_EVENTS, sizeof(TraceIsrEvent));
  TOOL_CHECK_OR_LOG_RETURN(traceQueue == NULL, "trace queue create failed");

  TOOL_CHECK_ESP_OK_OR_RETURN(gpio_install_isr_service(0));

  for (int i = 0; i < GpioCtrl_GetLogicalCount(); i++) {
    int hw = GpioCtrl_Hw(i);
    gpio_reset_pin(hw);
    TOOL_CHECK_ESP_OK_OR_RETURN(GpioCtrl_SyncPad(i));
    TOOL_CHECK_ESP_OK_OR_RETURN(gpio_isr_handler_add(hw, GpioCtrl_IsrHandler, (void*)(intptr_t)i));
  }

  TOOL_CHECK_ESP_OK_OR_RETURN(GpioCtrl_InitPowerRail(CONFIG_GPIO_POWER_3V3_PIN, &powerEnable3v3));
  TOOL_CHECK_ESP_OK_OR_RETURN(GpioCtrl_InitPowerRail(CONFIG_GPIO_POWER_5V_PIN, &powerEnable5v));
  return ESP_OK;
}

esp_err_t GpioCtrl_SetConfig(int logicalPin, GpioCtrl_Mode mode, bool openDrain, bool pullUp, bool pullDown)
{
  if (!GpioCtrl_IsValidLogicalPin(logicalPin)) return ESP_ERR_INVALID_ARG;
  if (openDrain && (mode == GPIO_CTRL_MODE_DISABLE || mode == GPIO_CTRL_MODE_DIGITAL_INPUT)) {
    return ESP_ERR_INVALID_ARG;
  }

  GpioPinRuntime* p = &pins[logicalPin];
  GpioCtrl_Mode prev = p->mode;

  if (prev == GPIO_CTRL_MODE_PWM_OUTPUT && mode != GPIO_CTRL_MODE_PWM_OUTPUT) {
    TOOL_CHECK_ESP_OK_OR_RETURN(GpioCtrl_DetachPwm(logicalPin));
  }

  p->mode = mode;
  p->openDrain = openDrain;
  p->pullUp = pullUp;
  p->pullDown = pullDown;
  p->interruptActive = false;

  if (mode == GPIO_CTRL_MODE_PWM_OUTPUT) {
    uint32_t freq = (uint32_t)lround(p->pwmFrequencyHz);
    if (freq < 1) freq = CONFIG_GPIO_PWM_DEFAULT_FREQ_HZ;
    if (freq > CONFIG_GPIO_PWM_MAX_FREQ_HZ) freq = CONFIG_GPIO_PWM_MAX_FREQ_HZ;
    esp_err_t err = GpioCtrl_AttachPwm(logicalPin, freq, p->pwmDutyPercent);
    if (err != ESP_OK) {
      p->mode = prev;
      return err;
    }
  }

  TOOL_CHECK_ESP_OK_OR_RETURN(GpioCtrl_SyncPad(logicalPin));
  return ESP_OK;
}

esp_err_t GpioCtrl_GetState(int logicalPin, GpioCtrl_State* out)
{
  if (!GpioCtrl_IsValidLogicalPin(logicalPin) || out == NULL) return ESP_ERR_INVALID_ARG;
  out->mode = pins[logicalPin].mode;
  out->openDrain = pins[logicalPin].openDrain;
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

esp_err_t GpioCtrl_SetPwm(int logicalPin, double frequencyHz, double dutyPercent)
{
  if (!GpioCtrl_IsValidLogicalPin(logicalPin)) return ESP_ERR_INVALID_ARG;
  if (!GpioCtrl_IsPwmMode(logicalPin)) return ESP_ERR_INVALID_STATE;
  if (frequencyHz < 1.0 || frequencyHz > (double)CONFIG_GPIO_PWM_MAX_FREQ_HZ) return ESP_ERR_INVALID_ARG;
  if (dutyPercent < 0.0 || dutyPercent > 100.0) return ESP_ERR_INVALID_ARG;

  uint32_t freq = (uint32_t)lround(frequencyHz);
  if (freq < 1) freq = 1;
  if (freq > CONFIG_GPIO_PWM_MAX_FREQ_HZ) freq = CONFIG_GPIO_PWM_MAX_FREQ_HZ;

  esp_err_t err = GpioCtrl_ApplyPwm(logicalPin, freq, dutyPercent);
  if (err != ESP_OK) return err;
  pins[logicalPin].pwmFrequencyHz = frequencyHz;
  pins[logicalPin].pwmDutyPercent = dutyPercent;
  TOOL_CHECK_ESP_OK_OR_RETURN(GpioCtrl_SyncPad(logicalPin));
  return ESP_OK;
}

esp_err_t GpioCtrl_GetPwm(int logicalPin, double* frequencyHz, double* dutyPercent)
{
  if (!GpioCtrl_IsValidLogicalPin(logicalPin) || frequencyHz == NULL || dutyPercent == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!GpioCtrl_IsPwmMode(logicalPin)) return ESP_ERR_INVALID_STATE;
  *frequencyHz = pins[logicalPin].pwmFrequencyHz;
  *dutyPercent = pins[logicalPin].pwmDutyPercent;
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
