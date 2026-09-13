/**
 * @name Lock module
 * @file lock.h
 * @author xqe2011
 */
#ifndef LOCK_H__
#define LOCK_H__

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOCK_MAX_RESOURCES 8

typedef enum {
  LOCK_METHOD_READ = 1,
  LOCK_METHOD_WRITE = 2,
} Lock_Method;

typedef enum {
  LOCK_KIND_GPIO = 0,
  LOCK_KIND_POWER_3V3,
  LOCK_KIND_POWER_5V,
} Lock_Kind;

typedef struct {
  Lock_Kind kind;
  int pin;       /* valid when kind == LOCK_KIND_GPIO */
  uint8_t methods; /* bit flags LOCK_METHOD_* */
} Lock_Resource;

typedef struct {
  char id[32];
  int64_t expiresAtUs;
  Lock_Resource resources[LOCK_MAX_RESOURCES];
  size_t resourceCount;
  bool used;
} Lock_Entry;

esp_err_t Lock_Init(void);
void Lock_SweepExpired(void);

esp_err_t Lock_Create(const Lock_Resource* resources, size_t count, Lock_Entry* out);
esp_err_t Lock_Renew(const char* id, Lock_Entry* out);
esp_err_t Lock_Delete(const char* id); /* ESP_OK even if missing */

/**
 * Check access for a lockable resource + methodBits.
 * Returns:
 *  ESP_OK — allowed
 *  ESP_ERR_INVALID_STATE — 423 (known other holder)
 *  ESP_ERR_NOT_FOUND — 412 (missing or unknown X-Lock-Id when required / presented)
 */
esp_err_t Lock_CheckAccess(Lock_Kind kind, int pin, uint8_t methodBits, const char* xLockId);

/** Touch TTL for a valid X-Lock-Id after successful request. */
void Lock_Touch(const char* xLockId);

bool Lock_Get(const char* id, Lock_Entry* out);
int64_t Lock_NowUs(void);

const char* Lock_KindToString(Lock_Kind kind);
bool Lock_PowerFromString(const char* s, Lock_Kind* out);

#endif
