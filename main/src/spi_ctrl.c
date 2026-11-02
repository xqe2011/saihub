/**
 * @name SPI control module
 * @file spi_ctrl.c
 * @author xqe2011
 */
#include "spi_ctrl.h"

#include "gpio_ctrl.h"
#include "lock.h"
#include "tool.h"

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <driver/spi_slave.h>
#include <esp_private/spi_slave_internal.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static const char* tag = "SAIHub-Spi";

static const int spiIdToHost[] = CONFIG_SPI_ID_TO_HOST;

typedef struct {
  SpiCtrl_Config cfg;
  bool driverInstalled;
  bool master;
  spi_device_handle_t masterDev;
  gpio_drive_cap_t originalClockDrive;
  bool clockDriveSaved;
  char owner[32];
} SpiCtrl_Runtime;

static SpiCtrl_Runtime buses[TOOL_GET_ARRAY_LENGTH(spiIdToHost)];
static SemaphoreHandle_t mutex;
static SemaphoreHandle_t slaveStopped;

/* Variable-length records. Allocated while the slave is enabled. */
static Tool_Ring slaveEventRing;

static uint8_t mapBuf[CONFIG_SPI_SLAVE_MAP_BYTES];
static size_t mapUsed;
static uint8_t armed[CONFIG_SPI_MAX_TRANSFER_BYTES];
static uint8_t armedLen;

/* Pad both DMA buffer slots to four-byte alignment. */
static DMA_ATTR uint8_t slaveRx[2][(CONFIG_SPI_MAX_TRANSFER_BYTES + 3) & ~3];
static DMA_ATTR uint8_t slaveTx[2][(CONFIG_SPI_MAX_TRANSFER_BYTES + 3) & ~3];
static spi_slave_transaction_t slaveTrans[2];
static int slaveSlot;
static spi_host_device_t slaveHost;
static volatile bool slaveStop;
static TaskHandle_t slaveTask;
static int slaveCsHw = -1;
static bool slaveIsrOn;

static portMUX_TYPE csMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t csEpoch;
static volatile uint32_t csSeenEpoch;
static volatile int64_t csStartUs;

bool SpiCtrl_IsValidId(int id)
{
  return id >= 0 && id < (int)TOOL_GET_ARRAY_LENGTH(spiIdToHost);
}

int SpiCtrl_GetCount(void)
{
  return (int)TOOL_GET_ARRAY_LENGTH(spiIdToHost);
}

const char* SpiCtrl_RoleToString(SpiCtrl_Role role)
{
  return role == SPI_CTRL_ROLE_SLAVE ? "slave" : "master";
}

bool SpiCtrl_RoleFromString(const char* s, SpiCtrl_Role* out)
{
  if (s == NULL || out == NULL) return false;
  if (strcmp(s, "master") == 0) {
    *out = SPI_CTRL_ROLE_MASTER;
    return true;
  }
  if (strcmp(s, "slave") == 0) {
    *out = SPI_CTRL_ROLE_SLAVE;
    return true;
  }
  return false;
}

const char* SpiCtrl_BitOrderToString(SpiCtrl_BitOrder order)
{
  return order == SPI_CTRL_BIT_LSB ? "lsb" : "msb";
}

bool SpiCtrl_BitOrderFromString(const char* s, SpiCtrl_BitOrder* out)
{
  if (s == NULL || out == NULL) return false;
  if (strcmp(s, "msb") == 0) {
    *out = SPI_CTRL_BIT_MSB;
    return true;
  }
  if (strcmp(s, "lsb") == 0) {
    *out = SPI_CTRL_BIT_LSB;
    return true;
  }
  return false;
}

static spi_host_device_t SpiCtrl_HwHost(int id)
{
  return (spi_host_device_t)spiIdToHost[id];
}

static int64_t SpiCtrl_NowUs(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
}

static void SpiCtrl_CsIsr(void* arg)
{
  (void)arg;
  portENTER_CRITICAL_ISR(&csMux);
  uint32_t epoch = csEpoch;
  if (csSeenEpoch != epoch) {
    csStartUs = esp_timer_get_time();
    csSeenEpoch = epoch;
  }
  portEXIT_CRITICAL_ISR(&csMux);
}

static void SpiCtrl_ArmCsEpoch(void)
{
  portENTER_CRITICAL(&csMux);
  csEpoch++;
  if (csEpoch == 0) csEpoch = 1;
  portEXIT_CRITICAL(&csMux);
}

static int64_t SpiCtrl_TakeCsStart(int64_t endUs)
{
  int64_t start = endUs;
  portENTER_CRITICAL(&csMux);
  if (csSeenEpoch == csEpoch) start = csStartUs;
  portEXIT_CRITICAL(&csMux);
  return start;
}

static void SpiCtrl_MakeOwner(int id, char* out, size_t outLen)
{
  snprintf(out, outLen, "spi:%d", id);
}

static int SpiCtrl_ClockEdges(const SpiCtrl_Config* cfg)
{
  return ((cfg->cpol & 1) << 1) | (cfg->cpha & 1);
}

