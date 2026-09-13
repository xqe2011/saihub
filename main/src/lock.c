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

static const char* tag = "Lock";
static Lock_Entry s_locks[CONFIG_LOCK_MAX_COUNT];
static SemaphoreHandle_t s_mutex;

int64_t Lock_NowUs(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
}

static void Lock_MakeId(char* out, size_t outLen)
{
  uint32_t a = esp_random();
  uint32_t b = esp_random();
  snprintf(out, outLen, "%08lx%08lx", (unsigned long)a, (unsigned long)b);
}

esp_err_t Lock_Init(void)
{
  memset(s_locks, 0, sizeof(s_locks));
  s_mutex = xSemaphoreCreateMutex();
  TOOL_CHECK_OR_LOG_RETURN(s_mutex == NULL, "mutex create failed");
  return ESP_OK;
}

void Lock_SweepExpired(void)
{
  int64_t now = Lock_NowUs();
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  for (size_t i = 0; i < CONFIG_LOCK_MAX_COUNT; i++) {
    if (s_locks[i].used && s_locks[i].expiresAtUs <= now) {
      s_locks[i].used = false;
    }
  }
  xSemaphoreGive(s_mutex);
}

static Lock_Entry* Lock_FindUnlocked(void)
{
  for (size_t i = 0; i < CONFIG_LOCK_MAX_COUNT; i++) {
    if (!s_locks[i].used) return &s_locks[i];
  }
  return NULL;
}

static Lock_Entry* Lock_FindById(const char* id)
{
  if (id == NULL || id[0] == '\0') return NULL;
  int64_t now = Lock_NowUs();
  for (size_t i = 0; i < CONFIG_LOCK_MAX_COUNT; i++) {
    if (s_locks[i].used && strcmp(s_locks[i].id, id) == 0) {
      if (s_locks[i].expiresAtUs <= now) {
        s_locks[i].used = false;
        return NULL;
      }
      return &s_locks[i];
    }
  }
  return NULL;
}

static bool Lock_Overlaps(const Lock_Entry* existing, const Lock_Resource* resources, size_t count)
{
  for (size_t i = 0; i < existing->resourceCount; i++) {
    for (size_t j = 0; j < count; j++) {
      if (existing->resources[i].pin == resources[j].pin &&
          (existing->resources[i].methods & resources[j].methods) != 0) {
        return true;
      }
    }
  }
  return false;
}

static const Lock_Entry* Lock_FindHolder(int pin, uint8_t methodBits)
{
  int64_t now = Lock_NowUs();
  for (size_t i = 0; i < CONFIG_LOCK_MAX_COUNT; i++) {
    if (!s_locks[i].used || s_locks[i].expiresAtUs <= now) continue;
    for (size_t r = 0; r < s_locks[i].resourceCount; r++) {
      if (s_locks[i].resources[r].pin == pin && (s_locks[i].resources[r].methods & methodBits) != 0) {
        return &s_locks[i];
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

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  int64_t now = Lock_NowUs();
  for (size_t i = 0; i < CONFIG_LOCK_MAX_COUNT; i++) {
    if (s_locks[i].used && s_locks[i].expiresAtUs <= now) {
      s_locks[i].used = false;
    }
  }
  for (size_t i = 0; i < CONFIG_LOCK_MAX_COUNT; i++) {
    if (s_locks[i].used && Lock_Overlaps(&s_locks[i], resources, count)) {
      xSemaphoreGive(s_mutex);
      return ESP_ERR_INVALID_STATE; /* 409 */
    }
  }

  Lock_Entry* slot = Lock_FindUnlocked();
  if (slot == NULL) {
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NO_MEM;
  }

  memset(slot, 0, sizeof(*slot));
  Lock_MakeId(slot->id, sizeof(slot->id));
  slot->expiresAtUs = now + (int64_t)CONFIG_LOCK_TTL_US;
  slot->resourceCount = count;
  memcpy(slot->resources, resources, count * sizeof(Lock_Resource));
  slot->used = true;
  *out = *slot;
  xSemaphoreGive(s_mutex);
  return ESP_OK;
}

esp_err_t Lock_Renew(const char* id, Lock_Entry* out)
{
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  Lock_Entry* entry = Lock_FindById(id);
  if (entry == NULL) {
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
  }
  entry->expiresAtUs = Lock_NowUs() + (int64_t)CONFIG_LOCK_TTL_US;
  if (out) *out = *entry;
  xSemaphoreGive(s_mutex);
  return ESP_OK;
}

esp_err_t Lock_Delete(const char* id)
{
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  Lock_Entry* entry = Lock_FindById(id);
  if (entry != NULL) {
    entry->used = false;
  }
  xSemaphoreGive(s_mutex);
  return ESP_OK;
}

bool Lock_Get(const char* id, Lock_Entry* out)
{
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  Lock_Entry* entry = Lock_FindById(id);
  bool ok = entry != NULL;
  if (ok && out) *out = *entry;
  xSemaphoreGive(s_mutex);
  return ok;
}

esp_err_t Lock_CheckAccess(int pin, uint8_t methodBits, const char* xLockId)
{
  xSemaphoreTake(s_mutex, portMAX_DELAY);

  bool headerPresent = xLockId != NULL && xLockId[0] != '\0';
  Lock_Entry* headerLock = NULL;
  if (headerPresent) {
    headerLock = Lock_FindById(xLockId);
    if (headerLock == NULL) {
      xSemaphoreGive(s_mutex);
      return ESP_ERR_NOT_FOUND; /* 412 unknown */
    }
  }

  const Lock_Entry* holder = Lock_FindHolder(pin, methodBits);
  if (holder == NULL) {
    xSemaphoreGive(s_mutex);
    return ESP_OK; /* unlocked; optional unknown header already handled */
  }

  if (!headerPresent) {
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND; /* 412 missing */
  }

  if (strcmp(holder->id, xLockId) != 0) {
    xSemaphoreGive(s_mutex);
    return ESP_ERR_INVALID_STATE; /* 423 different live holder */
  }

  xSemaphoreGive(s_mutex);
  return ESP_OK;
}

void Lock_Touch(const char* xLockId)
{
  if (xLockId == NULL || xLockId[0] == '\0') return;
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  Lock_Entry* entry = Lock_FindById(xLockId);
  if (entry != NULL) {
    entry->expiresAtUs = Lock_NowUs() + (int64_t)CONFIG_LOCK_TTL_US;
  }
  xSemaphoreGive(s_mutex);
}
