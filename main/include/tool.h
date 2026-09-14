/**
 * @name Tool macros
 * @file tool.h
 * @author xqe2011
 */
#ifndef TOOL_H__
#define TOOL_H__

#include <esp_err.h>
#include <esp_log.h>

#define TOOL_CHECK_ESP_OK_OR_LOG_RETURN(expression, message, ...) \
  if ((expression) != ESP_OK) {                                   \
    ESP_LOGW(tag, message, ##__VA_ARGS__);                        \
    return ESP_FAIL;                                              \
  }

#define TOOL_CHECK_ESP_OK_OR_RETURN(expression) \
  if ((expression) != ESP_OK) return ESP_FAIL;

#define TOOL_CHECK_OR_LOG_RETURN(expression, message, ...) \
  if (expression) {                                        \
    ESP_LOGE(tag, message, ##__VA_ARGS__);                  \
    return ESP_FAIL;                                       \
  }

#define TOOL_GET_ARRAY_LENGTH(arr) (sizeof(arr) / sizeof((arr)[0]))

#define TOOL_MIN(a, b) (((a) < (b)) ? (a) : (b))

/** Tool-call trace: TOOL_CALL_LOG("mcp %s", name) -> "Tool mcp set_pin_levels" */
#define TOOL_CALL_LOG(fmt, ...) ESP_LOGI("SAIHUB-Tool", "Tool " fmt, ##__VA_ARGS__)

#define TOOL_EXECUTE_CALLBACKS(callbacks, ...)                       \
  for (size_t i = 0; i < TOOL_GET_ARRAY_LENGTH(callbacks); i++) {    \
    if (callbacks[i] != NULL) callbacks[i](__VA_ARGS__);             \
  }

#define TOOL_REGISTER_CALLBACK(callbacks, callback, logName)         \
  for (size_t i = 0; i < TOOL_GET_ARRAY_LENGTH(callbacks); i++) {    \
    if (callbacks[i] == NULL) {                                      \
      callbacks[i] = callback;                                       \
      return ESP_OK;                                                 \
    }                                                                \
  }                                                                  \
  ESP_LOGE(tag, "Failed to register %s callback!", logName);         \
  return ESP_FAIL;

#endif