static bool SpiCtrl_PinOk(int pin)
{
  return pin < 0 || GpioCtrl_IsValidLogicalPin(pin);
}

static esp_err_t SpiCtrl_ValidateConfig(const SpiCtrl_Config* cfg, char* reason, size_t reasonLen)
{
  if (cfg == NULL) return ESP_ERR_INVALID_ARG;
  if (cfg->cpol != 0 && cfg->cpol != 1) {
    snprintf(reason, reasonLen, "cpol must be 0 or 1.");
    return ESP_ERR_INVALID_ARG;
  }
  if (cfg->cpha != 0 && cfg->cpha != 1) {
    snprintf(reason, reasonLen, "cpha must be 0 or 1.");
    return ESP_ERR_INVALID_ARG;
  }
  if (cfg->role != SPI_CTRL_ROLE_MASTER && cfg->role != SPI_CTRL_ROLE_SLAVE) {
    snprintf(reason, reasonLen, "role must be one of: master, slave.");
    return ESP_ERR_INVALID_ARG;
  }
  if (cfg->role == SPI_CTRL_ROLE_SLAVE) {
    if (cfg->clockHz != 0) {
      snprintf(reason, reasonLen, "clockHz is not supported when role is slave.");
      return ESP_ERR_INVALID_ARG;
    }
  } else if (cfg->clockHz < CONFIG_SPI_CLOCK_HZ_MIN || cfg->clockHz > CONFIG_SPI_CLOCK_HZ_MAX) {
    snprintf(reason, reasonLen, "clockHz must be an integer from %d to %d.", CONFIG_SPI_CLOCK_HZ_MIN,
             CONFIG_SPI_CLOCK_HZ_MAX);
    return ESP_ERR_INVALID_ARG;
  }
  const int pins[] = {cfg->mosiPin, cfg->misoPin, cfg->clkPin, cfg->csPin};
  const char* names[] = {"mosi", "miso", "clk", "cs"};
  for (size_t i = 0; i < 4; i++) {
    if (!SpiCtrl_PinOk(pins[i])) {
      snprintf(reason, reasonLen, "pins.%s is invalid. Use a pin from 0 to %d or omit it.", names[i],
               GpioCtrl_GetLogicalCount() - 1);
      return ESP_ERR_INVALID_ARG;
    }
  }
  for (size_t i = 0; i < 4; i++) {
    if (pins[i] < 0) continue;
    for (size_t j = i + 1; j < 4; j++) {
      if (pins[j] >= 0 && pins[i] == pins[j]) {
        snprintf(reason, reasonLen, "pins.%s and pins.%s must be different when both are set.", names[i], names[j]);
        return ESP_ERR_INVALID_ARG;
      }
    }
  }
  if (cfg->enable && (cfg->clkPin < 0 || cfg->csPin < 0)) {
    snprintf(reason, reasonLen, "enable true requires pins.clk and pins.cs.");
    return ESP_ERR_INVALID_ARG;
  }
  return ESP_OK;
}

static void SpiCtrl_AddPinResource(Lock_Resource* out, size_t* count, int pin)
{
  if (pin < 0) return;
  out[*count].kind = LOCK_KIND_GPIO;
  out[*count].pin = pin;
  out[*count].methods = LOCK_METHOD_READ | LOCK_METHOD_WRITE;
  (*count)++;
}

static void SpiCtrl_BuildPinResources(const SpiCtrl_Config* cfg, Lock_Resource* out, size_t* countOut)
{
  size_t n = 0;
  SpiCtrl_AddPinResource(out, &n, cfg->mosiPin);
  SpiCtrl_AddPinResource(out, &n, cfg->misoPin);
  SpiCtrl_AddPinResource(out, &n, cfg->clkPin);
  SpiCtrl_AddPinResource(out, &n, cfg->csPin);
  *countOut = n;
}

static void SpiCtrl_DisablePins(const SpiCtrl_Config* cfg)
{
  if (cfg->mosiPin >= 0) GpioCtrl_SetConfig(cfg->mosiPin, GPIO_CTRL_MODE_DISABLE, false, false, false);
  if (cfg->misoPin >= 0) GpioCtrl_SetConfig(cfg->misoPin, GPIO_CTRL_MODE_DISABLE, false, false, false);
  if (cfg->clkPin >= 0) GpioCtrl_SetConfig(cfg->clkPin, GPIO_CTRL_MODE_DISABLE, false, false, false);
  if (cfg->csPin >= 0) GpioCtrl_SetConfig(cfg->csPin, GPIO_CTRL_MODE_DISABLE, false, false, false);
}

static void SpiCtrl_ClearSlaveData(void)
{
  Tool_RingRelease(&slaveEventRing);
  mapUsed = 0;
  armedLen = 0;
  memset(armed, 0, sizeof(armed));
  memset(mapBuf, 0, sizeof(mapBuf));
}

