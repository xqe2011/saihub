/**
 * @name Lock module
 * @file lock.c
 * @author xqe2011
 */
#include "lock.h"

#include "config.h"
#include "tool.h"

#include <esp_log.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

static const char* tag = "SAIHUB-Lock";
static Lock_Entry locks[CONFIG_LOCK_MAX_COUNT];
static SemaphoreHandle_t mutex;

int64_t Lock_NowUs(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
}

const char* Lock_KindToString(Lock_Kind kind)
{
  switch (kind) {
    case LOCK_KIND_GPIO:
      return "gpio";
    case LOCK_KIND_POWER_3V3:
      return "3v3";
    case LOCK_KIND_POWER_5V:
      return "5v";
    default:
      return "gpio";
  }
}

bool Lock_PowerFromString(const char* s, Lock_Kind* out)
{
  if (s == NULL || out == NULL) return false;
  if (strcmp(s, "3v3") == 0) {
    *out = LOCK_KIND_POWER_3V3;
    return true;
  }
  if (strcmp(s, "5v") == 0) {
    *out = LOCK_KIND_POWER_5V;
    return true;
  }
  return false;
}

static void Lock_MakeId(char* out, size_t outLen)
{
  uint32_t a = esp_random();
  uint32_t b = esp_random();
  snprintf(out, outLen, "%08lx%08lx", (unsigned long)a, (unsigned long)b);
}

esp_err_t Lock_Init(void)
{
  memset(locks, 0, sizeof(locks));
  mutex = xSemaphoreCreateMutex();
  TOOL_CHECK_OR_LOG_RETURN(mutex == NULL, "mutex create failed");
  return ESP_OK;
}

void Lock_SweepExpired(void)
{
  int64_t now = Lock_NowUs();
  xSemaphoreTake(mutex, portMAX_DELAY);
  for (size_t i = 0; i < CONFIG_LOCK_MAX_COUNT; i++) {
    if (locks[i].used && locks[i].expiresAtUs <= now) {
      locks[i].used = false;
    }
  }
  xSemaphoreGive(mutex);
}

static Lock_Entry* Lock_FindUnlocked(void)
{
  for (size_t i = 0; i < CONFIG_LOCK_MAX_COUNT; i++) {
    if (!locks[i].used) return &locks[i];
  }
  return NULL;
}

static Lock_Entry* Lock_FindById(const char* id)
{
  if (id == NULL || id[0] == '\0') return NULL;
  int64_t now = Lock_NowUs();
  for (size_t i = 0; i < CONFIG_LOCK_MAX_COUNT; i++) {
    if (locks[i].used && strcmp(locks[i].id, id) == 0) {
      if (locks[i].expiresAtUs <= now) {
        locks[i].used = false;
        return NULL;
      }
      return &locks[i];
    }
  }
  return NULL;
}

static bool Lock_ResourceEqualKey(const Lock_Resource* a, const Lock_Resource* b)
{
  if (a->kind != b->kind) return false;
  if (a->kind == LOCK_KIND_GPIO) return a->pin == b->pin;
  return true;
}

static bool Lock_Overlaps(const Lock_Entry* existing, const Lock_Resource* resources, size_t count)
{
  for (size_t i = 0; i < existing->resourceCount; i++) {
    for (size_t j = 0; j < count; j++) {
      if (Lock_ResourceEqualKey(&existing->resources[i], &resources[j]) &&
          (existing->resources[i].methods & resources[j].methods) != 0) {
        return true;
      }
    }
  }
  return false;
}

static const Lock_Entry* Lock_FindHolder(Lock_Kind kind, int pin, uint8_t methodBits)
{
  int64_t now = Lock_NowUs();
  for (size_t i = 0; i < CONFIG_LOCK_MAX_COUNT; i++) {
    if (!locks[i].used || locks[i].expiresAtUs <= now) continue;
    for (size_t r = 0; r < locks[i].resourceCount; r++) {
      const Lock_Resource* res = &locks[i].resources[r];
      if (res->kind != kind) continue;
      if (kind == LOCK_KIND_GPIO && res->pin != pin) continue;
      if ((res->methods & methodBits) != 0) {
        return &locks[i];
      }
    }
  }
  return NULL;
}

