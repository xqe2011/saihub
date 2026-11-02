/**
 * @name Tool macros
 * @file tool.h
 * @author xqe2011
 */
#ifndef TOOL_H__
#define TOOL_H__

#include <esp_err.h>
#include <esp_log.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/** Byte ring. `head` is the next byte to read, `tail` is the next byte to write. */
typedef struct {
  uint8_t* data;
  size_t cap;
  size_t head;
  size_t tail;
  size_t used;
  bool owned;
} Tool_Ring;

/** Attach an external buffer. The ring does not free it. */
void Tool_RingInit(Tool_Ring* ring, uint8_t* data, size_t cap);
/** Allocate `cap` bytes and take ownership of them. */
bool Tool_RingCreate(Tool_Ring* ring, size_t cap);
/** Free a buffer owned by the ring and reset the ring. */
void Tool_RingRelease(Tool_Ring* ring);
void Tool_RingClear(Tool_Ring* ring);
size_t Tool_RingLen(const Tool_Ring* ring);
size_t Tool_RingFree(const Tool_Ring* ring);
/** Copy bytes in. Fails when `len` exceeds the free space; the ring is left unchanged. */
bool Tool_RingWrite(Tool_Ring* ring, const void* src, size_t len);
/** Copy bytes out and advance the read head. */
bool Tool_RingRead(Tool_Ring* ring, void* dst, size_t len);
/** Copy bytes out without advancing the read head. */
bool Tool_RingPeek(const Tool_Ring* ring, void* dst, size_t len);
/** Discard bytes from the read head. */
bool Tool_RingDrop(Tool_Ring* ring, size_t len);

#endif
