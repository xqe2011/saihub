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

static const char* tag = "SAIHub-Lock";

typedef struct {
  char owner[32];
  char label[48];
  Lock_Resource resources[CONFIG_LOCK_MAX_RESOURCES];
  size_t resourceCount;
  bool used;
} Lock_PeripheralHold;

static Lock_Entry locks[CONFIG_LOCK_MAX_COUNT];
static Lock_PeripheralHold peripherals[CONFIG_LOCK_MAX_PERIPHERAL];
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
    case LOCK_KIND_UART:
      return "uart";
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

bool Lock_IsValidUartId(int id)
{
  static const int map[] = CONFIG_UART_ID_TO_NUM;
  return id >= 0 && id < (int)TOOL_GET_ARRAY_LENGTH(map);
}

static void Lock_MakeId(char* out, size_t outLen)
{
  uint32_t a = esp_random();
  uint32_t b = esp_random();
  snprintf(out, outLen, "%08lx%08lx", (unsigned long)a, (unsigned long)b);
}

static void Lock_OwnerToLabel(const char* owner, char* out, size_t outLen)
{
  if (owner == NULL || out == NULL || outLen == 0) return;
  unsigned uartId = 0;
  if (sscanf(owner, "uart:%u", &uartId) == 1) {
    snprintf(out, outLen, "UART %u", uartId);
    return;
  }
  snprintf(out, outLen, "%s", owner);
}

static void Lock_FillConflict(Lock_Conflict* conflictOut, bool isPeripheral, Lock_Kind kind, int pin,
                              const char* peripheralLabel)
{
  if (conflictOut == NULL) return;
  conflictOut->isPeripheral = isPeripheral;
  conflictOut->kind = kind;
  conflictOut->pin = pin;
  conflictOut->peripheralLabel[0] = '\0';
  if (peripheralLabel != NULL) {
    snprintf(conflictOut->peripheralLabel, sizeof(conflictOut->peripheralLabel), "%s", peripheralLabel);
  }
}

