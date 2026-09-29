/**
 * @name Onboard buzzer
 * @file buzzer.c
 * @author xqe2011
 */
#include "buzzer.h"

#include "config.h"
#include "tool.h"

#include <driver/rmt_encoder.h>
#include <driver/rmt_tx.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <soc/soc_caps.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char* tag = "SAIHub-Buzzer";

#define BUZZER_RMT_RESOLUTION_HZ 1000000U
#define BUZZER_RMT_DURATION_MAX 32767U

_Static_assert(CONFIG_BUZZER_FREQ_HZ > 0, "buzzer frequency must be positive");
_Static_assert(CONFIG_BUZZER_DUTY_PERCENT > 0 && CONFIG_BUZZER_DUTY_PERCENT < 100, "buzzer duty must be 1-99");
_Static_assert((BUZZER_RMT_RESOLUTION_HZ / CONFIG_BUZZER_FREQ_HZ) >= 2, "buzzer period is too short");
_Static_assert((BUZZER_RMT_RESOLUTION_HZ / CONFIG_BUZZER_FREQ_HZ) <= (2 * BUZZER_RMT_DURATION_MAX),
               "buzzer period is too long");
_Static_assert(CONFIG_BUZZER_WPM > 0 && CONFIG_BUZZER_WPM <= 1200U, "buzzer WPM must be 1-1200");
_Static_assert(CONFIG_BUZZER_MAX_SEQUENCE > 0, "buzzer sequence limit");

static rmt_channel_handle_t txChannel;
static rmt_encoder_handle_t copyEncoder;
static rmt_symbol_word_t periodSymbol;
static SemaphoreHandle_t playMutex;
static bool ready;

static void Buzzer_SetReason(char* reason, size_t reasonLen, const char* text)
{
  if (reason == NULL || reasonLen == 0) return;
  snprintf(reason, reasonLen, "%s", text ? text : "error");
}

static bool Buzzer_CheckSequence(const char* sequence, char* reason, size_t reasonLen)
{
  if (sequence == NULL || sequence[0] == '\0') {
    Buzzer_SetReason(reason, reasonLen, "sequence must be a non-empty string of . and -.");
    return false;
  }
  size_t len = strlen(sequence);
  if (len > CONFIG_BUZZER_MAX_SEQUENCE) {
    char buf[96];
    snprintf(buf, sizeof(buf), "sequence must be at most %u characters.", (unsigned)CONFIG_BUZZER_MAX_SEQUENCE);
    Buzzer_SetReason(reason, reasonLen, buf);
    return false;
  }
  bool hasBeep = false;
  for (size_t i = 0; i < len; i++) {
    char c = sequence[i];
    if (c == '.' || c == '-') {
      hasBeep = true;
      continue;
    }
    if (c == ' ') continue;
    Buzzer_SetReason(reason, reasonLen, "sequence may only contain ., -, and spaces.");
    return false;
  }
  if (!hasBeep) {
    Buzzer_SetReason(reason, reasonLen, "sequence must include at least one . or -.");
    return false;
  }
  return true;
}

/* PARIS Morse: 50 units per word, so one dit is 1.2 s / WPM. */
static uint64_t Buzzer_DitUs(void)
{
  return 1200000ULL / (uint64_t)CONFIG_BUZZER_WPM;
}

static void Buzzer_DelayUs(uint64_t us)
{
  if (us == 0) return;
  uint32_t ms = (uint32_t)((us + 999ULL) / 1000ULL);
  vTaskDelay(pdMS_TO_TICKS(ms < 1 ? 1 : ms));
}

static esp_err_t Buzzer_Tone(uint64_t durationUs)
{
  if (txChannel == NULL || copyEncoder == NULL) return ESP_ERR_INVALID_STATE;
  uint64_t loops = (durationUs * (uint64_t)CONFIG_BUZZER_FREQ_HZ + 500000ULL) / 1000000ULL;
  if (loops == 0) loops = 1;
  if (loops > (uint64_t)INT32_MAX) loops = (uint64_t)INT32_MAX;
  rmt_transmit_config_t txConfig = {
      .loop_count = (int)loops,
      .flags.eot_level = 0,
  };
  TOOL_CHECK_ESP_OK_OR_RETURN(rmt_transmit(txChannel, copyEncoder, &periodSymbol, sizeof(periodSymbol), &txConfig));
  TOOL_CHECK_ESP_OK_OR_RETURN(rmt_tx_wait_all_done(txChannel, -1));
  return ESP_OK;
}

