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
    if ((expression) != ESP_OK) {                                     \
      ESP_LOGE("SAIHUB-Main", "Failed to load module %s", moduleName); \
      someModuleFailed = true;                                        \
    }                                                                 \
  }

void app_main(void);

#endif