esp_err_t Lock_Init(void)
{
  memset(locks, 0, sizeof(locks));
  memset(peripherals, 0, sizeof(peripherals));
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

static Lock_PeripheralHold* Lock_FindPeripheralUnlocked(void)
{
  for (size_t i = 0; i < CONFIG_LOCK_MAX_PERIPHERAL; i++) {
    if (!peripherals[i].used) return &peripherals[i];
  }
  return NULL;
}

static Lock_PeripheralHold* Lock_FindPeripheralByOwner(const char* owner)
{
  if (owner == NULL || owner[0] == '\0') return NULL;
  for (size_t i = 0; i < CONFIG_LOCK_MAX_PERIPHERAL; i++) {
    if (peripherals[i].used && strcmp(peripherals[i].owner, owner) == 0) return &peripherals[i];
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

static bool Lock_ResourceNeedsPin(Lock_Kind kind)
{
  return kind == LOCK_KIND_GPIO || kind == LOCK_KIND_UART;
}

static bool Lock_ResourceEqualKey(const Lock_Resource* a, const Lock_Resource* b)
{
  if (a->kind != b->kind) return false;
  if (Lock_ResourceNeedsPin(a->kind)) return a->pin == b->pin;
  return true;
}

static bool Lock_ResourcesOverlap(const Lock_Resource* a, size_t aCount, const Lock_Resource* b, size_t bCount,
                                  Lock_Kind* outKind, int* outPin)
{
  for (size_t i = 0; i < aCount; i++) {
    for (size_t j = 0; j < bCount; j++) {
      if (Lock_ResourceEqualKey(&a[i], &b[j]) && (a[i].methods & b[j].methods) != 0) {
        if (outKind) *outKind = a[i].kind;
        if (outPin) *outPin = a[i].pin;
        return true;
      }
    }
  }
  return false;
}

static bool Lock_Overlaps(const Lock_Entry* existing, const Lock_Resource* resources, size_t count, Lock_Kind* outKind,
                          int* outPin)
{
  return Lock_ResourcesOverlap(existing->resources, existing->resourceCount, resources, count, outKind, outPin);
}

static bool Lock_OverlapsPeripheral(const Lock_PeripheralHold* hold, const Lock_Resource* resources, size_t count,
                                    Lock_Kind* outKind, int* outPin)
{
  return Lock_ResourcesOverlap(hold->resources, hold->resourceCount, resources, count, outKind, outPin);
}

static const Lock_Entry* Lock_FindHolder(Lock_Kind kind, int pin, uint8_t methodBits)
{
  int64_t now = Lock_NowUs();
  for (size_t i = 0; i < CONFIG_LOCK_MAX_COUNT; i++) {
    if (!locks[i].used || locks[i].expiresAtUs <= now) continue;
    for (size_t r = 0; r < locks[i].resourceCount; r++) {
      const Lock_Resource* res = &locks[i].resources[r];
      if (res->kind != kind) continue;
      if (Lock_ResourceNeedsPin(kind) && res->pin != pin) continue;
      if ((res->methods & methodBits) != 0) {
        return &locks[i];
      }
    }
  }
  return NULL;
}

static const Lock_PeripheralHold* Lock_FindPeripheralHolder(Lock_Kind kind, int pin, uint8_t methodBits)
{
  for (size_t i = 0; i < CONFIG_LOCK_MAX_PERIPHERAL; i++) {
    if (!peripherals[i].used) continue;
    for (size_t r = 0; r < peripherals[i].resourceCount; r++) {
      const Lock_Resource* res = &peripherals[i].resources[r];
      if (res->kind != kind) continue;
      if (Lock_ResourceNeedsPin(kind) && res->pin != pin) continue;
      if ((res->methods & methodBits) != 0) {
        return &peripherals[i];
      }
    }
  }
  return NULL;
}

esp_err_t Lock_Create(const Lock_Resource* resources, size_t count, Lock_Entry* out, Lock_Conflict* conflictOut)
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

  for (size_t i = 0; i < CONFIG_LOCK_MAX_PERIPHERAL; i++) {
    if (!peripherals[i].used) continue;
    Lock_Kind kind = LOCK_KIND_GPIO;
    int pin = 0;
    if (Lock_OverlapsPeripheral(&peripherals[i], resources, count, &kind, &pin)) {
      Lock_FillConflict(conflictOut, true, kind, pin, peripherals[i].label);
      xSemaphoreGive(mutex);
      return ESP_ERR_INVALID_STATE;
    }
  }

  for (size_t i = 0; i < CONFIG_LOCK_MAX_COUNT; i++) {
    if (!locks[i].used) continue;
    Lock_Kind kind = LOCK_KIND_GPIO;
    int pin = 0;
    if (Lock_Overlaps(&locks[i], resources, count, &kind, &pin)) {
      Lock_FillConflict(conflictOut, false, kind, pin, NULL);
      xSemaphoreGive(mutex);
      return ESP_ERR_INVALID_STATE;
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

esp_err_t Lock_CheckAccess(Lock_Kind kind, int pin, uint8_t methodBits, const char* xLockId, char* peripheralLabelOut,
                           size_t peripheralLabelLen)
{
  xSemaphoreTake(mutex, portMAX_DELAY);

  if (peripheralLabelOut != NULL && peripheralLabelLen > 0) peripheralLabelOut[0] = '\0';

  bool headerPresent = xLockId != NULL && xLockId[0] != '\0';
  if (headerPresent) {
    Lock_Entry* headerLock = Lock_FindById(xLockId);
    if (headerLock == NULL) {
      xSemaphoreGive(mutex);
      return ESP_ERR_NOT_FOUND; /* 412 unknown */
    }
  }

  const Lock_PeripheralHold* periph = Lock_FindPeripheralHolder(kind, pin, methodBits);
  if (periph != NULL) {
    if (peripheralLabelOut != NULL && peripheralLabelLen > 0) {
      snprintf(peripheralLabelOut, peripheralLabelLen, "%s", periph->label);
    }
    xSemaphoreGive(mutex);
    return ESP_ERR_NOT_ALLOWED; /* 423 peripheral */
  }

  const Lock_Entry* holder = Lock_FindHolder(kind, pin, methodBits);
  if (holder == NULL) {
    xSemaphoreGive(mutex);
    return ESP_OK;
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

esp_err_t Lock_AcquirePeripheral(const char* owner, const Lock_Resource* resources, size_t count,
                                 Lock_Conflict* conflictOut)
{
  if (owner == NULL || owner[0] == '\0' || resources == NULL || count == 0 || count > CONFIG_LOCK_MAX_RESOURCES) {
    return ESP_ERR_INVALID_ARG;
  }

  xSemaphoreTake(mutex, portMAX_DELAY);
  int64_t now = Lock_NowUs();
  for (size_t i = 0; i < CONFIG_LOCK_MAX_COUNT; i++) {
    if (locks[i].used && locks[i].expiresAtUs <= now) {
      locks[i].used = false;
    }
  }

  Lock_PeripheralHold* existing = Lock_FindPeripheralByOwner(owner);
  /* Same owner re-acquiring: check overlap excluding self, then replace. */
  for (size_t i = 0; i < CONFIG_LOCK_MAX_PERIPHERAL; i++) {
    if (!peripherals[i].used) continue;
    if (existing == &peripherals[i]) continue;
    Lock_Kind kind = LOCK_KIND_GPIO;
    int pin = 0;
    if (Lock_OverlapsPeripheral(&peripherals[i], resources, count, &kind, &pin)) {
      Lock_FillConflict(conflictOut, true, kind, pin, peripherals[i].label);
      xSemaphoreGive(mutex);
      return ESP_ERR_INVALID_STATE;
    }
  }

  for (size_t i = 0; i < CONFIG_LOCK_MAX_COUNT; i++) {
    if (!locks[i].used) continue;
    Lock_Kind kind = LOCK_KIND_GPIO;
    int pin = 0;
    if (Lock_Overlaps(&locks[i], resources, count, &kind, &pin)) {
      Lock_FillConflict(conflictOut, false, kind, pin, NULL);
      xSemaphoreGive(mutex);
      return ESP_ERR_INVALID_STATE;
    }
  }

  Lock_PeripheralHold* slot = existing;
  if (slot == NULL) {
    slot = Lock_FindPeripheralUnlocked();
    if (slot == NULL) {
      xSemaphoreGive(mutex);
      return ESP_ERR_NO_MEM;
    }
  }

  memset(slot, 0, sizeof(*slot));
  snprintf(slot->owner, sizeof(slot->owner), "%s", owner);
  Lock_OwnerToLabel(owner, slot->label, sizeof(slot->label));
  slot->resourceCount = count;
  memcpy(slot->resources, resources, count * sizeof(Lock_Resource));
  slot->used = true;
  xSemaphoreGive(mutex);
  return ESP_OK;
}

esp_err_t Lock_ReleasePeripheral(const char* owner)
{
  if (owner == NULL || owner[0] == '\0') return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(mutex, portMAX_DELAY);
  Lock_PeripheralHold* hold = Lock_FindPeripheralByOwner(owner);
  if (hold != NULL) {
    hold->used = false;
  }
  xSemaphoreGive(mutex);
  return ESP_OK;
}
