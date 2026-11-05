/**
 * @name I2C control module
 * @file i2c_ctrl.h
 * @author xqe2011
 */
#ifndef I2C_CTRL_H__
#define I2C_CTRL_H__

#include "config.h"

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  I2C_CTRL_ROLE_MASTER = 0,
  I2C_CTRL_ROLE_SLAVE,
} I2cCtrl_Role;

typedef enum {
  I2C_CTRL_MAP_EXACT = 0,
  I2C_CTRL_MAP_PREFIX,
} I2cCtrl_MapType;

typedef struct {
  bool enable;
  I2cCtrl_Role role;
  int clockHz;     /* 0 when role is slave */
  int address;     /* slave address when role is slave; 0 when master */
  int addressBits; /* 7 or 10 */
  bool pullUp;     /* weak internal pull-ups on sda/scl */
  int sclWaitUs;   /* master SCL stretch wait; 0 = driver default; unused for slave */
  int sdaPin;      /* logical pin, or -1 */
  int sclPin;
} I2cCtrl_Config;

typedef struct {
  int64_t startUs;
  int64_t endUs;
  uint16_t recvLen;
  uint16_t sendLen;
  const uint8_t* recv;
  const uint8_t* send;
} I2cCtrl_Event;

bool I2cCtrl_IsValidId(int id);
int I2cCtrl_GetCount(void);
esp_err_t I2cCtrl_Init(void);
esp_err_t I2cCtrl_GetConfig(int id, I2cCtrl_Config* out);
esp_err_t I2cCtrl_SetConfig(int id, const I2cCtrl_Config* cfg, char* reason, size_t reasonLen);

/**
 * Master transfer. writeLen and/or readLen may be 0 (address probe when both are 0).
 * timeoutMs < 0 uses the default of 1000 ms.
 * *rxLen is set to readLen on success when readLen > 0.
 */
esp_err_t I2cCtrl_MasterTransceive(int id, int address, const uint8_t* tx, size_t txLen, uint8_t* rx, size_t readLen,
                                   int timeoutMs, size_t* rxLen, char* reason, size_t reasonLen);

esp_err_t I2cCtrl_CheckSlaveReceive(int id, char* reason, size_t reasonLen);
/** Copy queued event bytes into storage and remove them from the ring. Returns the byte count. */
size_t I2cCtrl_TakeSlaveEvents(int id, uint8_t* storage, size_t cap);
/** Decode the next event in a buffer from I2cCtrl_TakeSlaveEvents. recv and send point into storage. */
bool I2cCtrl_NextSlaveEvent(const uint8_t* storage, size_t len, size_t* offset, I2cCtrl_Event* out);

/** Append one packed map record. *used starts at 0 for a new map. */
esp_err_t I2cCtrl_PackMapAppend(uint8_t* buf, size_t cap, size_t* used, int index, I2cCtrl_MapType type,
                                const uint8_t* receive, size_t receiveLen, const uint8_t* send, size_t sendLen,
                                char* reason, size_t reasonLen);
esp_err_t I2cCtrl_SetSlaveMap(int id, const uint8_t* packed, size_t packedLen, char* reason, size_t reasonLen);

const char* I2cCtrl_RoleToString(I2cCtrl_Role role);
bool I2cCtrl_RoleFromString(const char* s, I2cCtrl_Role* out);

#endif
