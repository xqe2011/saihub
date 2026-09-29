/**
 * @name PWM control module
 * @file pwm_ctrl.c
 * @author xqe2011
 */
#include "pwm_ctrl.h"

#include "config.h"
#include "tool.h"

#include <driver/ledc.h>
#include <esp_log.h>
#include <math.h>
#include <string.h>

static const char* tag = "SAIHub-Pwm";

#define PWM_CTRL_LEDC_SPEED LEDC_LOW_SPEED_MODE
#define PWM_CTRL_LEDC_TIMER_COUNT SOC_LEDC_TIMER_NUM
#define PWM_CTRL_LEDC_CHANNEL_COUNT SOC_LEDC_TIMER_NUM
#define PWM_CTRL_LEDC_SRC_CLK_HZ 80000000U

_Static_assert(CONFIG_GPIO_PWM_MAX_OUTPUTS <= PWM_CTRL_LEDC_TIMER_COUNT,
               "Configured PWM output limit exceeds exclusive hardware timers");

typedef struct {
  bool inUse;
  uint32_t frequencyHz;
  uint32_t dutyResolution;
} LedcTimerSlot;

typedef struct {
  bool inUse;
  int hwPin;
  uint32_t frequencyHz;
  double dutyPercent;
  int ledcChannel;
  int ledcTimer;
} PwmCtrl_Slot;

static bool ready;
static bool ledcChannelInUse[PWM_CTRL_LEDC_CHANNEL_COUNT];
static LedcTimerSlot ledcTimers[PWM_CTRL_LEDC_TIMER_COUNT];
static PwmCtrl_Slot slots[CONFIG_GPIO_PWM_MAX_OUTPUTS];

static uint32_t PwmCtrl_LedcResolutionBits(uint32_t frequencyHz)
{
  return ledc_find_suitable_duty_resolution(PWM_CTRL_LEDC_SRC_CLK_HZ, frequencyHz);
}

static uint32_t PwmCtrl_DutyToTicks(double dutyPercent, uint32_t resolutionBits)
{
  uint32_t dutyMax = 1U << resolutionBits;
  if (dutyPercent <= 0.0) return 0;
  if (dutyPercent >= 100.0) return dutyMax;
  double ticks = (dutyPercent / 100.0) * (double)dutyMax;
  uint32_t rounded = (uint32_t)lround(ticks);
  if (rounded > dutyMax) rounded = dutyMax;
  return rounded;
}

static PwmCtrl_Slot* PwmCtrl_FindByHw(int hwPin)
{
  for (int i = 0; i < CONFIG_GPIO_PWM_MAX_OUTPUTS; i++) {
    if (slots[i].inUse && slots[i].hwPin == hwPin) return &slots[i];
  }
  return NULL;
}

static PwmCtrl_Slot* PwmCtrl_AllocSlot(int hwPin)
{
  for (int i = 0; i < CONFIG_GPIO_PWM_MAX_OUTPUTS; i++) {
    if (!slots[i].inUse) {
      memset(&slots[i], 0, sizeof(slots[i]));
      slots[i].inUse = true;
      slots[i].hwPin = hwPin;
      slots[i].ledcChannel = -1;
      slots[i].ledcTimer = -1;
      return &slots[i];
    }
  }
  return NULL;
}

static void PwmCtrl_FreeSlot(PwmCtrl_Slot* slot)
{
  if (slot == NULL) return;
  memset(slot, 0, sizeof(*slot));
  slot->ledcChannel = -1;
  slot->ledcTimer = -1;
}

static int PwmCtrl_AllocLedcChannel(void)
{
  for (int i = 0; i < PWM_CTRL_LEDC_CHANNEL_COUNT; i++) {
    if (!ledcChannelInUse[i]) {
      ledcChannelInUse[i] = true;
      return i;
    }
  }
  return -1;
}

static void PwmCtrl_FreeLedcChannel(int channel)
{
  if (channel >= 0 && channel < PWM_CTRL_LEDC_CHANNEL_COUNT) {
    ledcChannelInUse[channel] = false;
  }
}

static esp_err_t PwmCtrl_AcquireLedcTimer(uint32_t frequencyHz, int* timerOut)
{
  if (timerOut == NULL) return ESP_ERR_INVALID_ARG;
  uint32_t resolution = PwmCtrl_LedcResolutionBits(frequencyHz);
  if (resolution == 0) {
    ESP_LOGW(tag, "PWM cannot achieve %u Hz", (unsigned)frequencyHz);
    return ESP_ERR_NOT_SUPPORTED;
  }

  for (int i = 0; i < PWM_CTRL_LEDC_TIMER_COUNT; i++) {
    if (!ledcTimers[i].inUse) {
      ledc_timer_config_t cfg = {
          .speed_mode = PWM_CTRL_LEDC_SPEED,
          .duty_resolution = (ledc_timer_bit_t)resolution,
          .timer_num = (ledc_timer_t)i,
          .freq_hz = frequencyHz,
          .clk_cfg = LEDC_USE_PLL_DIV_CLK,
          .deconfigure = false,
      };
      if (ledc_timer_config(&cfg) != ESP_OK) return ESP_ERR_NOT_SUPPORTED;
      ledcTimers[i].inUse = true;
      ledcTimers[i].frequencyHz = frequencyHz;
      ledcTimers[i].dutyResolution = resolution;
      *timerOut = i;
      ESP_LOGI(tag, "PWM timer allocate: timer=%d frequency=%u resolution=%u", i, (unsigned)frequencyHz,
               (unsigned)resolution);
      return ESP_OK;
    }
  }
  return ESP_ERR_NO_MEM;
}