uint64_t Buzzer_SequenceDurationUs(const char* sequence)
{
  if (!Buzzer_CheckSequence(sequence, NULL, 0)) return 0;
  uint64_t ditUs = Buzzer_DitUs();
  uint64_t total = 0;
  bool needGap = false;
  for (const char* p = sequence; *p; p++) {
    if (*p == ' ') {
      total += 3ULL * ditUs;
      continue;
    }
    if (needGap) total += ditUs;
    needGap = true;
    total += (*p == '-') ? (3ULL * ditUs) : ditUs;
  }
  return total;
}

esp_err_t Buzzer_Play(const char* sequence, char* reason, size_t reasonLen)
{
  if (!Buzzer_CheckSequence(sequence, reason, reasonLen)) {
    ESP_LOGW(tag, "play failed sequence=%s: invalid sequence", sequence ? sequence : "");
    return ESP_ERR_INVALID_ARG;
  }
  if (!ready || playMutex == NULL) {
    Buzzer_SetReason(reason, reasonLen, "internal");
    ESP_LOGW(tag, "play failed sequence=%s: not ready", sequence);
    return ESP_FAIL;
  }
  if (xSemaphoreTake(playMutex, 0) != pdTRUE) {
    Buzzer_SetReason(reason, reasonLen, "The buzzer is already playing. Wait for it to finish.");
    ESP_LOGW(tag, "play failed sequence=%s: busy", sequence);
    return ESP_ERR_INVALID_STATE;
  }

  ESP_LOGI(tag, "play sequence=%s wpm=%u", sequence, (unsigned)CONFIG_BUZZER_WPM);
  uint64_t ditUs = Buzzer_DitUs();
  esp_err_t ret = ESP_OK;
  bool needGap = false;
  for (const char* p = sequence; *p && ret == ESP_OK; p++) {
    if (*p == ' ') {
      Buzzer_DelayUs(3ULL * ditUs);
      continue;
    }
    if (needGap) Buzzer_DelayUs(ditUs);
    needGap = true;
    ret = Buzzer_Tone((*p == '-') ? (3ULL * ditUs) : ditUs);
  }

  xSemaphoreGive(playMutex);
  if (ret != ESP_OK) {
    Buzzer_SetReason(reason, reasonLen, "internal");
    ESP_LOGW(tag, "play failed sequence=%s: internal", sequence);
    return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t Buzzer_Init(void)
{
  playMutex = xSemaphoreCreateMutex();
  TOOL_CHECK_OR_LOG_RETURN(playMutex == NULL, "buzzer mutex create failed");

  uint32_t periodTicks = BUZZER_RMT_RESOLUTION_HZ / CONFIG_BUZZER_FREQ_HZ;
  uint32_t highTicks = periodTicks * CONFIG_BUZZER_DUTY_PERCENT / 100U;
  if (highTicks == 0) highTicks = 1;
  if (highTicks >= periodTicks) highTicks = periodTicks - 1;
  uint32_t lowTicks = periodTicks - highTicks;
  if (highTicks > BUZZER_RMT_DURATION_MAX || lowTicks > BUZZER_RMT_DURATION_MAX || lowTicks == 0) {
    ESP_LOGE(tag, "buzzer period does not fit RMT durations");
    return ESP_ERR_INVALID_ARG;
  }
  periodSymbol.level0 = 1;
  periodSymbol.duration0 = (uint16_t)highTicks;
  periodSymbol.level1 = 0;
  periodSymbol.duration1 = (uint16_t)lowTicks;

  rmt_tx_channel_config_t txConfig = {
      .gpio_num = CONFIG_BUZZER_PIN,
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = BUZZER_RMT_RESOLUTION_HZ,
      .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL,
      .trans_queue_depth = 4,
      .flags.init_level = 0,
  };
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(rmt_new_tx_channel(&txConfig, &txChannel), "buzzer rmt channel failed");

  rmt_copy_encoder_config_t copyConfig = {};
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(rmt_new_copy_encoder(&copyConfig, &copyEncoder), "buzzer rmt encoder failed");
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(rmt_enable(txChannel), "buzzer rmt enable failed");

  ready = true;
  ESP_LOGI(tag, "buzzer ready pin=%d freq=%u duty=%u wpm=%u", CONFIG_BUZZER_PIN,
           (unsigned)CONFIG_BUZZER_FREQ_HZ, (unsigned)CONFIG_BUZZER_DUTY_PERCENT, (unsigned)CONFIG_BUZZER_WPM);
  return ESP_OK;
}