static void SpiCtrl_DetachCsIsr(void)
{
  if (!slaveIsrOn || slaveCsHw < 0) {
    slaveIsrOn = false;
    slaveCsHw = -1;
    return;
  }
  gpio_isr_handler_remove((gpio_num_t)slaveCsHw);
  slaveIsrOn = false;
  slaveCsHw = -1;
}

typedef struct __attribute__((packed)) {
  int64_t startUs;
  int64_t endUs;
  uint16_t recvLen;
  uint16_t sendLen;
} SpiCtrl_EventRec;

_Static_assert(sizeof(SpiCtrl_EventRec) + 2 * CONFIG_SPI_MAX_TRANSFER_BYTES <= CONFIG_SPI_SLAVE_EVENT_BYTES,
               "SPI slave event must fit in the event buffer");

static size_t SpiCtrl_EventSize(const SpiCtrl_EventRec* rec)
{
  if (rec->recvLen > CONFIG_SPI_MAX_TRANSFER_BYTES || rec->sendLen > CONFIG_SPI_MAX_TRANSFER_BYTES) return 0;
  return sizeof(*rec) + (size_t)rec->recvLen + (size_t)rec->sendLen;
}

static bool SpiCtrl_PeekEventSize(size_t* sizeOut)
{
  SpiCtrl_EventRec rec;
  if (!Tool_RingPeek(&slaveEventRing, &rec, sizeof(rec))) return false;
  size_t size = SpiCtrl_EventSize(&rec);
  if (size == 0 || size > Tool_RingLen(&slaveEventRing)) return false;
  *sizeOut = size;
  return true;
}

static void SpiCtrl_DropOldestEvent(void)
{
  size_t size = 0;
  if (!SpiCtrl_PeekEventSize(&size) || !Tool_RingDrop(&slaveEventRing, size)) Tool_RingClear(&slaveEventRing);
}

static void SpiCtrl_AppendEvent(int64_t startUs, int64_t endUs, const uint8_t* recv, uint16_t recvLen, const uint8_t* send,
                                uint16_t sendLen)
{
  if (recv == NULL) recvLen = 0;
  if (send == NULL) sendLen = 0;
  if (recvLen > CONFIG_SPI_MAX_TRANSFER_BYTES) recvLen = CONFIG_SPI_MAX_TRANSFER_BYTES;
  if (sendLen > CONFIG_SPI_MAX_TRANSFER_BYTES) sendLen = CONFIG_SPI_MAX_TRANSFER_BYTES;
  SpiCtrl_EventRec rec = {.startUs = startUs, .endUs = endUs, .recvLen = recvLen, .sendLen = sendLen};
  size_t need = SpiCtrl_EventSize(&rec);
  if (need == 0 || need > slaveEventRing.cap) return;
  while (Tool_RingFree(&slaveEventRing) < need) SpiCtrl_DropOldestEvent();
  bool ok = Tool_RingWrite(&slaveEventRing, &rec, sizeof(rec));
  if (ok && recvLen > 0) ok = Tool_RingWrite(&slaveEventRing, recv, recvLen);
  if (ok && sendLen > 0) ok = Tool_RingWrite(&slaveEventRing, send, sendLen);
  if (!ok) Tool_RingClear(&slaveEventRing);
}

static bool SpiCtrl_MapRead(size_t* off, uint8_t* type, const uint8_t** mosi, uint8_t* mosiLen, const uint8_t** miso,
                            uint8_t* misoLen)
{
  if (*off + 3 > mapUsed) return false;
  *type = mapBuf[*off];
  *mosiLen = mapBuf[*off + 1];
  *misoLen = mapBuf[*off + 2];
  size_t need = 3u + *mosiLen + *misoLen;
  if (*off + need > mapUsed) {
    return false;
  }
  *mosi = mapBuf + *off + 3;
  *miso = *mosi + *mosiLen;
  *off += need;
  return true;
}

static void SpiCtrl_ArmFromReceive(const uint8_t* recv, uint16_t recvLen)
{
  const uint8_t* exactMiso = NULL;
  uint8_t exactLen = 0;
  bool exact = false;
  size_t off = 0;
  uint8_t type = 0;
  const uint8_t* mosi = NULL;
  const uint8_t* miso = NULL;
  uint8_t mosiLen = 0;
  uint8_t misoLen = 0;
  while (SpiCtrl_MapRead(&off, &type, &mosi, &mosiLen, &miso, &misoLen)) {
    if (type == SPI_CTRL_MAP_EXACT && mosiLen == recvLen && (recvLen == 0 || memcmp(mosi, recv, recvLen) == 0)) {
      exactMiso = miso;
      exactLen = misoLen;
      exact = true;
      break;
    }
  }
  const uint8_t* prefixMiso = NULL;
  uint8_t prefixLen = 0;
  int bestPrefix = -1;
  if (!exact) {
    off = 0;
    while (SpiCtrl_MapRead(&off, &type, &mosi, &mosiLen, &miso, &misoLen)) {
      if (type != SPI_CTRL_MAP_PREFIX || mosiLen > recvLen) continue;
      if (mosiLen > 0 && memcmp(mosi, recv, mosiLen) != 0) continue;
      if ((int)mosiLen > bestPrefix) {
        bestPrefix = (int)mosiLen;
        prefixMiso = miso;
        prefixLen = misoLen;
      }
    }
  }
  const uint8_t* chosen = exact ? exactMiso : prefixMiso;
  uint8_t chosenLen = exact ? exactLen : (bestPrefix >= 0 ? prefixLen : 0);
  memset(armed, 0, sizeof(armed));
  armedLen = 0;
  if (chosen != NULL && chosenLen > 0) {
    memcpy(armed, chosen, chosenLen);
    armedLen = chosenLen;
  }
}

