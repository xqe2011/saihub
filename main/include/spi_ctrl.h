/**
 * @name SPI control module
 * @file spi_ctrl.h
 * @author xqe2011
 */
#ifndef SPI_CTRL_H__
#define SPI_CTRL_H__

#include "config.h"

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  SPI_CTRL_ROLE_MASTER = 0,
  SPI_CTRL_ROLE_SLAVE,
} SpiCtrl_Role;

typedef enum {
  SPI_CTRL_BIT_MSB = 0,
  SPI_CTRL_BIT_LSB,
} SpiCtrl_BitOrder;

typedef enum {
  SPI_CTRL_MAP_EXACT = 0,
  SPI_CTRL_MAP_PREFIX,
} SpiCtrl_MapType;

typedef struct {
  bool enable;
  SpiCtrl_Role role;
  int cpol; /* 0 or 1 */
  int cpha; /* 0 or 1 */
  SpiCtrl_BitOrder bitOrder;
  int clockHz; /* 0 when role is slave */
  int mosiPin; /* logical pin, or -1 */
  int misoPin;
  int clkPin;
  int csPin;
} SpiCtrl_Config;

typedef struct {
  int64_t startUs;
  int64_t endUs;
  uint16_t recvLen;
  uint16_t sendLen;
  const uint8_t* recv;
  const uint8_t* send;
} SpiCtrl_Event;

bool SpiCtrl_IsValidId(int id);
int SpiCtrl_GetCount(void);
esp_err_t SpiCtrl_Init(void);
esp_err_t SpiCtrl_GetConfig(int id, SpiCtrl_Config* out);
esp_err_t SpiCtrl_SetConfig(int id, const SpiCtrl_Config* cfg, char* reason, size_t reasonLen);

/**
 * Clock out txLen bytes. *rxLen is txLen when MISO is set, otherwise 0.
 * rx may be NULL when MISO is unset or txLen is 0.
 */
esp_err_t SpiCtrl_MasterTransceive(int id, const uint8_t* tx, size_t txLen, uint8_t* rx, size_t* rxLen, char* reason,
                                   size_t reasonLen);

esp_err_t SpiCtrl_CheckSlaveReceive(int id, char* reason, size_t reasonLen);
/** Copy queued event bytes into storage and remove them from the ring. Returns the byte count. */
size_t SpiCtrl_TakeSlaveEvents(int id, uint8_t* storage, size_t cap);
/** Decode the next event in a buffer from SpiCtrl_TakeSlaveEvents. recv and send point into storage. */
bool SpiCtrl_NextSlaveEvent(const uint8_t* storage, size_t len, size_t* offset, SpiCtrl_Event* out);

/** Append one packed map record. *used starts at 0 for a new map. */
esp_err_t SpiCtrl_PackMapAppend(uint8_t* buf, size_t cap, size_t* used, int index, SpiCtrl_MapType type,
                                const uint8_t* mosi, size_t mosiLen, const uint8_t* miso, size_t misoLen, char* reason,
                                size_t reasonLen);
esp_err_t SpiCtrl_SetSlaveMap(int id, const uint8_t* packed, size_t packedLen, char* reason, size_t reasonLen);

const char* SpiCtrl_RoleToString(SpiCtrl_Role role);
bool SpiCtrl_RoleFromString(const char* s, SpiCtrl_Role* out);
const char* SpiCtrl_BitOrderToString(SpiCtrl_BitOrder order);
bool SpiCtrl_BitOrderFromString(const char* s, SpiCtrl_BitOrder* out);

#endif
