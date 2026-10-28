/**
 * @name System info and OTA
 * @file system.c
 * @author xqe2011
 */
#include "system.h"

#include "cloud.h"
#include "config.h"
#include "tool.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* tag = "SAIHub-System";

static portMUX_TYPE otaMux = portMUX_INITIALIZER_UNLOCKED;
static System_OtaState otaState = SYSTEM_OTA_IDLE;
static int otaProgress = -1;
static char otaFailReason[192];

typedef struct {
  char* url;
  uint8_t expectSha[32];
} System_OtaJob;

static void System_OtaSetState(System_OtaState state, int progress, const char* reason)
{
  portENTER_CRITICAL(&otaMux);
  otaState = state;
  otaProgress = progress;
  if (reason && reason[0]) {
    snprintf(otaFailReason, sizeof(otaFailReason), "%s", reason);
  } else if (state != SYSTEM_OTA_FAILED) {
    otaFailReason[0] = '\0';
  }
  portEXIT_CRITICAL(&otaMux);
}

void System_GetInfo(System_Info* out)
{
  if (!out) return;
  const esp_app_desc_t* desc = esp_app_get_description();
  out->digest = Cloud_GetPublicKeyDigest();
  out->version = desc->version;
  snprintf(out->compiledDate, sizeof(out->compiledDate), "%s %s", desc->date, desc->time);
}

bool System_OtaIsUpgrading(void)
{
  portENTER_CRITICAL(&otaMux);
  bool busy = (otaState == SYSTEM_OTA_UPGRADING);
  portEXIT_CRITICAL(&otaMux);
  return busy;
}

void System_OtaGetStatus(System_OtaStatus* out)
{
  if (!out) return;
  portENTER_CRITICAL(&otaMux);
  out->state = otaState;
  out->progressPercent = otaProgress;
  snprintf(out->reason, sizeof(out->reason), "%s", otaFailReason);
  portEXIT_CRITICAL(&otaMux);
}

static bool System_ParseSha256Hex(const char* hex, uint8_t out[32])
{
  if (!hex || strlen(hex) != 64) return false;
  for (int i = 0; i < 32; i++) {
    unsigned v = 0;
    char pair[3] = {hex[i * 2], hex[i * 2 + 1], '\0'};
    if (sscanf(pair, "%2x", &v) != 1) return false;
    out[i] = (uint8_t)v;
  }
  return true;
}

typedef struct {
  char* buf;
  size_t len;
  size_t cap;
  esp_err_t err;
} System_HttpBody;

static esp_err_t System_HttpEvent(esp_http_client_event_t* evt)
{
  System_HttpBody* body = evt->user_data;
  if (!body) return ESP_OK;
  if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
    if (body->len + (size_t)evt->data_len + 1 > body->cap) {
      body->err = ESP_ERR_NO_MEM;
      return ESP_FAIL;
    }
    memcpy(body->buf + body->len, evt->data, (size_t)evt->data_len);
    body->len += (size_t)evt->data_len;
    body->buf[body->len] = '\0';
  }
  return ESP_OK;
}

static esp_err_t System_HttpGet(const char* url, char* buf, size_t cap, size_t* lenOut, char* reason, size_t reasonLen)
{
  if (lenOut) *lenOut = 0;
  if (!url || !buf || cap < 2) {
    snprintf(reason, reasonLen, "invalid HTTP get arguments.");
    return ESP_ERR_INVALID_ARG;
  }
  memset(buf, 0, cap);
  System_HttpBody body = {.buf = buf, .len = 0, .cap = cap, .err = ESP_OK};
  esp_http_client_config_t cfg = {
      .url = url,
      .timeout_ms = CONFIG_OTA_HTTP_TIMEOUT_MS,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .event_handler = System_HttpEvent,
      .user_data = &body,
      .keep_alive_enable = false,
      .user_agent = "saihub-ota/1.0",
  };
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) {
    snprintf(reason, reasonLen, "Could not start HTTP client.");
    return ESP_ERR_NO_MEM;
  }
  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (body.err != ESP_OK) {
    snprintf(reason, reasonLen, "Update manifest is too large.");
    return body.err;
  }
  if (err != ESP_OK) {
    snprintf(reason, reasonLen, "Could not download update manifest.");
    return err;
  }
  if (status < 200 || status >= 300) {
    snprintf(reason, reasonLen, "Update manifest HTTP status %d.", status);
    return ESP_FAIL;
  }
  if (lenOut) *lenOut = body.len;
  return ESP_OK;
}

