/**
 * @name On-device Lua script runner
 * @file script.h
 * @author xqe2011
 */
#ifndef SCRIPT_H__
#define SCRIPT_H__

#include <cJSON.h>
#include <esp_err.h>
#include <stdint.h>

typedef enum {
  SCRIPT_OK = 0,
  SCRIPT_ERR_BUSY,
  SCRIPT_ERR_BAD_ARG,
  SCRIPT_ERR_TIMEOUT,
  SCRIPT_ERR_RUNTIME,
  SCRIPT_ERR_INTERNAL,
} Script_Status;

typedef struct {
  cJSON* result; /* caller-owned; may be NULL */
  char* output;  /* caller-owned; may be NULL */
  uint32_t calls;
  uint64_t elapsedUs;
  char reason[256];
  int httpStatus; /* suggested REST status for failures */
} Script_Result;

esp_err_t Script_Init(void);

/**
 * Run a Lua script on the dedicated script task.
 * maxCalls/timeoutUs of 0 mean use config.h ceilings.
 * On success: result/output owned by caller; use Script_ResultFree.
 * On failure: reason and httpStatus filled; result/output may be NULL.
 */
Script_Status Script_Run(const char* script, uint32_t maxCalls, uint64_t timeoutUs, const char* defaultLockId,
                         Script_Result* out);

void Script_ResultFree(Script_Result* out);

#endif
