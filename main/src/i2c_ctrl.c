/**
 * @name I2C control module
 * @file i2c_ctrl.c
 * @author xqe2011
 */
#include "i2c_ctrl.h"

#include "gpio_ctrl.h"
#include "lock.h"
#include "tool.h"

#include <driver/i2c_master.h>
#include <driver/i2c_slave.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static const char* tag = "SAIHub-I2c";

static const int i2cIdToPort[] = CONFIG_I2C_ID_TO_PORT;

typedef struct {
  I2cCtrl_Config cfg;
  bool driverInstalled;
  bool master;
  i2c_master_bus_handle_t masterBus;
  i2c_slave_dev_handle_t slaveDev;
  char owner[32];
} I2cCtrl_Runtime;

enum {
  I2C_CTRL_EVT_RX = 1,
  I2C_CTRL_EVT_TX = 2,
};

typedef struct {
  uint8_t type;
  uint16_t len;
  uint8_t data[CONFIG_I2C_MAX_TRANSFER_BYTES];
  int64_t timeUs;
} I2cCtrl_SlaveEvt;

static I2cCtrl_Runtime buses[TOOL_GET_ARRAY_LENGTH(i2cIdToPort)];
static SemaphoreHandle_t mutex;
static SemaphoreHandle_t slaveStopped;
static QueueHandle_t slaveQueue;
static TaskHandle_t slaveTask;
static volatile bool slaveStop;

static Tool_Ring slaveEventRing;
static uint8_t mapBuf[CONFIG_I2C_SLAVE_MAP_BYTES];
static size_t mapUsed;
static uint8_t armed[CONFIG_I2C_MAX_TRANSFER_BYTES];
static uint8_t armedLen;
static uint8_t pendingRecv[CONFIG_I2C_MAX_TRANSFER_BYTES];
static uint16_t pendingRecvLen;
static int64_t pendingStartUs;
static bool pendingOpen;

bool I2cCtrl_IsValidId(int id)
{
  return id >= 0 && id < (int)TOOL_GET_ARRAY_LENGTH(i2cIdToPort);
}

int I2cCtrl_GetCount(void)
{
  return (int)TOOL_GET_ARRAY_LENGTH(i2cIdToPort);
}

const char* I2cCtrl_RoleToString(I2cCtrl_Role role)
{
  return role == I2C_CTRL_ROLE_SLAVE ? "slave" : "master";
}

bool I2cCtrl_RoleFromString(const char* s, I2cCtrl_Role* out)
{
  if (s == NULL || out == NULL) return false;
  if (strcmp(s, "master") == 0) {
    *out = I2C_CTRL_ROLE_MASTER;
    return true;
  }
  if (strcmp(s, "slave") == 0) {
    *out = I2C_CTRL_ROLE_SLAVE;
    return true;
  }
  return false;
}

static i2c_port_num_t I2cCtrl_HwPort(int id)
{
  return (i2c_port_num_t)i2cIdToPort[id];
}

static int64_t I2cCtrl_NowUs(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
}

static void I2cCtrl_MakeOwner(int id, char* out, size_t outLen)
{
  snprintf(out, outLen, "i2c:%d", id);
}

static bool I2cCtrl_PinOk(int pin)
{
  return pin < 0 || GpioCtrl_IsValidLogicalPin(pin);
}

static int I2cCtrl_AddressMax(int addressBits)
{
  return addressBits == 10 ? 1023 : 127;
}

static esp_err_t I2cCtrl_ValidateConfig(const I2cCtrl_Config* cfg, char* reason, size_t reasonLen)
{
  if (cfg == NULL) return ESP_ERR_INVALID_ARG;
  if (cfg->role != I2C_CTRL_ROLE_MASTER && cfg->role != I2C_CTRL_ROLE_SLAVE) {
    snprintf(reason, reasonLen, "role must be one of: master, slave.");
    return ESP_ERR_INVALID_ARG;
  }
  if (cfg->addressBits != 7 && cfg->addressBits != 10) {
    snprintf(reason, reasonLen, "addressBits must be 7 or 10.");
    return ESP_ERR_INVALID_ARG;
  }
  if (cfg->role == I2C_CTRL_ROLE_SLAVE) {
    if (cfg->clockHz != 0) {
      snprintf(reason, reasonLen, "clockHz is not supported when role is slave.");
      return ESP_ERR_INVALID_ARG;
    }
    if (cfg->sclWaitUs != 0) {
      snprintf(reason, reasonLen, "sclWaitUs is not supported when role is slave.");
      return ESP_ERR_INVALID_ARG;
    }
    int addrMax = I2cCtrl_AddressMax(cfg->addressBits);
    if (cfg->address < 0 || cfg->address > addrMax) {
      snprintf(reason, reasonLen, "address must be an integer from 0 to %d.", addrMax);
      return ESP_ERR_INVALID_ARG;
    }
  } else {
    if (cfg->address != 0) {
      snprintf(reason, reasonLen, "address is not supported when role is master.");
      return ESP_ERR_INVALID_ARG;
    }
    if (cfg->clockHz < CONFIG_I2C_CLOCK_HZ_MIN || cfg->clockHz > CONFIG_I2C_CLOCK_HZ_MAX) {
      snprintf(reason, reasonLen, "clockHz must be an integer from %d to %d.", CONFIG_I2C_CLOCK_HZ_MIN,
               CONFIG_I2C_CLOCK_HZ_MAX);
      return ESP_ERR_INVALID_ARG;
    }
    if (cfg->sclWaitUs < 0 || cfg->sclWaitUs > CONFIG_I2C_SCL_WAIT_US_MAX) {
      snprintf(reason, reasonLen, "sclWaitUs must be an integer from 0 to %d.", CONFIG_I2C_SCL_WAIT_US_MAX);
      return ESP_ERR_INVALID_ARG;
    }
  }
  if (!I2cCtrl_PinOk(cfg->sdaPin)) {
    snprintf(reason, reasonLen, "pins.sda is invalid. Use a pin from 0 to %d or omit it.",
             GpioCtrl_GetLogicalCount() - 1);
    return ESP_ERR_INVALID_ARG;
  }
  if (!I2cCtrl_PinOk(cfg->sclPin)) {
    snprintf(reason, reasonLen, "pins.scl is invalid. Use a pin from 0 to %d or omit it.",
             GpioCtrl_GetLogicalCount() - 1);
    return ESP_ERR_INVALID_ARG;
  }
  if (cfg->sdaPin >= 0 && cfg->sclPin >= 0 && cfg->sdaPin == cfg->sclPin) {
    snprintf(reason, reasonLen, "pins.sda and pins.scl must be different when both are set.");
    return ESP_ERR_INVALID_ARG;
  }
  if (cfg->enable && (cfg->sdaPin < 0 || cfg->sclPin < 0)) {
    snprintf(reason, reasonLen, "enable true requires pins.sda and pins.scl.");
    return ESP_ERR_INVALID_ARG;
  }
  return ESP_OK;
}