/** True when url is a direct github.com link (not already proxied). */
static bool System_OtaUrlIsGithub(const char* url)
{
  if (!url) return false;
  return strncmp(url, "https://github.com/", 19) == 0 || strncmp(url, "http://github.com/", 18) == 0;
}

/** Prefix a github.com url with gh-proxy. Returns false if not github or result would not fit. */
static bool System_OtaProxyUrl(const char* url, char* out, size_t outLen)
{
  if (!url || !out || outLen < 2 || !System_OtaUrlIsGithub(url)) return false;
  if (strlen(CONFIG_OTA_GH_PROXY_PREFIX) + strlen(url) + 1 > outLen) return false;
  snprintf(out, outLen, "%s%s", CONFIG_OTA_GH_PROXY_PREFIX, url);
  return true;
}

cJSON* System_OtaCheck(char* reason, size_t reasonLen)
{
  if (reason && reasonLen) reason[0] = '\0';
  if (System_OtaIsUpgrading()) {
    snprintf(reason, reasonLen, "Firmware update in progress.");
    return NULL;
  }

  char* buf = calloc(1, CONFIG_OTA_MANIFEST_MAX_BYTES);
  if (!buf) {
    snprintf(reason, reasonLen, "Out of memory.");
    return NULL;
  }
  size_t len = 0;
  bool usedProxy = false;
  esp_err_t err = System_HttpGet(CONFIG_OTA_MANIFEST_URL, buf, CONFIG_OTA_MANIFEST_MAX_BYTES, &len, reason, reasonLen);
  if (err == ESP_ERR_TIMEOUT) {
    char proxyUrl[CONFIG_OTA_URL_MAX_BYTES];
    if (!System_OtaProxyUrl(CONFIG_OTA_MANIFEST_URL, proxyUrl, sizeof(proxyUrl))) {
      free(buf);
      snprintf(reason, reasonLen, "Update manifest URL is too long for proxy retry.");
      return NULL;
    }
    ESP_LOGW(tag, "OTA manifest timed out; retrying via gh-proxy");
    err = System_HttpGet(proxyUrl, buf, CONFIG_OTA_MANIFEST_MAX_BYTES, &len, reason, reasonLen);
    usedProxy = (err == ESP_OK);
  }
  if (err != ESP_OK) {
    free(buf);
    return NULL;
  }

  cJSON* root = cJSON_ParseWithLength(buf, len);
  free(buf);
  if (!root || !cJSON_IsObject(root)) {
    cJSON_Delete(root);
    snprintf(reason, reasonLen, "Update manifest is not valid JSON.");
    return NULL;
  }

  const esp_app_desc_t* desc = esp_app_get_description();
  cJSON* out = cJSON_CreateObject();
  if (!out) {
    cJSON_Delete(root);
    snprintf(reason, reasonLen, "Out of memory.");
    return NULL;
  }
  cJSON_AddStringToObject(out, "currentVersion", desc->version);

  cJSON* versionItem = cJSON_GetObjectItem(root, "version");
  cJSON* images = cJSON_GetObjectItem(root, "images");
  if (!cJSON_IsString(versionItem) || !versionItem->valuestring || !cJSON_IsArray(images)) {
    cJSON_Delete(root);
    cJSON_Delete(out);
    snprintf(reason, reasonLen, "Update manifest is missing version or images.");
    return NULL;
  }

  cJSON* match = NULL;
  cJSON* img = NULL;
  cJSON_ArrayForEach(img, images)
  {
    if (!cJSON_IsObject(img)) continue;
    cJSON* target = cJSON_GetObjectItem(img, "target");
    if (!cJSON_IsString(target) || !target->valuestring) continue;
    if (strcmp(target->valuestring, CONFIG_OTA_IMAGE_TARGET) != 0) continue;
    match = img;
    break;
  }
  if (!match) {
    cJSON_Delete(root);
    cJSON_Delete(out);
    snprintf(reason, reasonLen, "No firmware image for this device target.");
    return NULL;
  }

  cJSON* url = cJSON_GetObjectItem(match, "url");
  cJSON* sha = cJSON_GetObjectItem(match, "sha256");
  if (!cJSON_IsString(url) || !url->valuestring || !cJSON_IsString(sha) || !sha->valuestring ||
      strlen(sha->valuestring) != 64) {
    cJSON_Delete(root);
    cJSON_Delete(out);
    snprintf(reason, reasonLen, "Matched image is missing url or sha256.");
    return NULL;
  }

  bool available = strcmp(versionItem->valuestring, desc->version) != 0;
  cJSON_AddBoolToObject(out, "updateAvailable", available);
  if (available) {
    cJSON_AddStringToObject(out, "version", versionItem->valuestring);
    cJSON* releaseDate = cJSON_GetObjectItem(root, "releaseDate");
    if (cJSON_IsString(releaseDate) && releaseDate->valuestring) {
      cJSON_AddStringToObject(out, "releaseDate", releaseDate->valuestring);
    }
    cJSON* changelog = cJSON_GetObjectItem(root, "changelog");
    if (cJSON_IsString(changelog) && changelog->valuestring) {
      cJSON_AddStringToObject(out, "changelog", changelog->valuestring);
    }
    /* If the check needed gh-proxy, return a proxied image URL so upgrade can fetch it too. */
    if (usedProxy && System_OtaUrlIsGithub(url->valuestring)) {
      char proxiedUrl[CONFIG_OTA_URL_MAX_BYTES];
      if (!System_OtaProxyUrl(url->valuestring, proxiedUrl, sizeof(proxiedUrl))) {
        cJSON_Delete(root);
        cJSON_Delete(out);
        snprintf(reason, reasonLen, "Image URL is too long for proxy.");
        return NULL;
      }
      cJSON_AddStringToObject(out, "url", proxiedUrl);
    } else {
      cJSON_AddStringToObject(out, "url", url->valuestring);
    }
    cJSON_AddStringToObject(out, "sha256", sha->valuestring);
  }

  cJSON_Delete(root);
  return out;
}

