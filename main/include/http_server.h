#ifndef HTTP_SERVER_H__
#define HTTP_SERVER_H__

#include <esp_err.h>

esp_err_t HttpServer_Init(void);
esp_err_t HttpServer_Start(void);
esp_err_t HttpServer_Stop(void);

#endif