static void I2cCtrl_AddPinResource(Lock_Resource* out, size_t* count, int pin)
{
  if (pin < 0) return;
  out[*count].kind = LOCK_KIND_GPIO;
  out[*count].pin = pin;
  out[*count].methods = LOCK_METHOD_READ | LOCK_METHOD_WRITE;
  (*count)++;
}

static void I2cCtrl_BuildPinResources(const I2cCtrl_Config* cfg, Lock_Resource* out, size_t* countOut)
{
  size_t n = 0;
  I2cCtrl_AddPinResource(out, &n, cfg->sdaPin);
  I2cCtrl_AddPinResource(out, &n, cfg->sclPin);
  *countOut = n;
}

static void I2cCtrl_DisablePins(const I2cCtrl_Config* cfg)
{
  if (cfg->sdaPin >= 0) GpioCtrl_SetConfig(cfg->sdaPin, GPIO_CTRL_MODE_DISABLE, false, false, false);
  if (cfg->sclPin >= 0) GpioCtrl_SetConfig(cfg->sclPin, GPIO_CTRL_MODE_DISABLE, false, false, false);
}

static void I2cCtrl_ClearSlaveData(void)
{
  Tool_RingRelease(&slaveEventRing);
  mapUsed = 0;
  armedLen = 0;
  pendingRecvLen = 0;
  pendingOpen = false;
  memset(armed, 0, sizeof(armed));
  memset(mapBuf, 0, sizeof(mapBuf));
  memset(pendingRecv, 0, sizeof(pendingRecv));
}

typedef struct __attribute__((packed)) {
  int64_t startUs;
  int64_t endUs;
  uint16_t recvLen;
  uint16_t sendLen;
} I2cCtrl_EventRec;

_Static_assert(sizeof(I2cCtrl_EventRec) + 2 * CONFIG_I2C_MAX_TRANSFER_BYTES <= CONFIG_I2C_SLAVE_EVENT_BYTES,
               "I2C slave event must fit in the event buffer");

static size_t I2cCtrl_EventSize(const I2cCtrl_EventRec* rec)
{
  if (rec->recvLen > CONFIG_I2C_MAX_TRANSFER_BYTES || rec->sendLen > CONFIG_I2C_MAX_TRANSFER_BYTES) return 0;
  return sizeof(*rec) + (size_t)rec->recvLen + (size_t)rec->sendLen;
}

static bool I2cCtrl_PeekEventSize(size_t* sizeOut)
{
  I2cCtrl_EventRec rec;
  if (!Tool_RingPeek(&slaveEventRing, &rec, sizeof(rec))) return false;
  size_t size = I2cCtrl_EventSize(&rec);
  if (size == 0 || size > Tool_RingLen(&slaveEventRing)) return false;
  *sizeOut = size;
  return true;
}

static void I2cCtrl_DropOldestEvent(void)
{
  size_t size = 0;
  if (!I2cCtrl_PeekEventSize(&size) || !Tool_RingDrop(&slaveEventRing, size)) Tool_RingClear(&slaveEventRing);
}

