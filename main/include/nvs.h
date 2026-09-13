#ifndef NVS_H__
#define NVS_H__

#include <esp_err.h>
#include <stddef.h>

#define NVS_RETURN_AND_CLOSE(handle, code) \
  {                                        \
    nvs_close(handle);                     \
    return code;                           \
  }

esp_err_t Nvs_Init(void);
esp_err_t Nvs_SetString(const char* key, const char* value);
esp_err_t Nvs_GetString(const char* key, char* value, size_t maxLength);
esp_err_t Nvs_GetStringDefault(const char* key, char* value, size_t maxLength, const char* defaultValue);
esp_err_t Nvs_Erase(void);

#endif