static void System_OtaTask(void* arg)
{
  System_OtaJob* job = arg;
  System_OtaSetState(SYSTEM_OTA_UPGRADING, 0, NULL);

  const char* url = job->url;
  char proxyUrl[CONFIG_OTA_URL_MAX_BYTES];
  bool triedProxy = false;
  esp_https_ota_handle_t handle = NULL;
  esp_err_t err = ESP_FAIL;

retry_download:
  {
    esp_http_client_config_t http = {
        .url = url,
        .timeout_ms = CONFIG_OTA_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .user_agent = "saihub-ota/1.0",
    };
    esp_https_ota_config_t otaCfg = {
        .http_config = &http,
    };

    handle = NULL;
    err = esp_https_ota_begin(&otaCfg, &handle);
    if (err != ESP_OK || !handle) {
      if (!triedProxy && err == ESP_ERR_TIMEOUT && System_OtaProxyUrl(job->url, proxyUrl, sizeof(proxyUrl))) {
        ESP_LOGW(tag, "OTA download timed out; retrying via gh-proxy");
        url = proxyUrl;
        triedProxy = true;
        goto retry_download;
      }
      System_OtaSetState(SYSTEM_OTA_FAILED, -1, "Could not start firmware download.");
      goto done;
    }

    esp_app_desc_t newDesc;
    err = esp_https_ota_get_img_desc(handle, &newDesc);
    if (err != ESP_OK) {
      esp_https_ota_abort(handle);
      handle = NULL;
      if (!triedProxy && err == ESP_ERR_TIMEOUT && System_OtaProxyUrl(job->url, proxyUrl, sizeof(proxyUrl))) {
        ESP_LOGW(tag, "OTA download timed out; retrying via gh-proxy");
        url = proxyUrl;
        triedProxy = true;
        goto retry_download;
      }
      System_OtaSetState(SYSTEM_OTA_FAILED, -1, "Could not read firmware image header.");
      goto done;
    }
    if (memcmp(newDesc.app_elf_sha256, job->expectSha, 32) != 0) {
      esp_https_ota_abort(handle);
      handle = NULL;
      System_OtaSetState(SYSTEM_OTA_FAILED, -1, "Firmware image digest does not match sha256.");
      goto done;
    }

    while (true) {
      err = esp_https_ota_perform(handle);
      if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
      int total = esp_https_ota_get_image_size(handle);
      int read = esp_https_ota_get_image_len_read(handle);
      int pct = (total > 0) ? (int)((read * 100LL) / total) : -1;
      System_OtaSetState(SYSTEM_OTA_UPGRADING, pct, NULL);
    }

    if (err != ESP_OK) {
      esp_https_ota_abort(handle);
      handle = NULL;
      if (!triedProxy && err == ESP_ERR_TIMEOUT && System_OtaProxyUrl(job->url, proxyUrl, sizeof(proxyUrl))) {
        ESP_LOGW(tag, "OTA download timed out; retrying via gh-proxy");
        url = proxyUrl;
        triedProxy = true;
        goto retry_download;
      }
      System_OtaSetState(SYSTEM_OTA_FAILED, -1, "Firmware download failed.");
      goto done;
    }
    if (!esp_https_ota_is_complete_data_received(handle)) {
      esp_https_ota_abort(handle);
      handle = NULL;
      System_OtaSetState(SYSTEM_OTA_FAILED, -1, "Firmware download incomplete.");
      goto done;
    }

    err = esp_https_ota_finish(handle);
    handle = NULL;
    if (err != ESP_OK) {
      System_OtaSetState(SYSTEM_OTA_FAILED, -1, "Could not finalize firmware update.");
      goto done;
    }
  }

  ESP_LOGI(tag, "OTA success, rebooting");
  free(job->url);
  free(job);
  esp_restart();
  return;

done:
  free(job->url);
  free(job);
  vTaskDelete(NULL);
}

