/**
 * @file main.h
 */
#ifndef MAIN_H__
#define MAIN_H__

#include <esp_err.h>
#include <esp_log.h>

#define MAIN_LOAD_MODULE(expression, moduleName) \
  {                                              \
    if ((expression) != ESP_OK) {                \
      ESP_LOGE("Main", "Failed to load module %s", moduleName); \
      someModuleFailed = true;                   \
    }                                            \
  }

void app_main(void);

#endif