static void I2cCtrl_AppendEvent(int64_t startUs, int64_t endUs, const uint8_t* recv, uint16_t recvLen, const uint8_t* send,
                                uint16_t sendLen)
{
  if (recv == NULL) recvLen = 0;
  if (send == NULL) sendLen = 0;
  if (recvLen > CONFIG_I2C_MAX_TRANSFER_BYTES) recvLen = CONFIG_I2C_MAX_TRANSFER_BYTES;
  if (sendLen > CONFIG_I2C_MAX_TRANSFER_BYTES) sendLen = CONFIG_I2C_MAX_TRANSFER_BYTES;
  I2cCtrl_EventRec rec = {.startUs = startUs, .endUs = endUs, .recvLen = recvLen, .sendLen = sendLen};
  size_t need = I2cCtrl_EventSize(&rec);
  if (need == 0 || need > slaveEventRing.cap) return;
  while (Tool_RingFree(&slaveEventRing) < need) I2cCtrl_DropOldestEvent();
  bool ok = Tool_RingWrite(&slaveEventRing, &rec, sizeof(rec));
  if (ok && recvLen > 0) ok = Tool_RingWrite(&slaveEventRing, recv, recvLen);
  if (ok && sendLen > 0) ok = Tool_RingWrite(&slaveEventRing, send, sendLen);
  if (!ok) Tool_RingClear(&slaveEventRing);
}

static bool I2cCtrl_MapRead(size_t* off, uint8_t* type, const uint8_t** receive, uint8_t* receiveLen, const uint8_t** send,
                            uint8_t* sendLen)
{
  if (*off + 3 > mapUsed) return false;
  *type = mapBuf[*off];
  *receiveLen = mapBuf[*off + 1];
  *sendLen = mapBuf[*off + 2];
  size_t need = 3u + *receiveLen + *sendLen;
  if (*off + need > mapUsed) return false;
  *receive = mapBuf + *off + 3;
  *send = *receive + *receiveLen;
  *off += need;
  return true;
}

static void I2cCtrl_ArmFromReceive(const uint8_t* recv, uint16_t recvLen)
{
  const uint8_t* exactSend = NULL;
  uint8_t exactLen = 0;
  bool exact = false;
  size_t off = 0;
  uint8_t type = 0;
  const uint8_t* receive = NULL;
  const uint8_t* send = NULL;
  uint8_t receiveLen = 0;
  uint8_t sendLen = 0;
  while (I2cCtrl_MapRead(&off, &type, &receive, &receiveLen, &send, &sendLen)) {
    if (type == I2C_CTRL_MAP_EXACT && receiveLen == recvLen && (recvLen == 0 || memcmp(receive, recv, recvLen) == 0)) {
      exactSend = send;
      exactLen = sendLen;
      exact = true;
      break;
    }
  }
  const uint8_t* prefixSend = NULL;
  uint8_t prefixLen = 0;
  int bestPrefix = -1;
  if (!exact) {
    off = 0;
    while (I2cCtrl_MapRead(&off, &type, &receive, &receiveLen, &send, &sendLen)) {
      if (type != I2C_CTRL_MAP_PREFIX || receiveLen > recvLen) continue;
      if (receiveLen > 0 && memcmp(receive, recv, receiveLen) != 0) continue;
      if ((int)receiveLen > bestPrefix) {
        bestPrefix = (int)receiveLen;
        prefixSend = send;
        prefixLen = sendLen;
      }
    }
  }
  const uint8_t* chosen = exact ? exactSend : prefixSend;
  uint8_t chosenLen = exact ? exactLen : (bestPrefix >= 0 ? prefixLen : 0);
  memset(armed, 0, sizeof(armed));
  armedLen = 0;
  if (chosen != NULL && chosenLen > 0) {
    memcpy(armed, chosen, chosenLen);
    armedLen = chosenLen;
  }
}

static void I2cCtrl_FlushPending(int64_t endUs, const uint8_t* send, uint16_t sendLen)
{
  if (!pendingOpen) return;
  I2cCtrl_AppendEvent(pendingStartUs, endUs, pendingRecv, pendingRecvLen, send, sendLen);
  pendingOpen = false;
  pendingRecvLen = 0;
}

static bool I2cCtrl_OnReceive(i2c_slave_dev_handle_t i2c_slave, const i2c_slave_rx_done_event_data_t* evt_data, void* arg)
{
  (void)i2c_slave;
  (void)arg;
  if (evt_data == NULL || slaveQueue == NULL) return false;
  I2cCtrl_SlaveEvt evt;
  memset(&evt, 0, sizeof(evt));
  evt.type = I2C_CTRL_EVT_RX;
  evt.timeUs = I2cCtrl_NowUs();
  uint32_t len = evt_data->length;
  if (len > CONFIG_I2C_MAX_TRANSFER_BYTES) len = CONFIG_I2C_MAX_TRANSFER_BYTES;
  evt.len = (uint16_t)len;
  if (len > 0 && evt_data->buffer != NULL) memcpy(evt.data, evt_data->buffer, len);
  BaseType_t woken = pdFALSE;
  xQueueSendFromISR(slaveQueue, &evt, &woken);
  return woken == pdTRUE;
}

static bool I2cCtrl_OnRequest(i2c_slave_dev_handle_t i2c_slave, const i2c_slave_request_event_data_t* evt_data, void* arg)
{
  (void)i2c_slave;
  (void)evt_data;
  (void)arg;
  if (slaveQueue == NULL) return false;
  I2cCtrl_SlaveEvt evt;
  memset(&evt, 0, sizeof(evt));
  evt.type = I2C_CTRL_EVT_TX;
  evt.timeUs = I2cCtrl_NowUs();
  BaseType_t woken = pdFALSE;
  xQueueSendFromISR(slaveQueue, &evt, &woken);
  return woken == pdTRUE;
}