esp_err_t System_OtaUpgrade(const char* url, const char* sha256, char* reason, size_t reasonLen)
{
  if (reason && reasonLen) reason[0] = '\0';
  if (!url || !url[0] || !sha256) {
    snprintf(reason, reasonLen, "url and sha256 are required.");
    return ESP_ERR_INVALID_ARG;
  }
  uint8_t expect[32];
  if (!System_ParseSha256Hex(sha256, expect)) {
    snprintf(reason, reasonLen, "sha256 must be 64 hexadecimal characters.");
    return ESP_ERR_INVALID_ARG;
  }
  if (System_OtaIsUpgrading()) {
    snprintf(reason, reasonLen, "Firmware update in progress.");
    return ESP_ERR_INVALID_STATE;
  }

  System_OtaJob* job = calloc(1, sizeof(*job));
  if (!job) {
    snprintf(reason, reasonLen, "Out of memory.");
    return ESP_ERR_NO_MEM;
  }
  size_t n = strlen(url);
  job->url = malloc(n + 1);
  if (!job->url) {
    free(job);
    snprintf(reason, reasonLen, "Out of memory.");
    return ESP_ERR_NO_MEM;
  }
  memcpy(job->url, url, n + 1);
  memcpy(job->expectSha, expect, 32);

  System_OtaSetState(SYSTEM_OTA_UPGRADING, 0, NULL);
  if (xTaskCreate(System_OtaTask, "system_ota", 12288, job, 5, NULL) != pdPASS) {
    System_OtaSetState(SYSTEM_OTA_IDLE, -1, NULL);
    free(job->url);
    free(job);
    snprintf(reason, reasonLen, "Could not start firmware update task.");
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}
