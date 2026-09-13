#ifndef LOCK_H__
#define LOCK_H__

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  LOCK_METHOD_READ = 1,
  LOCK_METHOD_WRITE = 2,
} Lock_Method;

typedef struct {
  int pin;
  uint8_t methods; /* bit flags LOCK_METHOD_* */
} Lock_Resource;

typedef struct {
  char id[32];
  int64_t expiresAtUs;
  Lock_Resource resources[8];
  size_t resourceCount;
  bool used;
} Lock_Entry;

esp_err_t Lock_Init(void);
void Lock_SweepExpired(void);

esp_err_t Lock_Create(const Lock_Resource* resources, size_t count, Lock_Entry* out);
esp_err_t Lock_Renew(const char* id, Lock_Entry* out);
esp_err_t Lock_Delete(const char* id); /* ESP_OK even if missing */

/**
 * Check GPIO access for pin+methodBits.
 * Returns:
 *  ESP_OK — allowed
 *  ESP_ERR_INVALID_STATE — 423 (known other holder)
 *  ESP_ERR_NOT_FOUND — 412 (missing or unknown X-Lock-Id when required / presented)
 */
esp_err_t Lock_CheckAccess(int pin, uint8_t methodBits, const char* xLockId);

/** Touch TTL for a valid X-Lock-Id after successful request. */
void Lock_Touch(const char* xLockId);

bool Lock_Get(const char* id, Lock_Entry* out);
int64_t Lock_NowUs(void);

#endif
