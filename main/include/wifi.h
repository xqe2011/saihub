/**
 * @name WIFI module
 * @file wifi.h
 * @author xqe2011
 */
#ifndef WIFI_H__
#define WIFI_H__

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
  WIFI_PAIR_STATE_IDLE = 0,
  WIFI_PAIR_STATE_CONNECTING,
  WIFI_PAIR_STATE_CONNECTED,
  WIFI_PAIR_STATE_FAILED,
} Wifi_PairState;

typedef struct {
  char ssid[33];
  int rssi;
  bool secure;
} Wifi_Network;

typedef void (*Wifi_ConnectedCallback)(void);
typedef void (*Wifi_DisconnectedCallback)(void);
typedef void (*Wifi_PairingStartedCallback)(void);
typedef void (*Wifi_PairingStoppedCallback)(void);

bool Wifi_IsConnected(void);
bool Wifi_IsPairing(void);
esp_err_t Wifi_RegisterConnectedCallback(Wifi_ConnectedCallback callback);
esp_err_t Wifi_RegisterDisconnectedCallback(Wifi_DisconnectedCallback callback);
esp_err_t Wifi_RegisterPairingStartedCallback(Wifi_PairingStartedCallback callback);
esp_err_t Wifi_RegisterPairingStoppedCallback(Wifi_PairingStoppedCallback callback);
esp_err_t Wifi_ConnectWifi(const char* ssid, const char* password);
esp_err_t Wifi_ConnectWifiAsync(const char* ssid, const char* password);
esp_err_t Wifi_StartPairing(void);
esp_err_t Wifi_StopPairing(void);
esp_err_t Wifi_ScanNetworks(Wifi_Network* out, size_t maxOut, size_t* countOut);
Wifi_PairState Wifi_GetPairStatus(char* ipOut, size_t ipLen, char* reasonOut, size_t reasonLen);
esp_err_t Wifi_Init(void);

#endif
