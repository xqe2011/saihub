#ifndef NTP_H__
#define NTP_H__

#include <esp_err.h>
#include <stdbool.h>

esp_err_t Ntp_Init(void);
esp_err_t Ntp_SyncAndWait(void);
bool Ntp_IsSynced(void);

#endif
