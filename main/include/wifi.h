/**
 * @name WIFI module
 * @file wifi.h
 * @author xqe2011
 */
#ifndef WIFI_H__
#define WIFI_H__

#include <esp_err.h>
#include <stdbool.h>

typedef void (*Wifi_ConnectedCallback)(void);
typedef void (*Wifi_DisconnectedCallback)(void);

bool Wifi_IsConnected(void);
esp_err_t Wifi_RegisterConnectedCallback(Wifi_ConnectedCallback callback);
esp_err_t Wifi_RegisterDisconnectedCallback(Wifi_DisconnectedCallback callback);
esp_err_t Wifi_ConnectWifi(const char* ssid, const char* password);
esp_err_t Wifi_Init(void);

#endif