esp_err_t Lock_Create(const Lock_Resource* resources, size_t count, Lock_Entry* out)
{
  if (resources == NULL || count == 0 || count > CONFIG_LOCK_MAX_RESOURCES || out == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  xSemaphoreTake(mutex, portMAX_DELAY);
  int64_t now = Lock_NowUs();
  for (size_t i = 0; i < CONFIG_LOCK_MAX_COUNT; i++) {
    if (locks[i].used && locks[i].expiresAtUs <= now) {
      locks[i].used = false;
    }
  }
  for (size_t i = 0; i < CONFIG_LOCK_MAX_COUNT; i++) {
    if (locks[i].used && Lock_Overlaps(&locks[i], resources, count)) {
      xSemaphoreGive(mutex);
      return ESP_ERR_INVALID_STATE; /* 409 */
    }
  }

  Lock_Entry* slot = Lock_FindUnlocked();
  if (slot == NULL) {
    xSemaphoreGive(mutex);
    return ESP_ERR_NO_MEM;
  }

  memset(slot, 0, sizeof(*slot));
  Lock_MakeId(slot->id, sizeof(slot->id));
  slot->expiresAtUs = now + (int64_t)CONFIG_LOCK_TTL_US;
  slot->resourceCount = count;
  memcpy(slot->resources, resources, count * sizeof(Lock_Resource));
  slot->used = true;
  *out = *slot;
  xSemaphoreGive(mutex);
  return ESP_OK;
}

esp_err_t Lock_Renew(const char* id, Lock_Entry* out)
{
  xSemaphoreTake(mutex, portMAX_DELAY);
  Lock_Entry* entry = Lock_FindById(id);
  if (entry == NULL) {
    xSemaphoreGive(mutex);
    return ESP_ERR_NOT_FOUND;
  }
  entry->expiresAtUs = Lock_NowUs() + (int64_t)CONFIG_LOCK_TTL_US;
  if (out) *out = *entry;
  xSemaphoreGive(mutex);
  return ESP_OK;
}

esp_err_t Lock_Delete(const char* id)
{
  xSemaphoreTake(mutex, portMAX_DELAY);
  Lock_Entry* entry = Lock_FindById(id);
  if (entry != NULL) {
    entry->used = false;
  }
  xSemaphoreGive(mutex);
  return ESP_OK;
}

bool Lock_Get(const char* id, Lock_Entry* out)
{
  xSemaphoreTake(mutex, portMAX_DELAY);
  Lock_Entry* entry = Lock_FindById(id);
  bool ok = entry != NULL;
  if (ok && out) *out = *entry;
  xSemaphoreGive(mutex);
  return ok;
}

esp_err_t Lock_CheckAccess(Lock_Kind kind, int pin, uint8_t methodBits, const char* xLockId)
{
  xSemaphoreTake(mutex, portMAX_DELAY);

  bool headerPresent = xLockId != NULL && xLockId[0] != '\0';
  if (headerPresent) {
    Lock_Entry* headerLock = Lock_FindById(xLockId);
    if (headerLock == NULL) {
      xSemaphoreGive(mutex);
      return ESP_ERR_NOT_FOUND; /* 412 unknown */
    }
  }

  const Lock_Entry* holder = Lock_FindHolder(kind, pin, methodBits);
  if (holder == NULL) {
    xSemaphoreGive(mutex);
    return ESP_OK; /* unlocked; optional unknown header already handled */
  }

  if (!headerPresent) {
    xSemaphoreGive(mutex);
    return ESP_ERR_NOT_FOUND; /* 412 missing */
  }

  if (strcmp(holder->id, xLockId) != 0) {
    xSemaphoreGive(mutex);
    return ESP_ERR_INVALID_STATE; /* 423 different live holder */
  }

  xSemaphoreGive(mutex);
  return ESP_OK;
}

void Lock_Touch(const char* xLockId)
{
  if (xLockId == NULL || xLockId[0] == '\0') return;
  xSemaphoreTake(mutex, portMAX_DELAY);
  Lock_Entry* entry = Lock_FindById(xLockId);
  if (entry != NULL) {
    entry->expiresAtUs = Lock_NowUs() + (int64_t)CONFIG_LOCK_TTL_US;
  }
  xSemaphoreGive(mutex);
}
