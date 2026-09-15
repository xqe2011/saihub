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

typedef struct {
  bool enable;
  int baudRate;
  int dataBits;
  UartCtrl_Parity parity;
  int stopBits;
  UartCtrl_Encoding encoding;
  int rxPin; /* logical pin, or -1 if unused */
  int txPin; /* logical pin, or -1 if unused */
} UartCtrl_Config;

bool UartCtrl_IsValidId(int id);
int UartCtrl_GetCount(void);
esp_err_t UartCtrl_Init(void);
esp_err_t UartCtrl_GetConfig(int id, UartCtrl_Config* out);
/**
 * Apply full config. When enable is true, claims pins via peripheral lock.
 * On failure the previous running state is preserved when possible.
 */
esp_err_t UartCtrl_SetConfig(int id, const UartCtrl_Config* cfg, char* reason, size_t reasonLen);
esp_err_t UartCtrl_Transmit(int id, const uint8_t* data, size_t len, char* reason, size_t reasonLen);
/** Drain up to maxLen bytes from RX into out. *outLen is set to bytes written. */
esp_err_t UartCtrl_Receive(int id, uint8_t* out, size_t maxLen, size_t* outLen, char* reason, size_t reasonLen);
esp_err_t UartCtrl_Flush(int id, char* reason, size_t reasonLen);

const char* UartCtrl_ParityToString(UartCtrl_Parity p);
bool UartCtrl_ParityFromString(const char* s, UartCtrl_Parity* out);
const char* UartCtrl_EncodingToString(UartCtrl_Encoding e);
bool UartCtrl_EncodingFromString(const char* s, UartCtrl_Encoding* out);

#endif
