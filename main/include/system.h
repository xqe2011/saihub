/**
 * @name System info and OTA
 * @file system.h
 * @author xqe2011
 */
#ifndef SYSTEM_H__
#define SYSTEM_H__

#include <cJSON.h>
#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
  const char* digest;
  const char* version;
  char compiledDate[48];
} System_Info;

typedef enum {
  SYSTEM_OTA_IDLE = 0,
  SYSTEM_OTA_UPGRADING,
  SYSTEM_OTA_FAILED,
} System_OtaState;

typedef struct {
  System_OtaState state;
  int progressPercent; /* 0-100 while upgrading; -1 if unknown */
  char reason[192];    /* failure reason when state is FAILED */
} System_OtaStatus;

/** Fill info from the running app image header and Authentication Digest. */
void System_GetInfo(System_Info* out);

/** True while a firmware upgrade task is running. */
bool System_OtaIsUpgrading(void);

/** Live OTA status only (no last-check cache). */
void System_OtaGetStatus(System_OtaStatus* out);

/**
 * Side-effect-free update check: fetch manifest, pick matching target image.
 * Tries the direct GitHub manifest URL first; on HTTP timeout or connection
 * reset retries via CONFIG_OTA_GH_PROXY_PREFIX (same as the flash page). When
 * the proxy path is used and the image url is github.com, a successful
 * updateAvailable response prefixes url the same way.
 * On success returns an owned JSON object:
 * { updateAvailable, currentVersion, version?, releaseDate?, changelog?, url?, sha256? }
 * Caller must cJSON_Delete. On failure returns NULL and writes reason.
 */
cJSON* System_OtaCheck(char* reason, size_t reasonLen);

/**
 * Start background upgrade. url and sha256 (64 hex chars of app ELF digest) are required.
 * On download timeout or connection reset, if url is a direct github.com link,
 * retries once via CONFIG_OTA_GH_PROXY_PREFIX.
 * Returns ESP_OK when the task started, ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_STATE / ESP_ERR_NO_MEM otherwise.
 */
esp_err_t System_OtaUpgrade(const char* url, const char* sha256, char* reason, size_t reasonLen);

#endif
