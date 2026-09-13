#include "nvs.h"

#include "config.h"
#include "tool.h"

#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <stdlib.h>
#include <string.h>

static const char* tag = "SAIHUB-Nvs";

esp_err_t Nvs_Erase(void)
{
  return nvs_flash_erase();
}

esp_err_t Nvs_SetString(const char* key, const char* value)
{
  size_t valueLength = strlen(value) + 1;
  char* tempValue = malloc(valueLength);
  if (tempValue == NULL) {
    return ESP_ERR_NO_MEM;
  }
  if (Nvs_GetString(key, tempValue, valueLength) == ESP_OK && memcmp(tempValue, value, valueLength) == 0) {
    free(tempValue);
    return ESP_OK;
  }
  free(tempValue);

  nvs_handle_t handle;
  if (nvs_open(CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
    return ESP_FAIL;
  }
  esp_err_t ret = nvs_set_str(handle, key, value);
  if (ret != ESP_OK) {
    ESP_LOGE(tag, "Cannot write key %s: %s", key, esp_err_to_name(ret));
    NVS_RETURN_AND_CLOSE(handle, ESP_FAIL);
  }
  ret = nvs_commit(handle);
  if (ret != ESP_OK) {
    ESP_LOGE(tag, "Cannot commit key %s", key);
    NVS_RETURN_AND_CLOSE(handle, ESP_FAIL);
  }
  nvs_close(handle);
  return ESP_OK;
}

esp_err_t Nvs_GetString(const char* key, char* value, size_t maxLength)
{
  nvs_handle_t handle;
  if (nvs_open(CONFIG_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
    return ESP_FAIL;
  }
  size_t realLength = 0;
  esp_err_t ret = nvs_get_str(handle, key, NULL, &realLength);
  if (ret == ESP_ERR_NVS_NOT_FOUND) {
    NVS_RETURN_AND_CLOSE(handle, ESP_ERR_NVS_NOT_FOUND);
  }
  if (ret != ESP_OK) {
    NVS_RETURN_AND_CLOSE(handle, ESP_FAIL);
  }
  if (realLength > maxLength) {
    NVS_RETURN_AND_CLOSE(handle, ESP_FAIL);
  }
  if (nvs_get_str(handle, key, value, &realLength) != ESP_OK) {
    NVS_RETURN_AND_CLOSE(handle, ESP_FAIL);
  }
  NVS_RETURN_AND_CLOSE(handle, ESP_OK);
}

esp_err_t Nvs_GetStringDefault(const char* key, char* value, size_t maxLength, const char* defaultValue)
{
  esp_err_t ret = Nvs_GetString(key, value, maxLength);
  if (ret == ESP_ERR_NVS_NOT_FOUND) {
    strncpy(value, defaultValue, maxLength - 1);
    value[maxLength - 1] = '\0';
    return ESP_OK;
  }
  return ret;
}

esp_err_t Nvs_Init(void)
{
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    TOOL_CHECK_ESP_OK_OR_RETURN(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  TOOL_CHECK_ESP_OK_OR_RETURN(ret);
  ESP_LOGI(tag, "NVS ready");
  return ESP_OK;
}
