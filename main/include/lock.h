/**
 * @name Lock module
 * @file lock.h
 * @author xqe2011
 */
#ifndef LOCK_H__
#define LOCK_H__

#include "config.h"

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  LOCK_METHOD_READ = 1,
  LOCK_METHOD_WRITE = 2,
} Lock_Method;

typedef enum {
  LOCK_KIND_GPIO = 0,
  LOCK_KIND_POWER_3V3,
  LOCK_KIND_POWER_5V,
  LOCK_KIND_UART,
} Lock_Kind;

typedef struct {
  Lock_Kind kind;
  int pin;         /* GPIO logical pin, or UART id when kind == LOCK_KIND_UART */
  uint8_t methods; /* bit flags LOCK_METHOD_* */
} Lock_Resource;

typedef struct {
  char id[32];
  int64_t expiresAtUs;
  Lock_Resource resources[CONFIG_LOCK_MAX_RESOURCES];
  size_t resourceCount;
  bool used;
} Lock_Entry;

/** Filled when Lock_Create fails with ESP_ERR_INVALID_STATE. */
typedef struct {
  bool isPeripheral;
  Lock_Kind kind;
  int pin;
  char peripheralLabel[48]; /* e.g. "UART 0" */
} Lock_Conflict;

esp_err_t Lock_Init(void);
void Lock_SweepExpired(void);

esp_err_t Lock_Create(const Lock_Resource* resources, size_t count, Lock_Entry* out, Lock_Conflict* conflictOut);
esp_err_t Lock_Renew(const char* id, Lock_Entry* out);
esp_err_t Lock_Delete(const char* id); /* ESP_OK even if missing */

/**
 * Check access for a lockable resource + methodBits.
 * Returns:
 *  ESP_OK — allowed
 *  ESP_ERR_INVALID_STATE — 423 (known other user lock holder)
 *  ESP_ERR_NOT_ALLOWED — 423 (peripheral hold; peripheralLabelOut filled when non-NULL)
 *  ESP_ERR_NOT_FOUND — 412 (missing or unknown X-Lock-Id when required / presented)
 */
esp_err_t Lock_CheckAccess(Lock_Kind kind, int pin, uint8_t methodBits, const char* xLockId, char* peripheralLabelOut,
                           size_t peripheralLabelLen);

/** Touch TTL for a valid X-Lock-Id after successful request. */
void Lock_Touch(const char* xLockId);

bool Lock_Get(const char* id, Lock_Entry* out);
int64_t Lock_NowUs(void);

const char* Lock_KindToString(Lock_Kind kind);
bool Lock_PowerFromString(const char* s, Lock_Kind* out);
bool Lock_IsValidUartId(int id);

/**
 * Acquire a no-TTL peripheral hold over resources. owner e.g. "uart:0".
 * Display label for client reasons is derived (uart:0 -> "UART 0").
 * Returns ESP_ERR_INVALID_STATE on overlap with user or other peripheral holds.
 */
esp_err_t Lock_AcquirePeripheral(const char* owner, const Lock_Resource* resources, size_t count,
                                 Lock_Conflict* conflictOut);
/** Release peripheral hold by owner. Idempotent. */
esp_err_t Lock_ReleasePeripheral(const char* owner);

#endif
