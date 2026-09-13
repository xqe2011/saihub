/**
 * @file tool.h
 * Validation and helper macros.
 */
#ifndef TOOL_H__
#define TOOL_H__

#include <esp_log.h>
#include <esp_err.h>

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

#endif
