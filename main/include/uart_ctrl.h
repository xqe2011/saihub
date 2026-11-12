/**
 * @name UART control module
 * @file uart_ctrl.h
 * @author xqe2011
 */
#ifndef UART_CTRL_H__
#define UART_CTRL_H__

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  UART_CTRL_PARITY_NONE = 0,
  UART_CTRL_PARITY_EVEN,
  UART_CTRL_PARITY_ODD,
} UartCtrl_Parity;

typedef enum {
  UART_CTRL_ENCODING_UTF8 = 0,
  UART_CTRL_ENCODING_BYTE,
} UartCtrl_Encoding;

typedef enum {
  UART_CTRL_MODE_NATIVE = 0,
  UART_CTRL_MODE_RFC2217,
} UartCtrl_Mode;

typedef struct {
  bool enable;
  UartCtrl_Mode mode;
  int baudRate;
  int dataBits;
  UartCtrl_Parity parity;
  int stopBits;
  UartCtrl_Encoding encoding;
  int rxPin;  /* logical pin, or -1 if unused */
  int txPin;  /* logical pin, or -1 if unused */
  int rtsPin; /* logical pin, or -1 if unused */
  int dtrPin; /* logical pin, or -1 if unused */
} UartCtrl_Config;

typedef struct {
  bool cloud;
  const char* prompt; /* local or cloud prompt; not owned */
  int port;           /* local */
  char ip[16];        /* local STA IPv4 text, or empty if none — never "0.0.0.0" */
  char url[640];      /* cloud WebSocket URL with a token placeholder */
} UartCtrl_Rfc2217Status;

bool UartCtrl_IsValidId(int id);
int UartCtrl_GetCount(void);
esp_err_t UartCtrl_Init(void);
esp_err_t UartCtrl_GetConfig(int id, UartCtrl_Config* out);
/**
 * Apply config. Only the fields that differ from the current config are applied.
 * When enable is true, claims pins via peripheral lock.
 * On failure the previous running state is preserved when possible.
 * RFC2217 line changes call this too; mode gating for HTTP/MCP lives in the routes.
 */
esp_err_t UartCtrl_SetConfig(int id, const UartCtrl_Config* cfg, char* reason, size_t reasonLen);
/** ESP_ERR_INVALID_STATE when this UART is not enabled in rfc2217 mode (route → 503). ESP_FAIL when a cloud URL cannot be built. */
esp_err_t UartCtrl_GetRfc2217Status(int id, bool cloud, UartCtrl_Rfc2217Status* out);
esp_err_t UartCtrl_Transmit(int id, const uint8_t* data, size_t len, char* reason, size_t reasonLen);
/** Drain up to maxLen bytes from RX into out. *outLen is set to bytes written. */
esp_err_t UartCtrl_Receive(int id, uint8_t* out, size_t maxLen, size_t* outLen, char* reason, size_t reasonLen);
esp_err_t UartCtrl_Flush(int id, char* reason, size_t reasonLen);
/** Read stored DTR/RTS active state. Requires UART enabled with that pin set. */
esp_err_t UartCtrl_GetDtrActive(int id, bool* activeOut, char* reason, size_t reasonLen);
esp_err_t UartCtrl_GetRtsActive(int id, bool* activeOut, char* reason, size_t reasonLen);
esp_err_t UartCtrl_SetDtr(int id, bool active, char* reason, size_t reasonLen);
esp_err_t UartCtrl_SetRts(int id, bool active, char* reason, size_t reasonLen);

const char* UartCtrl_ParityToString(UartCtrl_Parity p);
bool UartCtrl_ParityFromString(const char* s, UartCtrl_Parity* out);
const char* UartCtrl_EncodingToString(UartCtrl_Encoding e);
bool UartCtrl_EncodingFromString(const char* s, UartCtrl_Encoding* out);
const char* UartCtrl_ModeToString(UartCtrl_Mode mode);
bool UartCtrl_ModeFromString(const char* s, UartCtrl_Mode* out);

#endif