static void PwmCtrl_ReleaseLedcTimer(int timer)
{
  if (timer < 0 || timer >= PWM_CTRL_LEDC_TIMER_COUNT) return;
  if (!ledcTimers[timer].inUse) return;
  ESP_LOGI(tag, "PWM timer release: timer=%d frequency=%u", timer, (unsigned)ledcTimers[timer].frequencyHz);
  ledc_timer_pause(PWM_CTRL_LEDC_SPEED, (ledc_timer_t)timer);
  ledc_timer_config_t cfg = {
      .speed_mode = PWM_CTRL_LEDC_SPEED,
      .timer_num = (ledc_timer_t)timer,
      .deconfigure = true,
  };
  ledc_timer_config(&cfg);
  ledcTimers[timer].inUse = false;
  ledcTimers[timer].frequencyHz = 0;
  ledcTimers[timer].dutyResolution = 0;
}

static esp_err_t PwmCtrl_ReconfigureLedcTimer(int timer, uint32_t frequencyHz)
{
  if (timer < 0 || timer >= PWM_CTRL_LEDC_TIMER_COUNT || !ledcTimers[timer].inUse) return ESP_ERR_INVALID_STATE;
  uint32_t resolution = PwmCtrl_LedcResolutionBits(frequencyHz);
  if (resolution == 0) return ESP_ERR_NOT_SUPPORTED;

  ledc_timer_config_t cfg = {
      .speed_mode = PWM_CTRL_LEDC_SPEED,
      .duty_resolution = (ledc_timer_bit_t)resolution,
      .timer_num = (ledc_timer_t)timer,
      .freq_hz = frequencyHz,
      .clk_cfg = LEDC_USE_PLL_DIV_CLK,
      .deconfigure = false,
  };
  if (ledc_timer_config(&cfg) != ESP_OK) return ESP_ERR_NOT_SUPPORTED;
  ledcTimers[timer].frequencyHz = frequencyHz;
  ledcTimers[timer].dutyResolution = resolution;
  return ESP_OK;
}

static void PwmCtrl_DetachSlot(PwmCtrl_Slot* slot)
{
  if (slot->ledcChannel >= 0) {
    ledc_stop(PWM_CTRL_LEDC_SPEED, (ledc_channel_t)slot->ledcChannel, 0);
    PwmCtrl_FreeLedcChannel(slot->ledcChannel);
    slot->ledcChannel = -1;
  }
  if (slot->ledcTimer >= 0) {
    PwmCtrl_ReleaseLedcTimer(slot->ledcTimer);
    slot->ledcTimer = -1;
  }
}

static esp_err_t PwmCtrl_AttachSlot(PwmCtrl_Slot* slot, int hwPin, uint32_t frequencyHz, double dutyPercent)
{
  int channel = PwmCtrl_AllocLedcChannel();
  if (channel < 0) return ESP_ERR_NO_MEM;
  int timer = -1;
  esp_err_t terr = PwmCtrl_AcquireLedcTimer(frequencyHz, &timer);
  if (terr != ESP_OK) {
    PwmCtrl_FreeLedcChannel(channel);
    return terr;
  }

  ledc_channel_config_t ch = {
      .gpio_num = hwPin,
      .speed_mode = PWM_CTRL_LEDC_SPEED,
      .channel = (ledc_channel_t)channel,
      .intr_type = LEDC_INTR_DISABLE,
      .timer_sel = (ledc_timer_t)timer,
      .duty = PwmCtrl_DutyToTicks(dutyPercent, ledcTimers[timer].dutyResolution),
      .hpoint = 0,
      .flags = {.output_invert = 0},
  };
  esp_err_t err = ledc_channel_config(&ch);
  if (err != ESP_OK) {
    PwmCtrl_ReleaseLedcTimer(timer);
    PwmCtrl_FreeLedcChannel(channel);
    return err;
  }

  slot->ledcChannel = channel;
  slot->ledcTimer = timer;
  slot->frequencyHz = frequencyHz;
  slot->dutyPercent = dutyPercent;
  ESP_LOGI(tag, "PWM attach: gpio=%d channel=%d timer=%d frequency=%u", hwPin, channel, timer, (unsigned)frequencyHz);
  return ESP_OK;
}