static void SpiCtrl_FillTx(int slot)
{
  memset(slaveTx[slot], 0, sizeof(slaveTx[slot]));
  if (armedLen > 0) memcpy(slaveTx[slot], armed, armedLen);
  memset(slaveRx[slot], 0, sizeof(slaveRx[slot]));
  memset(&slaveTrans[slot], 0, sizeof(slaveTrans[slot]));
  slaveTrans[slot].length = CONFIG_SPI_MAX_TRANSFER_BYTES * 8;
  slaveTrans[slot].tx_buffer = slaveTx[slot];
  slaveTrans[slot].rx_buffer = slaveRx[slot];
  slaveTrans[slot].user = (void*)(intptr_t)slot;
}

static void SpiCtrl_HandleSlaveResult(void)
{
  spi_slave_transaction_t* done = &slaveTrans[slaveSlot];
  size_t nbytes = done->trans_len / 8;
  if (nbytes > CONFIG_SPI_MAX_TRANSFER_BYTES) nbytes = CONFIG_SPI_MAX_TRANSFER_BYTES;
  int64_t endUs = SpiCtrl_NowUs();
  int64_t startUs = SpiCtrl_TakeCsStart(endUs);
  if (nbytes > 0) {
    SpiCtrl_AppendEvent(startUs, endUs, slaveRx[slaveSlot], (uint16_t)nbytes, slaveTx[slaveSlot], (uint16_t)nbytes);
  }
  SpiCtrl_ArmFromReceive(slaveRx[slaveSlot], (uint16_t)nbytes);
}