static void I2cCtrl_HandleSlaveEvt(I2cCtrl_Runtime* u, const I2cCtrl_SlaveEvt* evt)
{
  if (evt->type == I2C_CTRL_EVT_RX) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (pendingOpen) I2cCtrl_FlushPending(evt->timeUs, NULL, 0);
    pendingRecvLen = evt->len;
    if (pendingRecvLen > 0) memcpy(pendingRecv, evt->data, pendingRecvLen);
    pendingStartUs = evt->timeUs;
    pendingOpen = true;
    I2cCtrl_ArmFromReceive(pendingRecv, pendingRecvLen);
    xSemaphoreGive(mutex);
    return;
  }
  if (evt->type != I2C_CTRL_EVT_TX || u->slaveDev == NULL) return;

  uint8_t localArmed[CONFIG_I2C_MAX_TRANSFER_BYTES];
  uint8_t localLen = 0;
  xSemaphoreTake(mutex, portMAX_DELAY);
  localLen = armedLen;
  if (localLen > 0) memcpy(localArmed, armed, localLen);
  xSemaphoreGive(mutex);

  uint32_t writeLen = 0;
  uint32_t total = 0;
  if (localLen == 0) {
    uint8_t zero = 0;
    (void)i2c_slave_write(u->slaveDev, &zero, 1, &writeLen, 50);
  } else {
    while (total < localLen) {
      writeLen = 0;
      if (i2c_slave_write(u->slaveDev, localArmed + total, localLen - total, &writeLen, 100) != ESP_OK || writeLen == 0) {
        break;
      }
      total += writeLen;
    }
  }

  xSemaphoreTake(mutex, portMAX_DELAY);
  if (pendingOpen) {
    I2cCtrl_FlushPending(evt->timeUs, localArmed, localLen);
  } else {
    I2cCtrl_AppendEvent(evt->timeUs, evt->timeUs, NULL, 0, localArmed, localLen);
  }
  xSemaphoreGive(mutex);
}

static void I2cCtrl_SlaveTask(void* arg)
{
  I2cCtrl_Runtime* u = (I2cCtrl_Runtime*)arg;
  while (!slaveStop) {
    I2cCtrl_SlaveEvt evt;
    if (xQueueReceive(slaveQueue, &evt, pdMS_TO_TICKS(50)) != pdTRUE) continue;
    if (slaveStop) break;
    I2cCtrl_HandleSlaveEvt(u, &evt);
  }
  slaveTask = NULL;
  xSemaphoreGive(slaveStopped);
  vTaskDelete(NULL);
}

static bool I2cCtrl_RequestSlaveStop(void)
{
  xSemaphoreTake(mutex, portMAX_DELAY);
  if (slaveTask == NULL) {
    xSemaphoreGive(mutex);
    return true;
  }
  while (xSemaphoreTake(slaveStopped, 0) == pdTRUE) {
  }
  slaveStop = true;
  xSemaphoreGive(mutex);
  bool stopped = xSemaphoreTake(slaveStopped, pdMS_TO_TICKS(2000)) == pdTRUE;
  xSemaphoreTake(mutex, portMAX_DELAY);
  if (stopped) slaveTask = NULL;
  if (stopped) slaveStop = false;
  xSemaphoreGive(mutex);
  if (!stopped) ESP_LOGW(tag, "slave task did not stop");
  return stopped;
}

static int I2cCtrl_HwPin(int logical)
{
  return logical >= 0 ? GpioCtrl_GetHwPin(logical) : -1;
}

static void I2cCtrl_ShutdownHardware(int id)
{
  I2cCtrl_Runtime* u = &buses[id];
  if (u->driverInstalled && u->master) {
    if (u->masterBus != NULL) {
      i2c_del_master_bus(u->masterBus);
      u->masterBus = NULL;
    }
  } else if (u->driverInstalled && u->slaveDev != NULL) {
    i2c_slave_event_callbacks_t clearCbs = {0};
    i2c_slave_register_event_callbacks(u->slaveDev, &clearCbs, NULL);
    i2c_del_slave_device(u->slaveDev);
    u->slaveDev = NULL;
  }
  u->driverInstalled = false;
  u->master = false;
  if (u->owner[0] != '\0') {
    Lock_ReleasePeripheral(u->owner);
    u->owner[0] = '\0';
  }
  I2cCtrl_DisablePins(&u->cfg);
  I2cCtrl_ClearSlaveData();
  if (slaveQueue != NULL) {
    xQueueReset(slaveQueue);
  }
}

static esp_err_t I2cCtrl_StartMaster(int id, const I2cCtrl_Config* cfg, char* reason, size_t reasonLen)
{
  i2c_master_bus_config_t bus = {
      .i2c_port = I2cCtrl_HwPort(id),
      .sda_io_num = I2cCtrl_HwPin(cfg->sdaPin),
      .scl_io_num = I2cCtrl_HwPin(cfg->sclPin),
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags =
          {
              .enable_internal_pullup = cfg->pullUp,
          },
  };
  i2c_master_bus_handle_t handle = NULL;
  esp_err_t err = i2c_new_master_bus(&bus, &handle);
  if (err != ESP_OK) {
    snprintf(reason, reasonLen, "Could not start I2C. Check pins and try again.");
    return err;
  }
  buses[id].masterBus = handle;
  buses[id].master = true;
  return ESP_OK;
}

