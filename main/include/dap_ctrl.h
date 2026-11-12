/**
 * @name CMSIS-DAP control module
 * @file dap_ctrl.h
 * @author xqe2011
 */
#ifndef DAP_CTRL_H__
#define DAP_CTRL_H__

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  bool enable;
  int swclkPin;  /* logical, required when enable */
  int swdioPin;  /* logical, required when enable */
  int nresetPin; /* logical, or -1 */
} DapCtrl_Config;

typedef struct {
  bool cloud;
  const char* prompt; /* local or cloud prompt; not owned */
  int port;           /* local */
  char ip[16];        /* local STA IPv4 text, or empty if none — never "0.0.0.0" */
  char url[640];      /* cloud WebSocket URL with a token placeholder */
} DapCtrl_Status;

/** Transport sends one CMSIS-DAP response. data is only valid for the call. */
typedef void (*DapCtrl_SendFn)(void* user, const uint8_t* data, size_t length);

esp_err_t DapCtrl_Init(void);
esp_err_t DapCtrl_GetConfig(DapCtrl_Config* out);
esp_err_t DapCtrl_SetConfig(const DapCtrl_Config* cfg, char* reason, size_t reasonLen);
/** ESP_ERR_INVALID_STATE when the TCP service is not running (route → 503). ESP_FAIL when a cloud URL cannot be built. */
esp_err_t DapCtrl_GetStatus(bool cloud, DapCtrl_Status* out);

/**
 * One debugger session. ESP_ERR_INVALID_STATE when a session is already active or DAP is disabled.
 * The caller drops the client in that case.
 */
esp_err_t DapCtrl_OnConnect(DapCtrl_SendFn send, void* user);
/** One complete CMSIS-DAP request. The response is passed to the send callback. */
void DapCtrl_OnData(const uint8_t* data, size_t length);
void DapCtrl_OnDisconnect(void);

#endif
