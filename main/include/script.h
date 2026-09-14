/**
 * @name On-device Lua script runner
 * @file script.h
 * @author xqe2011
 */
#ifndef SCRIPT_H__
#define SCRIPT_H__

#include <cJSON.h>
#include <esp_err.h>
#include <esp_http_server.h>
#include <stdbool.h>
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

/** True while a script is running or reserved for a run. */
bool Script_IsBusy(void);

/**
 * Run a Lua script on the dedicated script task.
 * maxCalls/timeoutUs of 0 mean use config.h ceilings.
 * On success: result/output owned by caller; use Script_ResultFree.
 * On failure: reason and httpStatus filled; result/output may be NULL.
 */
Script_Status Script_Run(const char* script, uint32_t maxCalls, uint64_t timeoutUs, const char* defaultLockId,
                         Script_Result* out);

/**
 * Called by Script_RunAsync when the run finishes (or immediately if busy / setup fails).
 * Must send the HTTP response and call Script_ResultFree(result).
 * On the async path, req is an async copy; the helper completes it after this returns.
 */
typedef void (*Script_HttpRespondFn)(httpd_req_t* req, Script_Status status, Script_Result* result, void* userCtx);

/**
 * Offload Script_Run so the HTTP server can accept other requests.
 * If already busy: invoke respond on req with SCRIPT_ERR_BUSY (sync), no waiter.
 * Otherwise: detach req, spawn a short-lived waiter, return ESP_OK; respond runs later.
 * Always invokes respond exactly once (including setup failures).
 */
esp_err_t Script_RunAsync(httpd_req_t* req, const char* script, uint32_t maxCalls, uint64_t timeoutUs,
                          const char* defaultLockId, Script_HttpRespondFn respond, void* userCtx);

void Script_ResultFree(Script_Result* out);

#endif