static esp_err_t I2cCtrl_StartSlave(int id, const I2cCtrl_Config* cfg, char* reason, size_t reasonLen)
{
  if (!Tool_RingCreate(&slaveEventRing, CONFIG_I2C_SLAVE_EVENT_BYTES)) {
    snprintf(reason, reasonLen, "internal");
    return ESP_ERR_NO_MEM;
  }
  if (slaveQueue == NULL) {
    slaveQueue = xQueueCreate(16, sizeof(I2cCtrl_SlaveEvt));
    if (slaveQueue == NULL) {
      Tool_RingRelease(&slaveEventRing);
      snprintf(reason, reasonLen, "internal");
      return ESP_ERR_NO_MEM;
    }
  } else {
    xQueueReset(slaveQueue);
  }

  i2c_slave_config_t slave = {
      .i2c_port = I2cCtrl_HwPort(id),
      .sda_io_num = I2cCtrl_HwPin(cfg->sdaPin),
      .scl_io_num = I2cCtrl_HwPin(cfg->sclPin),
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .send_buf_depth = CONFIG_I2C_MAX_TRANSFER_BYTES,
      .receive_buf_depth = CONFIG_I2C_MAX_TRANSFER_BYTES,
      .slave_addr = (uint16_t)cfg->address,
      .addr_bit_len = cfg->addressBits == 10 ? I2C_ADDR_BIT_LEN_10 : I2C_ADDR_BIT_LEN_7,
      .flags =
          {
              .enable_internal_pullup = cfg->pullUp,
          },
  };
  i2c_slave_dev_handle_t handle = NULL;
  esp_err_t err = i2c_new_slave_device(&slave, &handle);
  if (err != ESP_OK) {
    Tool_RingRelease(&slaveEventRing);
    snprintf(reason, reasonLen, "Could not start I2C. Check pins and address.");
    return err;
  }
  i2c_slave_event_callbacks_t cbs = {
      .on_receive = I2cCtrl_OnReceive,
      .on_request = I2cCtrl_OnRequest,
  };
  err = i2c_slave_register_event_callbacks(handle, &cbs, NULL);
  if (err != ESP_OK) {
    i2c_del_slave_device(handle);
    Tool_RingRelease(&slaveEventRing);
    snprintf(reason, reasonLen, "Could not start I2C. Try again.");
    return err;
  }

  buses[id].slaveDev = handle;
  buses[id].master = false;
  slaveStop = false;
  pendingOpen = false;
  pendingRecvLen = 0;
  armedLen = 0;
  I2cCtrl_ArmFromReceive(NULL, 0);

  if (xTaskCreate(I2cCtrl_SlaveTask, "i2c_slave", 4096, &buses[id], 5, &slaveTask) != pdPASS) {
    i2c_slave_event_callbacks_t clearCbs = {0};
    i2c_slave_register_event_callbacks(handle, &clearCbs, NULL);
    i2c_del_slave_device(handle);
    buses[id].slaveDev = NULL;
    Tool_RingRelease(&slaveEventRing);
    slaveTask = NULL;
    snprintf(reason, reasonLen, "internal");
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

static esp_err_t I2cCtrl_Startup(int id, const I2cCtrl_Config* cfg, char* reason, size_t reasonLen)
{
  Lock_Resource resources[2];
  size_t resourceCount = 0;
  I2cCtrl_BuildPinResources(cfg, resources, &resourceCount);
  char owner[32];
  I2cCtrl_MakeOwner(id, owner, sizeof(owner));
  Lock_Conflict conflict;
  memset(&conflict, 0, sizeof(conflict));
  esp_err_t aret = Lock_AcquirePeripheral(owner, resources, resourceCount, &conflict);
  if (aret == ESP_ERR_INVALID_STATE) {
    if (conflict.isPeripheral) {
      const char* label = conflict.peripheralLabel[0] ? conflict.peripheralLabel : "a peripheral";
      snprintf(reason, reasonLen, "Pin %d is reserved by %s. Disable %s or use other pins.", conflict.pin, label, label);
    } else {
      snprintf(reason, reasonLen,
               "Pin %d is already held by a lock. DELETE that lock or wait until it expires before enabling I2C.",
               conflict.pin);
    }
    return ESP_ERR_INVALID_STATE;
  }
  if (aret != ESP_OK) {
    snprintf(reason, reasonLen, "internal");
    return aret;
  }
  I2cCtrl_DisablePins(cfg);
  esp_err_t err = cfg->role == I2C_CTRL_ROLE_SLAVE ? I2cCtrl_StartSlave(id, cfg, reason, reasonLen)
                                                   : I2cCtrl_StartMaster(id, cfg, reason, reasonLen);
  if (err != ESP_OK) {
    Lock_ReleasePeripheral(owner);
    return err;
  }
  I2cCtrl_Runtime* u = &buses[id];
  u->cfg = *cfg;
  u->cfg.enable = true;
  if (u->cfg.role == I2C_CTRL_ROLE_SLAVE) {
    u->cfg.clockHz = 0;
  } else {
    u->cfg.address = 0;
  }
  u->driverInstalled = true;
  snprintf(u->owner, sizeof(u->owner), "%s", owner);
  return ESP_OK;
}

static void I2cCtrl_StoreDisabled(int id, const I2cCtrl_Config* cfg)
{
  I2cCtrl_Runtime* u = &buses[id];
  u->cfg = *cfg;
  u->cfg.enable = false;
  if (u->cfg.role == I2C_CTRL_ROLE_SLAVE) {
    u->cfg.clockHz = 0;
    u->cfg.sclWaitUs = 0;
  } else {
    u->cfg.address = 0;
  }
  u->driverInstalled = false;
  u->master = false;
  u->masterBus = NULL;
  u->slaveDev = NULL;
  u->owner[0] = '\0';
}

esp_err_t I2cCtrl_Init(void)
{
  memset(buses, 0, sizeof(buses));
  for (size_t i = 0; i < TOOL_GET_ARRAY_LENGTH(buses); i++) {
    buses[i].cfg.enable = false;
    buses[i].cfg.role = I2C_CTRL_ROLE_MASTER;
    buses[i].cfg.clockHz = 100000;
    buses[i].cfg.address = 0;
    buses[i].cfg.addressBits = 7;
    buses[i].cfg.pullUp = true;
    buses[i].cfg.sclWaitUs = 0;
    buses[i].cfg.sdaPin = -1;
    buses[i].cfg.sclPin = -1;
  }
  I2cCtrl_ClearSlaveData();
  mutex = xSemaphoreCreateMutex();
  slaveStopped = xSemaphoreCreateBinary();
  TOOL_CHECK_OR_LOG_RETURN(mutex == NULL || slaveStopped == NULL, "mutex create failed");
  return ESP_OK;
}

esp_err_t I2cCtrl_GetConfig(int id, I2cCtrl_Config* out)
{
  if (!I2cCtrl_IsValidId(id) || out == NULL) return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(mutex, portMAX_DELAY);
  *out = buses[id].cfg;
  xSemaphoreGive(mutex);
  return ESP_OK;
}

esp_err_t I2cCtrl_SetConfig(int id, const I2cCtrl_Config* cfg, char* reason, size_t reasonLen)
{
  if (!I2cCtrl_IsValidId(id) || cfg == NULL) return ESP_ERR_INVALID_ARG;
  if (I2cCtrl_ValidateConfig(cfg, reason, reasonLen) != ESP_OK) return ESP_ERR_INVALID_ARG;

  if (!I2cCtrl_RequestSlaveStop()) {
    snprintf(reason, reasonLen, "internal");
    return ESP_FAIL;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);
  I2cCtrl_Runtime* u = &buses[id];
  I2cCtrl_Config previous = u->cfg;
  bool wasEnabled = u->driverInstalled && previous.enable;
  I2cCtrl_ShutdownHardware(id);

  if (!cfg->enable) {
    I2cCtrl_StoreDisabled(id, cfg);
    xSemaphoreGive(mutex);
    return ESP_OK;
  }

  esp_err_t err = I2cCtrl_Startup(id, cfg, reason, reasonLen);
  if (err != ESP_OK) {
    if (wasEnabled) {
      char restoreReason[160];
      if (I2cCtrl_Startup(id, &previous, restoreReason, sizeof(restoreReason)) != ESP_OK) {
        ESP_LOGW(tag, "Failed to restore I2C %d after config error: %s", id, restoreReason);
        I2cCtrl_StoreDisabled(id, &previous);
      }
    } else {
      I2cCtrl_StoreDisabled(id, cfg);
    }
    xSemaphoreGive(mutex);
    return err;
  }
  xSemaphoreGive(mutex);
  return ESP_OK;
}

static esp_err_t I2cCtrl_MapTransferErr(esp_err_t err, char* reason, size_t reasonLen)
{
  if (err == ESP_OK) return ESP_OK;
  if (err == ESP_ERR_TIMEOUT) {
    snprintf(reason, reasonLen, "Transfer timed out.");
    return ESP_ERR_TIMEOUT;
  }
  if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_RESPONSE) {
    snprintf(reason, reasonLen, "Device did not acknowledge address.");
    return ESP_ERR_INVALID_STATE;
  }
  snprintf(reason, reasonLen, "internal");
  return err;
}

esp_err_t I2cCtrl_MasterTransceive(int id, int address, const uint8_t* tx, size_t txLen, uint8_t* rx, size_t readLen,
                                   int timeoutMs, size_t* rxLen, char* reason, size_t reasonLen)
{
  if (!I2cCtrl_IsValidId(id) || rxLen == NULL) return ESP_ERR_INVALID_ARG;
  if (tx == NULL && txLen > 0) return ESP_ERR_INVALID_ARG;
  if (rx == NULL && readLen > 0) return ESP_ERR_INVALID_ARG;
  *rxLen = 0;
  if (txLen > CONFIG_I2C_MAX_TRANSFER_BYTES) {
    snprintf(reason, reasonLen, "write is too large. Send at most %u bytes per request.",
             (unsigned)CONFIG_I2C_MAX_TRANSFER_BYTES);
    return ESP_ERR_INVALID_SIZE;
  }
  if (readLen > CONFIG_I2C_MAX_TRANSFER_BYTES) {
    snprintf(reason, reasonLen, "readLen must be an integer from 0 to %u.", (unsigned)CONFIG_I2C_MAX_TRANSFER_BYTES);
    return ESP_ERR_INVALID_SIZE;
  }
  int waitMs = timeoutMs < 0 ? 1000 : timeoutMs;
  if (waitMs < CONFIG_I2C_TRANSFER_TIMEOUT_MS_MIN || waitMs > CONFIG_I2C_TRANSFER_TIMEOUT_MS_MAX) {
    snprintf(reason, reasonLen, "timeoutMs must be an integer from %d to %d.", CONFIG_I2C_TRANSFER_TIMEOUT_MS_MIN,
             CONFIG_I2C_TRANSFER_TIMEOUT_MS_MAX);
    return ESP_ERR_INVALID_ARG;
  }

  xSemaphoreTake(mutex, portMAX_DELAY);
  I2cCtrl_Runtime* u = &buses[id];
  if (!u->cfg.enable || !u->driverInstalled || !u->master || u->cfg.role != I2C_CTRL_ROLE_MASTER || u->masterBus == NULL) {
    snprintf(reason, reasonLen, "I2C %d is not an enabled master. POST /i2c/%d/config with enable true and role master.",
             id, id);
    xSemaphoreGive(mutex);
    return ESP_ERR_INVALID_STATE;
  }

  i2c_master_bus_handle_t bus = u->masterBus;
  int clockHz = u->cfg.clockHz;
  int addressBits = u->cfg.addressBits;
  int sclWaitUs = u->cfg.sclWaitUs;
  xSemaphoreGive(mutex);

  int addrMax = I2cCtrl_AddressMax(addressBits);
  if (address < 0 || address > addrMax) {
    snprintf(reason, reasonLen, "address must be an integer from 0 to %d.", addrMax);
    return ESP_ERR_INVALID_ARG;
  }

  if (txLen == 0 && readLen == 0) {
    esp_err_t probe = i2c_master_probe(bus, (uint16_t)address, waitMs);
    return I2cCtrl_MapTransferErr(probe, reason, reasonLen);
  }

  i2c_device_config_t devCfg = {
      .dev_addr_length = addressBits == 10 ? I2C_ADDR_BIT_LEN_10 : I2C_ADDR_BIT_LEN_7,
      .device_address = (uint16_t)address,
      .scl_speed_hz = (uint32_t)clockHz,
      .scl_wait_us = (uint32_t)sclWaitUs,
  };
  i2c_master_dev_handle_t dev = NULL;
  esp_err_t err = i2c_master_bus_add_device(bus, &devCfg, &dev);
  if (err != ESP_OK) {
    snprintf(reason, reasonLen, "internal");
    return err;
  }

  if (txLen > 1 && readLen > 0) {
    /* Temporary workaround for a driver command-sequencing bug: a full TX FIFO can
     * discard the repeated-start read address. Split off the final write byte to
     * drain the FIFO first while preserving the repeated start. */
    i2c_operation_job_t ops[] = {
        {.command = I2C_MASTER_CMD_START},
        {.command = I2C_MASTER_CMD_WRITE, .write = {.ack_check = true, .data = (uint8_t*)tx, .total_bytes = txLen - 1}},
        {.command = I2C_MASTER_CMD_WRITE, .write = {.ack_check = true, .data = (uint8_t*)tx + txLen - 1, .total_bytes = 1}},
        {.command = I2C_MASTER_CMD_START},
        {.command = I2C_MASTER_CMD_READ, .read = {.ack_value = I2C_ACK_VAL, .data = rx, .total_bytes = readLen - 1}},
        {.command = I2C_MASTER_CMD_READ, .read = {.ack_value = I2C_NACK_VAL, .data = rx + readLen - 1, .total_bytes = 1}},
        {.command = I2C_MASTER_CMD_STOP},
    };
    err = i2c_master_execute_defined_operations(dev, ops, TOOL_GET_ARRAY_LENGTH(ops), waitMs);
  } else if (txLen > 0 && readLen > 0) {
    err = i2c_master_transmit_receive(dev, tx, txLen, rx, readLen, waitMs);
  } else if (txLen > 0) {
    err = i2c_master_transmit(dev, tx, txLen, waitMs);
  } else {
    err = i2c_master_receive(dev, rx, readLen, waitMs);
  }
  i2c_master_bus_rm_device(dev);
  err = I2cCtrl_MapTransferErr(err, reason, reasonLen);
  if (err == ESP_OK && readLen > 0) *rxLen = readLen;
  return err;
}

static esp_err_t I2cCtrl_RequireSlave(int id, char* reason, size_t reasonLen)
{
  I2cCtrl_Runtime* u = &buses[id];
  if (!u->cfg.enable || !u->driverInstalled || u->master || u->cfg.role != I2C_CTRL_ROLE_SLAVE) {
    snprintf(reason, reasonLen, "I2C %d is not an enabled slave. POST /i2c/%d/config with enable true and role slave.", id,
             id);
    return ESP_ERR_INVALID_STATE;
  }
  return ESP_OK;
}

esp_err_t I2cCtrl_CheckSlaveReceive(int id, char* reason, size_t reasonLen)
{
  if (!I2cCtrl_IsValidId(id)) return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(mutex, portMAX_DELAY);
  esp_err_t err = I2cCtrl_RequireSlave(id, reason, reasonLen);
  xSemaphoreGive(mutex);
  return err;
}

size_t I2cCtrl_TakeSlaveEvents(int id, uint8_t* storage, size_t cap)
{
  if (!I2cCtrl_IsValidId(id) || storage == NULL || cap == 0) return 0;
  xSemaphoreTake(mutex, portMAX_DELAY);
  if (pendingOpen) I2cCtrl_FlushPending(I2cCtrl_NowUs(), NULL, 0);
  size_t copied = 0;
  while (Tool_RingLen(&slaveEventRing) > 0) {
    size_t size = 0;
    if (!I2cCtrl_PeekEventSize(&size)) {
      Tool_RingClear(&slaveEventRing);
      break;
    }
    if (size > cap) {
      Tool_RingDrop(&slaveEventRing, size);
      continue;
    }
    if (copied + size > cap) break;
    if (!Tool_RingRead(&slaveEventRing, storage + copied, size)) break;
    copied += size;
  }
  xSemaphoreGive(mutex);
  return copied;
}

bool I2cCtrl_NextSlaveEvent(const uint8_t* storage, size_t len, size_t* offset, I2cCtrl_Event* out)
{
  if (storage == NULL || offset == NULL || out == NULL || *offset > len) return false;
  if (len - *offset < sizeof(I2cCtrl_EventRec)) return false;
  I2cCtrl_EventRec rec;
  memcpy(&rec, storage + *offset, sizeof(rec));
  size_t size = I2cCtrl_EventSize(&rec);
  if (size == 0 || *offset + size > len) return false;
  const uint8_t* payload = storage + *offset + sizeof(rec);
  out->startUs = rec.startUs;
  out->endUs = rec.endUs;
  out->recvLen = rec.recvLen;
  out->sendLen = rec.sendLen;
  out->recv = payload;
  out->send = payload + rec.recvLen;
  *offset += size;
  return true;
}

esp_err_t I2cCtrl_PackMapAppend(uint8_t* buf, size_t cap, size_t* used, int index, I2cCtrl_MapType type,
                                const uint8_t* receive, size_t receiveLen, const uint8_t* send, size_t sendLen,
                                char* reason, size_t reasonLen)
{
  if (buf == NULL || used == NULL || cap == 0) return ESP_ERR_INVALID_ARG;
  if (type == I2C_CTRL_MAP_EXACT) {
    if (receiveLen < 1 || receiveLen > CONFIG_I2C_MAX_TRANSFER_BYTES) {
      snprintf(reason, reasonLen, "mapping[%d].receive must contain 1 to %u bytes when type is exact.", index,
               (unsigned)CONFIG_I2C_MAX_TRANSFER_BYTES);
      return ESP_ERR_INVALID_ARG;
    }
  } else if (type == I2C_CTRL_MAP_PREFIX) {
    if (receiveLen > CONFIG_I2C_MAX_TRANSFER_BYTES) {
      snprintf(reason, reasonLen, "mapping[%d].receive must contain 0 to %u bytes when type is prefix.", index,
               (unsigned)CONFIG_I2C_MAX_TRANSFER_BYTES);
      return ESP_ERR_INVALID_ARG;
    }
  } else {
    snprintf(reason, reasonLen, "mapping[%d].type must be one of: exact, prefix.", index);
    return ESP_ERR_INVALID_ARG;
  }
  if (sendLen > CONFIG_I2C_MAX_TRANSFER_BYTES) {
    snprintf(reason, reasonLen, "mapping[%d].send must contain 0 to %u bytes.", index,
             (unsigned)CONFIG_I2C_MAX_TRANSFER_BYTES);
    return ESP_ERR_INVALID_ARG;
  }
  if ((receiveLen > 0 && receive == NULL) || (sendLen > 0 && send == NULL)) return ESP_ERR_INVALID_ARG;
  size_t need = 3u + receiveLen + sendLen;
  if (*used > cap || cap - *used < need) {
    snprintf(reason, reasonLen, "mapping is too large. The packed map must fit in %u bytes.",
             (unsigned)CONFIG_I2C_SLAVE_MAP_BYTES);
    return ESP_ERR_INVALID_SIZE;
  }
  buf[*used] = (uint8_t)type;
  buf[*used + 1] = (uint8_t)receiveLen;
  buf[*used + 2] = (uint8_t)sendLen;
  if (receiveLen > 0) memcpy(buf + *used + 3, receive, receiveLen);
  if (sendLen > 0) memcpy(buf + *used + 3 + receiveLen, send, sendLen);
  *used += need;
  return ESP_OK;
}

esp_err_t I2cCtrl_SetSlaveMap(int id, const uint8_t* packed, size_t packedLen, char* reason, size_t reasonLen)
{
  if (!I2cCtrl_IsValidId(id)) return ESP_ERR_INVALID_ARG;
  if (packed == NULL && packedLen > 0) return ESP_ERR_INVALID_ARG;
  if (packedLen > CONFIG_I2C_SLAVE_MAP_BYTES) {
    snprintf(reason, reasonLen, "mapping is too large. The packed map must fit in %u bytes.",
             (unsigned)CONFIG_I2C_SLAVE_MAP_BYTES);
    return ESP_ERR_INVALID_SIZE;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);
  esp_err_t err = I2cCtrl_RequireSlave(id, reason, reasonLen);
  if (err == ESP_OK) {
    memset(mapBuf, 0, sizeof(mapBuf));
    mapUsed = packedLen;
    if (packedLen > 0) memcpy(mapBuf, packed, packedLen);
    I2cCtrl_ArmFromReceive(pendingOpen ? pendingRecv : NULL, pendingOpen ? pendingRecvLen : 0);
  }
  xSemaphoreGive(mutex);
  return err;
}
