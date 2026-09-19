/**
 * @name Main module
 * @file main.h
 * @author xqe2011
 */
#ifndef MAIN_H__
#define MAIN_H__

#include <esp_err.h>
#include <esp_log.h>

#define MAIN_LOAD_MODULE(expression, moduleName)                      \
  {                                                                   \
    esp_err_t moduleLoadError = (expression);                         \
    if (moduleLoadError != ESP_OK) {                                  \
      ESP_LOGE("SAIHUB-Main", "Failed to load module %s: %s (0x%x)",  \
               moduleName, esp_err_to_name(moduleLoadError),          \
               (unsigned int)moduleLoadError);                        \
      someModuleFailed = true;                                        \
    }                                                                 \
  }

void app_main(void);

#endif