static void SpiCtrl_SlaveTask(void* arg)
{
  (void)arg;
  while (!slaveStop) {
    spi_slave_transaction_t* done = NULL;
    esp_err_t err = spi_slave_get_trans_result(slaveHost, &done, pdMS_TO_TICKS(50));
    if (slaveStop) break;
    if (err == ESP_ERR_TIMEOUT) continue;
    if (err != ESP_OK || done == NULL) continue;
    int next = slaveSlot ^ 1;
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (!slaveStop) {
      SpiCtrl_HandleSlaveResult();
      SpiCtrl_FillTx(next);
    }
    bool stopping = slaveStop;
    xSemaphoreGive(mutex);
    if (stopping) break;
    while (!slaveStop) {
      SpiCtrl_ArmCsEpoch();
      err = spi_slave_queue_trans(slaveHost, &slaveTrans[next], pdMS_TO_TICKS(100));
      if (err == ESP_OK) {
        slaveSlot = next;
        break;
      }
      ESP_LOGW(tag, "slave requeue failed: %s", esp_err_to_name(err));
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
  slaveTask = NULL;
  xSemaphoreGive(slaveStopped);
  vTaskDelete(NULL);
}

static bool SpiCtrl_RequestSlaveStop(void)
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

static void SpiCtrl_ShutdownHardware(int id)
{
  SpiCtrl_Runtime* u = &buses[id];
  SpiCtrl_DetachCsIsr();
  if (u->driverInstalled && u->master) {
    if (u->masterDev != NULL) {
      spi_bus_remove_device(u->masterDev);
      u->masterDev = NULL;
    }
    spi_bus_free(SpiCtrl_HwHost(id));
  } else if (u->driverInstalled) {
    spi_slave_queue_reset(SpiCtrl_HwHost(id));
    spi_slave_free(SpiCtrl_HwHost(id));
  }
  u->driverInstalled = false;
  u->master = false;
  if (u->owner[0] != '\0') {
    Lock_ReleasePeripheral(u->owner);
    u->owner[0] = '\0';
  }
  SpiCtrl_DisablePins(&u->cfg);
  if (u->clockDriveSaved) {
    gpio_set_drive_capability(GpioCtrl_GetHwPin(u->cfg.clkPin), u->originalClockDrive);
    u->clockDriveSaved = false;
  }
  SpiCtrl_ClearSlaveData();
}

static int SpiCtrl_HwPin(int logical)
{
  return logical >= 0 ? GpioCtrl_GetHwPin(logical) : -1;
}

static esp_err_t SpiCtrl_StartMaster(int id, const SpiCtrl_Config* cfg, char* reason, size_t reasonLen)
{
  spi_bus_config_t bus = {
      .mosi_io_num = SpiCtrl_HwPin(cfg->mosiPin),
      .miso_io_num = SpiCtrl_HwPin(cfg->misoPin),
      .sclk_io_num = SpiCtrl_HwPin(cfg->clkPin),
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = CONFIG_SPI_MAX_TRANSFER_BYTES,
  };
  spi_host_device_t host = SpiCtrl_HwHost(id);
  esp_err_t err = spi_bus_initialize(host, &bus, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    snprintf(reason, reasonLen, "Could not start SPI. Check pins and try again.");
    return err;
  }
  spi_device_interface_config_t dev = {
      .clock_speed_hz = cfg->clockHz,
      .mode = SpiCtrl_ClockEdges(cfg),
      .spics_io_num = SpiCtrl_HwPin(cfg->csPin),
      .queue_size = 1,
      .flags = cfg->bitOrder == SPI_CTRL_BIT_LSB ? SPI_DEVICE_BIT_LSBFIRST : 0,
  };
  spi_device_handle_t handle = NULL;
  err = spi_bus_add_device(host, &dev, &handle);
  if (err != ESP_OK) {
    spi_bus_free(host);
    snprintf(reason, reasonLen, "Could not start SPI. Check pins, cpol, cpha, and clockHz.");
    return err;
  }
  gpio_drive_cap_t originalDrive;
  err = gpio_get_drive_capability(SpiCtrl_HwPin(cfg->clkPin), &originalDrive);
  if (err == ESP_OK) {
    /* Use gentler clock edges for reliable pin-to-pin transfers. */
    err = gpio_set_drive_capability(SpiCtrl_HwPin(cfg->clkPin), GPIO_DRIVE_CAP_0);
  }
  if (err != ESP_OK) {
    spi_bus_remove_device(handle);
    spi_bus_free(host);
    snprintf(reason, reasonLen, "Could not configure the SPI clock pin. Check pins and try again.");
    return err;
  }
  buses[id].originalClockDrive = originalDrive;
  buses[id].clockDriveSaved = true;
  buses[id].masterDev = handle;
  buses[id].master = true;
  return ESP_OK;
}

static esp_err_t SpiCtrl_StartSlave(int id, const SpiCtrl_Config* cfg, char* reason, size_t reasonLen)
{
  spi_bus_config_t bus = {
      .mosi_io_num = SpiCtrl_HwPin(cfg->mosiPin),
      .miso_io_num = SpiCtrl_HwPin(cfg->misoPin),
      .sclk_io_num = SpiCtrl_HwPin(cfg->clkPin),
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = CONFIG_SPI_MAX_TRANSFER_BYTES,
  };
  spi_slave_interface_config_t slave = {
      .spics_io_num = SpiCtrl_HwPin(cfg->csPin),
      .flags = cfg->bitOrder == SPI_CTRL_BIT_LSB ? SPI_SLAVE_BIT_LSBFIRST : 0,
      .queue_size = 2,
      .mode = SpiCtrl_ClockEdges(cfg),
      .post_setup_cb = NULL,
      .post_trans_cb = NULL,
  };
  spi_host_device_t host = SpiCtrl_HwHost(id);
  esp_err_t err = spi_slave_initialize(host, &bus, &slave, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    snprintf(reason, reasonLen, "Could not start SPI. Check pins, cpol, and cpha.");
    return err;
  }
  if (!Tool_RingCreate(&slaveEventRing, CONFIG_SPI_SLAVE_EVENT_BYTES)) {
    spi_slave_free(host);
    snprintf(reason, reasonLen, "internal");
    return ESP_ERR_NO_MEM;
  }
  slaveHost = host;
  slaveSlot = 0;
  slaveStop = false;
  SpiCtrl_FillTx(0);
  SpiCtrl_ArmCsEpoch();
  err = spi_slave_queue_trans(host, &slaveTrans[0], pdMS_TO_TICKS(100));
  if (err != ESP_OK) {
    Tool_RingRelease(&slaveEventRing);
    spi_slave_free(host);
    snprintf(reason, reasonLen, "Could not start SPI. Try again.");
    return err;
  }
  slaveCsHw = SpiCtrl_HwPin(cfg->csPin);
  if (slaveCsHw >= 0 && gpio_set_intr_type((gpio_num_t)slaveCsHw, GPIO_INTR_NEGEDGE) == ESP_OK &&
      gpio_isr_handler_add((gpio_num_t)slaveCsHw, SpiCtrl_CsIsr, NULL) == ESP_OK) {
    slaveIsrOn = true;
  } else {
    slaveIsrOn = false;
  }
  if (xTaskCreate(SpiCtrl_SlaveTask, "spi_slave", 4096, NULL, 5, &slaveTask) != pdPASS) {
    Tool_RingRelease(&slaveEventRing);
    SpiCtrl_DetachCsIsr();
    spi_slave_queue_reset(host);
    spi_slave_free(host);
    slaveTask = NULL;
    snprintf(reason, reasonLen, "internal");
    return ESP_ERR_NO_MEM;
  }
  buses[id].master = false;
  return ESP_OK;
}

static esp_err_t SpiCtrl_Startup(int id, const SpiCtrl_Config* cfg, char* reason, size_t reasonLen)
{
  Lock_Resource resources[4];
  size_t resourceCount = 0;
  SpiCtrl_BuildPinResources(cfg, resources, &resourceCount);
  char owner[32];
  SpiCtrl_MakeOwner(id, owner, sizeof(owner));
  Lock_Conflict conflict;
  memset(&conflict, 0, sizeof(conflict));
  esp_err_t aret = Lock_AcquirePeripheral(owner, resources, resourceCount, &conflict);
  if (aret == ESP_ERR_INVALID_STATE) {
    if (conflict.isPeripheral) {
      const char* label = conflict.peripheralLabel[0] ? conflict.peripheralLabel : "a peripheral";
      snprintf(reason, reasonLen, "Pin %d is reserved by %s. Disable %s or use other pins.", conflict.pin, label, label);
    } else {
      snprintf(reason, reasonLen,
               "Pin %d is already held by a lock. DELETE that lock or wait until it expires before enabling SPI.",
               conflict.pin);
    }
    return ESP_ERR_INVALID_STATE;
  }
  if (aret != ESP_OK) {
    snprintf(reason, reasonLen, "internal");
    return aret;
  }
  SpiCtrl_DisablePins(cfg);
  esp_err_t err = cfg->role == SPI_CTRL_ROLE_SLAVE ? SpiCtrl_StartSlave(id, cfg, reason, reasonLen)
                                                   : SpiCtrl_StartMaster(id, cfg, reason, reasonLen);
  if (err != ESP_OK) {
    Lock_ReleasePeripheral(owner);
    return err;
  }
  SpiCtrl_Runtime* u = &buses[id];
  u->cfg = *cfg;
  u->cfg.enable = true;
  if (u->cfg.role == SPI_CTRL_ROLE_SLAVE) u->cfg.clockHz = 0;
  u->driverInstalled = true;
  snprintf(u->owner, sizeof(u->owner), "%s", owner);
  return ESP_OK;
}

static void SpiCtrl_StoreDisabled(int id, const SpiCtrl_Config* cfg)
{
  SpiCtrl_Runtime* u = &buses[id];
  u->cfg = *cfg;
  u->cfg.enable = false;
  if (u->cfg.role == SPI_CTRL_ROLE_SLAVE) u->cfg.clockHz = 0;
  u->driverInstalled = false;
  u->master = false;
  u->masterDev = NULL;
  u->owner[0] = '\0';
}

esp_err_t SpiCtrl_Init(void)
{
  memset(buses, 0, sizeof(buses));
  for (size_t i = 0; i < TOOL_GET_ARRAY_LENGTH(buses); i++) {
    buses[i].cfg.enable = false;
    buses[i].cfg.role = SPI_CTRL_ROLE_MASTER;
    buses[i].cfg.cpol = 0;
    buses[i].cfg.cpha = 0;
    buses[i].cfg.bitOrder = SPI_CTRL_BIT_MSB;
    buses[i].cfg.clockHz = 1000000;
    buses[i].cfg.mosiPin = -1;
    buses[i].cfg.misoPin = -1;
    buses[i].cfg.clkPin = -1;
    buses[i].cfg.csPin = -1;
  }
  SpiCtrl_ClearSlaveData();
  mutex = xSemaphoreCreateMutex();
  slaveStopped = xSemaphoreCreateBinary();
  TOOL_CHECK_OR_LOG_RETURN(mutex == NULL || slaveStopped == NULL, "mutex create failed");
  return ESP_OK;
}

esp_err_t SpiCtrl_GetConfig(int id, SpiCtrl_Config* out)
{
  if (!SpiCtrl_IsValidId(id) || out == NULL) return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(mutex, portMAX_DELAY);
  *out = buses[id].cfg;
  xSemaphoreGive(mutex);
  return ESP_OK;
}

esp_err_t SpiCtrl_SetConfig(int id, const SpiCtrl_Config* cfg, char* reason, size_t reasonLen)
{
  if (!SpiCtrl_IsValidId(id) || cfg == NULL) return ESP_ERR_INVALID_ARG;
  if (SpiCtrl_ValidateConfig(cfg, reason, reasonLen) != ESP_OK) return ESP_ERR_INVALID_ARG;

  if (!SpiCtrl_RequestSlaveStop()) {
    snprintf(reason, reasonLen, "internal");
    return ESP_FAIL;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);
  SpiCtrl_Runtime* u = &buses[id];
  SpiCtrl_Config previous = u->cfg;
  bool wasEnabled = u->driverInstalled && previous.enable;
  SpiCtrl_ShutdownHardware(id);

  if (!cfg->enable) {
    SpiCtrl_StoreDisabled(id, cfg);
    xSemaphoreGive(mutex);
    return ESP_OK;
  }

  esp_err_t err = SpiCtrl_Startup(id, cfg, reason, reasonLen);
  if (err != ESP_OK) {
    if (wasEnabled) {
      char restoreReason[160];
      if (SpiCtrl_Startup(id, &previous, restoreReason, sizeof(restoreReason)) != ESP_OK) {
        ESP_LOGW(tag, "Failed to restore SPI %d after config error: %s", id, restoreReason);
        SpiCtrl_StoreDisabled(id, &previous);
      }
    } else {
      SpiCtrl_StoreDisabled(id, cfg);
    }
    xSemaphoreGive(mutex);
    return err;
  }
  xSemaphoreGive(mutex);
  return ESP_OK;
}

esp_err_t SpiCtrl_MasterTransceive(int id, const uint8_t* tx, size_t txLen, uint8_t* rx, size_t* rxLen, char* reason,
                                   size_t reasonLen)
{
  if (!SpiCtrl_IsValidId(id) || rxLen == NULL) return ESP_ERR_INVALID_ARG;
  if (tx == NULL && txLen > 0) return ESP_ERR_INVALID_ARG;
  *rxLen = 0;
  if (txLen > CONFIG_SPI_MAX_TRANSFER_BYTES) {
    snprintf(reason, reasonLen, "data is too large. Send at most %u bytes per request.",
             (unsigned)CONFIG_SPI_MAX_TRANSFER_BYTES);
    return ESP_ERR_INVALID_SIZE;
  }

  xSemaphoreTake(mutex, portMAX_DELAY);
  SpiCtrl_Runtime* u = &buses[id];
  if (!u->cfg.enable || !u->driverInstalled || !u->master || u->cfg.role != SPI_CTRL_ROLE_MASTER) {
    snprintf(reason, reasonLen, "SPI %d is not an enabled master. POST /spi/%d/config with enable true and role master.",
             id, id);
    xSemaphoreGive(mutex);
    return ESP_ERR_INVALID_STATE;
  }
  if (txLen > 0 && u->cfg.mosiPin < 0) {
    snprintf(reason, reasonLen, "SPI %d has no MOSI pin. POST /spi/%d/config with pins.mosi set before sending data.", id,
             id);
    xSemaphoreGive(mutex);
    return ESP_ERR_INVALID_STATE;
  }
  if (txLen == 0) {
    xSemaphoreGive(mutex);
    return ESP_OK;
  }
  bool wantRx = u->cfg.misoPin >= 0;
  if (wantRx && rx == NULL) {
    xSemaphoreGive(mutex);
    snprintf(reason, reasonLen, "internal");
    return ESP_ERR_INVALID_ARG;
  }
  uint8_t* dmaTx = heap_caps_malloc(txLen, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  uint8_t* dmaRx = wantRx ? heap_caps_malloc(txLen, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) : NULL;
  if (dmaTx == NULL || (wantRx && dmaRx == NULL)) {
    free(dmaTx);
    free(dmaRx);
    xSemaphoreGive(mutex);
    snprintf(reason, reasonLen, "internal");
    return ESP_ERR_NO_MEM;
  }
  memcpy(dmaTx, tx, txLen);
  if (dmaRx != NULL) memset(dmaRx, 0, txLen);
  spi_transaction_t trans;
  memset(&trans, 0, sizeof(trans));
  trans.length = txLen * 8;
  trans.tx_buffer = dmaTx;
  trans.rx_buffer = dmaRx;
  esp_err_t err = spi_device_transmit(u->masterDev, &trans);
  if (err == ESP_OK && wantRx) {
    memcpy(rx, dmaRx, txLen);
    *rxLen = txLen;
  }
  free(dmaTx);
  free(dmaRx);
  xSemaphoreGive(mutex);
  if (err != ESP_OK) {
    snprintf(reason, reasonLen, "internal");
    return err;
  }
  return ESP_OK;
}

static esp_err_t SpiCtrl_RequireSlave(int id, char* reason, size_t reasonLen)
{
  SpiCtrl_Runtime* u = &buses[id];
  if (!u->cfg.enable || !u->driverInstalled || u->master || u->cfg.role != SPI_CTRL_ROLE_SLAVE) {
    snprintf(reason, reasonLen, "SPI %d is not an enabled slave. POST /spi/%d/config with enable true and role slave.",
             id, id);
    return ESP_ERR_INVALID_STATE;
  }
  return ESP_OK;
}

esp_err_t SpiCtrl_CheckSlaveReceive(int id, char* reason, size_t reasonLen)
{
  if (!SpiCtrl_IsValidId(id)) return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(mutex, portMAX_DELAY);
  esp_err_t err = SpiCtrl_RequireSlave(id, reason, reasonLen);
  if (err == ESP_OK && buses[id].cfg.misoPin < 0) {
    snprintf(reason, reasonLen, "SPI %d has no MISO pin. POST /spi/%d/config with pins.miso set before reading slave events.",
             id, id);
    err = ESP_ERR_INVALID_STATE;
  }
  xSemaphoreGive(mutex);
  return err;
}

size_t SpiCtrl_TakeSlaveEvents(int id, uint8_t* storage, size_t cap)
{
  if (!SpiCtrl_IsValidId(id) || storage == NULL || cap == 0) return 0;
  xSemaphoreTake(mutex, portMAX_DELAY);
  size_t copied = 0;
  while (Tool_RingLen(&slaveEventRing) > 0) {
    size_t size = 0;
    if (!SpiCtrl_PeekEventSize(&size)) {
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

bool SpiCtrl_NextSlaveEvent(const uint8_t* storage, size_t len, size_t* offset, SpiCtrl_Event* out)
{
  if (storage == NULL || offset == NULL || out == NULL || *offset > len) return false;
  if (len - *offset < sizeof(SpiCtrl_EventRec)) return false;
  SpiCtrl_EventRec rec;
  memcpy(&rec, storage + *offset, sizeof(rec));
  size_t size = SpiCtrl_EventSize(&rec);
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

esp_err_t SpiCtrl_PackMapAppend(uint8_t* buf, size_t cap, size_t* used, int index, SpiCtrl_MapType type,
                                const uint8_t* mosi, size_t mosiLen, const uint8_t* miso, size_t misoLen, char* reason,
                                size_t reasonLen)
{
  if (buf == NULL || used == NULL || cap == 0) return ESP_ERR_INVALID_ARG;
  if (type == SPI_CTRL_MAP_EXACT) {
    if (mosiLen < 1 || mosiLen > CONFIG_SPI_MAX_TRANSFER_BYTES) {
      snprintf(reason, reasonLen, "mapping[%d].mosi must contain 1 to %u bytes when type is exact.", index,
               (unsigned)CONFIG_SPI_MAX_TRANSFER_BYTES);
      return ESP_ERR_INVALID_ARG;
    }
  } else if (type == SPI_CTRL_MAP_PREFIX) {
    if (mosiLen > CONFIG_SPI_MAX_TRANSFER_BYTES) {
      snprintf(reason, reasonLen, "mapping[%d].mosi must contain 0 to %u bytes when type is prefix.", index,
               (unsigned)CONFIG_SPI_MAX_TRANSFER_BYTES);
      return ESP_ERR_INVALID_ARG;
    }
  } else {
    snprintf(reason, reasonLen, "mapping[%d].type must be one of: exact, prefix.", index);
    return ESP_ERR_INVALID_ARG;
  }
  if (misoLen > CONFIG_SPI_MAX_TRANSFER_BYTES) {
    snprintf(reason, reasonLen, "mapping[%d].miso must contain 0 to %u bytes.", index,
             (unsigned)CONFIG_SPI_MAX_TRANSFER_BYTES);
    return ESP_ERR_INVALID_ARG;
  }
  if ((mosiLen > 0 && mosi == NULL) || (misoLen > 0 && miso == NULL)) return ESP_ERR_INVALID_ARG;
  size_t need = 3u + mosiLen + misoLen;
  if (*used > cap || cap - *used < need) {
    snprintf(reason, reasonLen, "mapping is too large. The packed map must fit in %u bytes.",
             (unsigned)CONFIG_SPI_SLAVE_MAP_BYTES);
    return ESP_ERR_INVALID_SIZE;
  }
  buf[*used] = (uint8_t)type;
  buf[*used + 1] = (uint8_t)mosiLen;
  buf[*used + 2] = (uint8_t)misoLen;
  if (mosiLen > 0) memcpy(buf + *used + 3, mosi, mosiLen);
  if (misoLen > 0) memcpy(buf + *used + 3 + mosiLen, miso, misoLen);
  *used += need;
  return ESP_OK;
}

esp_err_t SpiCtrl_SetSlaveMap(int id, const uint8_t* packed, size_t packedLen, char* reason, size_t reasonLen)
{
  if (!SpiCtrl_IsValidId(id)) return ESP_ERR_INVALID_ARG;
  if (packed == NULL && packedLen > 0) return ESP_ERR_INVALID_ARG;
  if (packedLen > CONFIG_SPI_SLAVE_MAP_BYTES) {
    snprintf(reason, reasonLen, "mapping is too large. The packed map must fit in %u bytes.",
             (unsigned)CONFIG_SPI_SLAVE_MAP_BYTES);
    return ESP_ERR_INVALID_SIZE;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);
  esp_err_t err = SpiCtrl_RequireSlave(id, reason, reasonLen);
  if (err == ESP_OK && buses[id].cfg.mosiPin < 0) {
    snprintf(reason, reasonLen, "SPI %d has no MOSI pin. POST /spi/%d/config with pins.mosi set before arming a slave response.",
             id, id);
    err = ESP_ERR_INVALID_STATE;
  }
  if (err == ESP_OK) {
    memset(mapBuf, 0, sizeof(mapBuf));
    mapUsed = packedLen;
    if (packedLen > 0) memcpy(mapBuf, packed, packedLen);
  }
  xSemaphoreGive(mutex);
  return err;
}
