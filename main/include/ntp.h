#ifndef NTP_H__
#define NTP_H__

#include <esp_err.h>
#include <stdbool.h>

typedef void (*Ntp_SyncedCallback)(void);

esp_err_t Ntp_Init(void);
esp_err_t Ntp_Start(void);
void Ntp_Stop(void);
bool Ntp_IsSynced(void);
esp_err_t Ntp_RegisterSyncedCallback(Ntp_SyncedCallback callback);

#endif