static esp_err_t PwmCtrl_UpdateSlot(PwmCtrl_Slot* slot, uint32_t frequencyHz, double dutyPercent)
{
  if (slot->ledcChannel < 0 || slot->ledcTimer < 0) return ESP_ERR_INVALID_STATE;
  if (ledcTimers[slot->ledcTimer].frequencyHz != frequencyHz) {
    TOOL_CHECK_ESP_OK_OR_RETURN(PwmCtrl_ReconfigureLedcTimer(slot->ledcTimer, frequencyHz));
  }
  TOOL_CHECK_ESP_OK_OR_RETURN(ledc_set_duty_and_update(
      PWM_CTRL_LEDC_SPEED, (ledc_channel_t)slot->ledcChannel,
      PwmCtrl_DutyToTicks(dutyPercent, ledcTimers[slot->ledcTimer].dutyResolution), 0));
  slot->frequencyHz = frequencyHz;
  slot->dutyPercent = dutyPercent;
  return ESP_OK;
}

static esp_err_t PwmCtrl_InitLedcFade(void)
{
  uint32_t resolution = PwmCtrl_LedcResolutionBits(CONFIG_GPIO_PWM_DEFAULT_FREQ_HZ);
  if (resolution == 0) return ESP_ERR_NOT_SUPPORTED;

  ledc_timer_config_t cfg = {
      .speed_mode = PWM_CTRL_LEDC_SPEED,
      .duty_resolution = (ledc_timer_bit_t)resolution,
      .timer_num = (ledc_timer_t)0,
      .freq_hz = CONFIG_GPIO_PWM_DEFAULT_FREQ_HZ,
      .clk_cfg = LEDC_USE_PLL_DIV_CLK,
      .deconfigure = false,
  };
  TOOL_CHECK_ESP_OK_OR_RETURN(ledc_timer_config(&cfg));

  esp_err_t fadeErr = ledc_fade_func_install(0);

  ledc_timer_pause(PWM_CTRL_LEDC_SPEED, (ledc_timer_t)0);
  ledc_timer_config_t undo = {
      .speed_mode = PWM_CTRL_LEDC_SPEED,
      .timer_num = (ledc_timer_t)0,
      .deconfigure = true,
  };
  ledc_timer_config(&undo);

  TOOL_CHECK_ESP_OK_OR_RETURN(fadeErr);
  return ESP_OK;
}

esp_err_t PwmCtrl_Init(void)
{
  if (ready) return ESP_OK;
  memset(ledcChannelInUse, 0, sizeof(ledcChannelInUse));
  memset(ledcTimers, 0, sizeof(ledcTimers));
  memset(slots, 0, sizeof(slots));
  for (int i = 0; i < CONFIG_GPIO_PWM_MAX_OUTPUTS; i++) {
    slots[i].ledcChannel = -1;
    slots[i].ledcTimer = -1;
  }
  TOOL_CHECK_ESP_OK_OR_RETURN(PwmCtrl_InitLedcFade());
  ready = true;
  return ESP_OK;
}

bool PwmCtrl_IsAttached(int hwPin)
{
  return PwmCtrl_FindByHw(hwPin) != NULL;
}

esp_err_t PwmCtrl_Set(int hwPin, uint32_t frequencyHz, double dutyPercent)
{
  if (!ready) return ESP_ERR_INVALID_STATE;
  if (hwPin < 0 || frequencyHz < 1) return ESP_ERR_INVALID_ARG;
  if (dutyPercent < 0.0 || dutyPercent > 100.0) return ESP_ERR_INVALID_ARG;

  PwmCtrl_Slot* slot = PwmCtrl_FindByHw(hwPin);
  if (slot != NULL && slot->ledcChannel >= 0 && slot->ledcTimer >= 0) {
    return PwmCtrl_UpdateSlot(slot, frequencyHz, dutyPercent);
  }
  if (slot == NULL) {
    slot = PwmCtrl_AllocSlot(hwPin);
    if (slot == NULL) return ESP_ERR_NO_MEM;
  }

  esp_err_t err = PwmCtrl_AttachSlot(slot, hwPin, frequencyHz, dutyPercent);
  if (err != ESP_OK) {
    PwmCtrl_FreeSlot(slot);
    return err;
  }
  return ESP_OK;
}

esp_err_t PwmCtrl_Clear(int hwPin)
{
  PwmCtrl_Slot* slot = PwmCtrl_FindByHw(hwPin);
  if (slot == NULL) return ESP_OK;
  ESP_LOGI(tag, "PWM detach: gpio=%d frequency=%u", hwPin, (unsigned)slot->frequencyHz);
  PwmCtrl_DetachSlot(slot);
  PwmCtrl_FreeSlot(slot);
  return ESP_OK;
}
