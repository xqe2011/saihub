/**
 * @name UART control module
 * @file uart_ctrl.c
 * @author xqe2011
 */
#include "uart_ctrl.h"

#include "cloud.h"
#include "config.h"
#include "gpio_ctrl.h"
#include "lock.h"
#include "tcp.h"
#include "tool.h"
#include "wifi.h"

#include <driver/uart.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdio.h>
#include <string.h>

static const char* tag = "SAIHub-Uart";

static const int uartIdToNum[] = CONFIG_UART_ID_TO_NUM;

typedef struct {
  UartCtrl_Config cfg;
  bool driverInstalled;
  bool rtsActive;
  bool dtrActive;
  char owner[32];
} UartCtrl_Runtime;

static UartCtrl_Runtime uarts[TOOL_GET_ARRAY_LENGTH(uartIdToNum)];
static SemaphoreHandle_t mutex;
static SemaphoreHandle_t configMutex;

bool UartCtrl_IsValidId(int id)
{
  return id >= 0 && id < (int)TOOL_GET_ARRAY_LENGTH(uartIdToNum);
}

int UartCtrl_GetCount(void)
{
  return (int)TOOL_GET_ARRAY_LENGTH(uartIdToNum);
}

const char* UartCtrl_ParityToString(UartCtrl_Parity p)
{
  switch (p) {
    case UART_CTRL_PARITY_EVEN:
      return "even";
    case UART_CTRL_PARITY_ODD:
      return "odd";
    case UART_CTRL_PARITY_NONE:
    default:
      return "none";
  }
}

bool UartCtrl_ParityFromString(const char* s, UartCtrl_Parity* out)
{
  if (s == NULL || out == NULL) return false;
  if (strcmp(s, "none") == 0) {
    *out = UART_CTRL_PARITY_NONE;
    return true;
  }
  if (strcmp(s, "even") == 0) {
    *out = UART_CTRL_PARITY_EVEN;
    return true;
  }
  if (strcmp(s, "odd") == 0) {
    *out = UART_CTRL_PARITY_ODD;
    return true;
  }
  return false;
}

const char* UartCtrl_EncodingToString(UartCtrl_Encoding e)
{
  return e == UART_CTRL_ENCODING_BYTE ? "byte" : "utf8";
}

bool UartCtrl_EncodingFromString(const char* s, UartCtrl_Encoding* out)
{
  if (s == NULL || out == NULL) return false;
  if (strcmp(s, "utf8") == 0) {
    *out = UART_CTRL_ENCODING_UTF8;
    return true;
  }
  if (strcmp(s, "byte") == 0) {
    *out = UART_CTRL_ENCODING_BYTE;
    return true;
  }
  return false;
}

const char* UartCtrl_ModeToString(UartCtrl_Mode mode)
{
  return mode == UART_CTRL_MODE_RFC2217 ? "rfc2217" : "native";
}

bool UartCtrl_ModeFromString(const char* s, UartCtrl_Mode* out)
{
  if (s == NULL || out == NULL) return false;
  if (strcmp(s, "native") == 0) {
    *out = UART_CTRL_MODE_NATIVE;
    return true;
  }
  if (strcmp(s, "rfc2217") == 0) {
    *out = UART_CTRL_MODE_RFC2217;
    return true;
  }
  return false;
}

static uart_port_t UartCtrl_HwPort(int id)
{
  return (uart_port_t)uartIdToNum[id];
}

static uart_word_length_t UartCtrl_ToWordLength(int dataBits)
{
  switch (dataBits) {
    case 5:
      return UART_DATA_5_BITS;
    case 6:
      return UART_DATA_6_BITS;
    case 7:
      return UART_DATA_7_BITS;
    case 8:
    default:
      return UART_DATA_8_BITS;
  }
}

static uart_parity_t UartCtrl_ToParity(UartCtrl_Parity p)
{
  switch (p) {
    case UART_CTRL_PARITY_EVEN:
      return UART_PARITY_EVEN;
    case UART_CTRL_PARITY_ODD:
      return UART_PARITY_ODD;
    case UART_CTRL_PARITY_NONE:
    default:
      return UART_PARITY_DISABLE;
  }
}

static uart_stop_bits_t UartCtrl_ToStopBits(int stopBits)
{
  return stopBits == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
}

static void UartCtrl_MakeOwner(int id, char* out, size_t outLen)
{
  snprintf(out, outLen, "uart:%d", id);
}

static bool UartCtrl_PinsConflict(const UartCtrl_Config* cfg, char* reason, size_t reasonLen)
{
  const struct {
    const char* name;
    int pin;
  } assigned[] = {
      {"rx", cfg->rxPin},
      {"tx", cfg->txPin},
      {"rts", cfg->rtsPin},
      {"dtr", cfg->dtrPin},
  };
  for (size_t i = 0; i < TOOL_GET_ARRAY_LENGTH(assigned); i++) {
    if (assigned[i].pin < 0) continue;
    for (size_t j = i + 1; j < TOOL_GET_ARRAY_LENGTH(assigned); j++) {
      if (assigned[j].pin < 0) continue;
      if (assigned[i].pin == assigned[j].pin) {
        snprintf(reason, reasonLen, "pins.%s and pins.%s must be different when both are set.", assigned[i].name,
                 assigned[j].name);
        return true;
      }
    }
  }
  return false;
}

static esp_err_t UartCtrl_ValidateOptionalPin(int pin, const char* key, char* reason, size_t reasonLen)
{
  if (pin < 0) return ESP_OK;
  if (!GpioCtrl_IsValidLogicalPin(pin)) {
    snprintf(reason, reasonLen, "pins.%s is invalid. Use a pin from 0 to %d or omit it.", key,
             GpioCtrl_GetLogicalCount() - 1);
    return ESP_ERR_INVALID_ARG;
  }
  return ESP_OK;
}

static esp_err_t UartCtrl_ValidateConfig(const UartCtrl_Config* cfg, char* reason, size_t reasonLen)
{
  if (cfg == NULL) return ESP_ERR_INVALID_ARG;
  if (cfg->baudRate < 1) {
    snprintf(reason, reasonLen, "baudRate must be a positive integer.");
    return ESP_ERR_INVALID_ARG;
  }
  if (cfg->dataBits < 5 || cfg->dataBits > 8) {
    snprintf(reason, reasonLen, "dataBits must be 5, 6, 7, or 8.");
    return ESP_ERR_INVALID_ARG;
  }
  if (cfg->stopBits != 1 && cfg->stopBits != 2) {
    snprintf(reason, reasonLen, "stopBits must be 1 or 2.");
    return ESP_ERR_INVALID_ARG;
  }
  if (UartCtrl_ValidateOptionalPin(cfg->rxPin, "rx", reason, reasonLen) != ESP_OK) return ESP_ERR_INVALID_ARG;
  if (UartCtrl_ValidateOptionalPin(cfg->txPin, "tx", reason, reasonLen) != ESP_OK) return ESP_ERR_INVALID_ARG;
  if (UartCtrl_ValidateOptionalPin(cfg->rtsPin, "rts", reason, reasonLen) != ESP_OK) return ESP_ERR_INVALID_ARG;
  if (UartCtrl_ValidateOptionalPin(cfg->dtrPin, "dtr", reason, reasonLen) != ESP_OK) return ESP_ERR_INVALID_ARG;
  if (UartCtrl_PinsConflict(cfg, reason, reasonLen)) return ESP_ERR_INVALID_ARG;
  if (cfg->mode != UART_CTRL_MODE_NATIVE && cfg->mode != UART_CTRL_MODE_RFC2217) {
    snprintf(reason, reasonLen, "mode must be one of: native, rfc2217.");
    return ESP_ERR_INVALID_ARG;
  }
  if (cfg->enable && cfg->rxPin < 0 && cfg->txPin < 0) {
    snprintf(reason, reasonLen, "enable true requires at least one of pins.rx or pins.tx.");
    return ESP_ERR_INVALID_ARG;
  }
  return ESP_OK;
}

static void UartCtrl_BuildPinResources(const UartCtrl_Config* cfg, Lock_Resource* out, size_t* countOut)
{
  size_t n = 0;
  const int pins[] = {cfg->rxPin, cfg->txPin, cfg->rtsPin, cfg->dtrPin};
  for (size_t i = 0; i < TOOL_GET_ARRAY_LENGTH(pins); i++) {
    if (pins[i] < 0) continue;
    out[n].kind = LOCK_KIND_GPIO;
    out[n].pin = pins[i];
    out[n].methods = LOCK_METHOD_READ | LOCK_METHOD_WRITE;
    n++;
  }
  *countOut = n;
}

static void UartCtrl_DisablePinModes(const UartCtrl_Config* cfg)
{
  const int pins[] = {cfg->rxPin, cfg->txPin, cfg->rtsPin, cfg->dtrPin};
  for (size_t i = 0; i < TOOL_GET_ARRAY_LENGTH(pins); i++) {
    if (pins[i] >= 0) GpioCtrl_SetConfig(pins[i], GPIO_CTRL_MODE_DISABLE, false, false, false);
  }
}

static esp_err_t UartCtrl_ApplyControlPin(int logicalPin, bool active, char* reason, size_t reasonLen)
{
  if (GpioCtrl_SetConfig(logicalPin, GPIO_CTRL_MODE_DIGITAL_OUTPUT, false, false, false) != ESP_OK) {
    snprintf(reason, reasonLen, "Could not configure UART control pin %d as digital output.", logicalPin);
    return ESP_FAIL;
  }
  /* UART control outputs are active low. */
  if (GpioCtrl_SetLevel(logicalPin, active ? 0 : 1) != ESP_OK) {
    snprintf(reason, reasonLen, "Could not drive UART control pin %d.", logicalPin);
    return ESP_FAIL;
  }
  return ESP_OK;
}

static esp_err_t UartCtrl_ApplyControlPins(UartCtrl_Runtime* u, char* reason, size_t reasonLen)
{
  if (u->cfg.rtsPin >= 0) {
    esp_err_t err = UartCtrl_ApplyControlPin(u->cfg.rtsPin, u->rtsActive, reason, reasonLen);
    if (err != ESP_OK) return err;
  }
  if (u->cfg.dtrPin >= 0) {
    esp_err_t err = UartCtrl_ApplyControlPin(u->cfg.dtrPin, u->dtrActive, reason, reasonLen);
    if (err != ESP_OK) return err;
  }
  return ESP_OK;
}

static esp_err_t UartCtrl_StartDriver(int id, const UartCtrl_Config* cfg, bool rtsActive, bool dtrActive, char* reason,
                                      size_t reasonLen)
{
  Lock_Resource resources[4];
  size_t resourceCount = 0;
  UartCtrl_BuildPinResources(cfg, resources, &resourceCount);

  char owner[32];
  UartCtrl_MakeOwner(id, owner, sizeof(owner));
  Lock_Conflict conflict;
  memset(&conflict, 0, sizeof(conflict));
  esp_err_t aret = Lock_AcquirePeripheral(owner, resources, resourceCount, &conflict);
  if (aret == ESP_ERR_INVALID_STATE) {
    if (conflict.isPeripheral) {
      const char* label = conflict.peripheralLabel[0] ? conflict.peripheralLabel : "a peripheral";
      snprintf(reason, reasonLen, "Pin %d is reserved by %s. Disable %s or use other pins.", conflict.pin, label,
               label);
    } else {
      snprintf(reason, reasonLen,
               "Pin %d is already held by a lock. DELETE that lock or wait until it expires before enabling UART.",
               conflict.pin);
    }
    return ESP_ERR_INVALID_STATE;
  }
  if (aret != ESP_OK) {
    snprintf(reason, reasonLen, "internal");
    return aret;
  }

  UartCtrl_DisablePinModes(cfg);

  uart_config_t ucfg = {
      .baud_rate = cfg->baudRate,
      .data_bits = UartCtrl_ToWordLength(cfg->dataBits),
      .parity = UartCtrl_ToParity(cfg->parity),
      .stop_bits = UartCtrl_ToStopBits(cfg->stopBits),
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };

  uart_port_t port = UartCtrl_HwPort(id);
  esp_err_t err = uart_driver_install(port, CONFIG_UART_RX_BUF_BYTES, CONFIG_UART_RX_BUF_BYTES, 0, NULL, 0);
  if (err != ESP_OK) {
    Lock_ReleasePeripheral(owner);
    snprintf(reason, reasonLen, "UART resources are unavailable. Disable another UART or try again.");
    return err;
  }
  err = uart_param_config(port, &ucfg);
  if (err != ESP_OK) {
    uart_driver_delete(port);
    Lock_ReleasePeripheral(owner);
    snprintf(reason, reasonLen, "UART configuration was rejected. Check baudRate, dataBits, parity, and stopBits.");
    return err;
  }

  int txHw = cfg->txPin >= 0 ? GpioCtrl_GetHwPin(cfg->txPin) : UART_PIN_NO_CHANGE;
  int rxHw = cfg->rxPin >= 0 ? GpioCtrl_GetHwPin(cfg->rxPin) : UART_PIN_NO_CHANGE;
  /* uart_set_pin(tx, rx, rts, cts) — control lines are GPIO-owned; leave RTS/CTS unbound. */
  err = uart_set_pin(port, txHw, rxHw, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  if (err != ESP_OK) {
    uart_driver_delete(port);
    Lock_ReleasePeripheral(owner);
    snprintf(reason, reasonLen, "Could not bind UART pins. Use other pins or disable UART.");
    return err;
  }

  UartCtrl_Runtime* u = &uarts[id];
  u->cfg = *cfg;
  u->cfg.enable = true;
  u->rtsActive = cfg->rtsPin >= 0 && rtsActive;
  u->dtrActive = cfg->dtrPin >= 0 && dtrActive;
  u->driverInstalled = true;
  snprintf(u->owner, sizeof(u->owner), "%s", owner);

  err = UartCtrl_ApplyControlPins(u, reason, reasonLen);
  if (err != ESP_OK) {
    uart_driver_delete(port);
    u->driverInstalled = false;
    Lock_ReleasePeripheral(owner);
    u->owner[0] = '\0';
    UartCtrl_DisablePinModes(cfg);
    return err;
  }
  return ESP_OK;
}

esp_err_t UartCtrl_Init(void)
{
  memset(uarts, 0, sizeof(uarts));
  for (size_t i = 0; i < TOOL_GET_ARRAY_LENGTH(uarts); i++) {
    uarts[i].cfg.enable = false;
    uarts[i].cfg.mode = UART_CTRL_MODE_NATIVE;
    uarts[i].cfg.baudRate = 115200;
    uarts[i].cfg.dataBits = 8;
    uarts[i].cfg.parity = UART_CTRL_PARITY_NONE;
    uarts[i].cfg.stopBits = 1;
    uarts[i].cfg.encoding = UART_CTRL_ENCODING_UTF8;
    uarts[i].cfg.rxPin = -1;
    uarts[i].cfg.txPin = -1;
    uarts[i].cfg.rtsPin = -1;
    uarts[i].cfg.dtrPin = -1;
    uarts[i].rtsActive = false;
    uarts[i].dtrActive = false;
    uarts[i].driverInstalled = false;
    uarts[i].owner[0] = '\0';
  }
  configMutex = xSemaphoreCreateMutex();
  TOOL_CHECK_OR_LOG_RETURN(configMutex == NULL, "config mutex create failed");
  mutex = xSemaphoreCreateMutex();
  if (mutex == NULL) {
    vSemaphoreDelete(configMutex);
    configMutex = NULL;
    ESP_LOGE(tag, "mutex create failed");
    return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t UartCtrl_GetConfig(int id, UartCtrl_Config* out)
{
  if (!UartCtrl_IsValidId(id) || out == NULL) return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(mutex, portMAX_DELAY);
  *out = uarts[id].cfg;
  xSemaphoreGive(mutex);
  return ESP_OK;
}

static esp_err_t UartCtrl_GetControlLine(int id, bool isDtr, bool* activeOut, char* reason, size_t reasonLen)
{
  if (!UartCtrl_IsValidId(id) || activeOut == NULL) return ESP_ERR_INVALID_ARG;

  xSemaphoreTake(mutex, portMAX_DELAY);
  UartCtrl_Runtime* u = &uarts[id];
  if (!u->cfg.enable || !u->driverInstalled) {
    snprintf(reason, reasonLen, "UART %d is not enabled. POST /uart/%d/config with enable true first.", id, id);
    xSemaphoreGive(mutex);
    return ESP_ERR_INVALID_STATE;
  }
  int pin = isDtr ? u->cfg.dtrPin : u->cfg.rtsPin;
  const char* name = isDtr ? "dtr" : "rts";
  if (pin < 0) {
    snprintf(reason, reasonLen, "UART %d has no %s pin. POST /uart/%d/config with pins.%s set.", id, name, id, name);
    xSemaphoreGive(mutex);
    return ESP_ERR_INVALID_STATE;
  }
  *activeOut = isDtr ? u->dtrActive : u->rtsActive;
  xSemaphoreGive(mutex);
  return ESP_OK;
}

esp_err_t UartCtrl_GetDtrActive(int id, bool* activeOut, char* reason, size_t reasonLen)
{
  return UartCtrl_GetControlLine(id, true, activeOut, reason, reasonLen);
}

esp_err_t UartCtrl_GetRtsActive(int id, bool* activeOut, char* reason, size_t reasonLen)
{
  return UartCtrl_GetControlLine(id, false, activeOut, reason, reasonLen);
}

static bool UartCtrl_SamePins(const UartCtrl_Config* a, const UartCtrl_Config* b)
{
  return a->rxPin == b->rxPin && a->txPin == b->txPin && a->rtsPin == b->rtsPin && a->dtrPin == b->dtrPin;
}

static bool UartCtrl_SameFraming(const UartCtrl_Config* a, const UartCtrl_Config* b)
{
  return a->baudRate == b->baudRate && a->dataBits == b->dataBits && a->parity == b->parity && a->stopBits == b->stopBits;
}

static bool UartCtrl_SameConfig(const UartCtrl_Config* a, const UartCtrl_Config* b)
{
  return a->enable == b->enable && a->mode == b->mode && a->encoding == b->encoding && UartCtrl_SamePins(a, b) &&
         UartCtrl_SameFraming(a, b);
}

static void UartCtrl_StopDriverLocked(int id)
{
  UartCtrl_Runtime* u = &uarts[id];
  if (u->driverInstalled) {
    uart_driver_delete(UartCtrl_HwPort(id));
    u->driverInstalled = false;
  }
  if (u->owner[0] != '\0') {
    Lock_ReleasePeripheral(u->owner);
    u->owner[0] = '\0';
  }
  UartCtrl_DisablePinModes(&u->cfg);
}

static esp_err_t UartCtrl_ApplyFraming(int id, const UartCtrl_Config* cfg, char* reason, size_t reasonLen)
{
  uart_config_t ucfg = {
      .baud_rate = cfg->baudRate,
      .data_bits = UartCtrl_ToWordLength(cfg->dataBits),
      .parity = UartCtrl_ToParity(cfg->parity),
      .stop_bits = UartCtrl_ToStopBits(cfg->stopBits),
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };
  if (uart_param_config(UartCtrl_HwPort(id), &ucfg) != ESP_OK) {
    snprintf(reason, reasonLen, "UART configuration was rejected. Check baudRate, dataBits, parity, and stopBits.");
    return ESP_FAIL;
  }
  return ESP_OK;
}

/* Caller holds mutex. Releases it around the stop so the bridge task can finish an in-flight UartCtrl call. */
static void UartCtrl_DropBridge(void)
{
  if (!Tcp_Rfc2217IsRunning()) return;
  xSemaphoreGive(mutex);
  Tcp_Rfc2217Stop();
  xSemaphoreTake(mutex, portMAX_DELAY);
}

/* Caller holds mutex. Releases it around start so the bridge task can call SetConfig. */
static esp_err_t UartCtrl_StartBridge(int id, char* reason, size_t reasonLen)
{
  xSemaphoreGive(mutex);
  esp_err_t err = Tcp_Rfc2217Start(id);
  xSemaphoreTake(mutex, portMAX_DELAY);
  if (err != ESP_OK) snprintf(reason, reasonLen, "RFC2217 service failed to start.");
  return err;
}

static void UartCtrl_StoreDisabled(UartCtrl_Runtime* u, const UartCtrl_Config* cfg)
{
  u->cfg = *cfg;
  u->cfg.enable = false;
  u->rtsActive = false;
  u->dtrActive = false;
}

static esp_err_t UartCtrl_RestoreLocked(int id, const UartCtrl_Config* previous, bool rtsActive, bool dtrActive)
{
  UartCtrl_Runtime* u = &uarts[id];
  if (!previous->enable) {
    UartCtrl_StoreDisabled(u, previous);
    return ESP_OK;
  }
  char restoreReason[160];
  esp_err_t err = UartCtrl_StartDriver(id, previous, rtsActive, dtrActive, restoreReason, sizeof(restoreReason));
  if (err != ESP_OK) {
    ESP_LOGW(tag, "Failed to restore UART %d after config error: %s", id, restoreReason);
    UartCtrl_StoreDisabled(u, previous);
    return err;
  }
  if (previous->mode == UART_CTRL_MODE_RFC2217) {
    err = UartCtrl_StartBridge(id, restoreReason, sizeof(restoreReason));
    if (err != ESP_OK) {
      ESP_LOGW(tag, "Failed to restore UART %d RFC2217 after config error: %s", id, restoreReason);
      UartCtrl_StopDriverLocked(id);
      UartCtrl_StoreDisabled(u, previous);
    }
  }
  return err;
}

esp_err_t UartCtrl_SetConfig(int id, const UartCtrl_Config* cfg, char* reason, size_t reasonLen)
{
  if (!UartCtrl_IsValidId(id) || cfg == NULL) return ESP_ERR_INVALID_ARG;
  if (UartCtrl_ValidateConfig(cfg, reason, reasonLen) != ESP_OK) return ESP_ERR_INVALID_ARG;

  /* The bridge task calls SetConfig for line changes. It must not take configMutex: HTTP holds that
   * mutex across Tcp_Rfc2217Stop, and Stop waits for this task. */
  bool fromBridge = Tcp_Rfc2217InTask();
  if (!fromBridge) xSemaphoreTake(configMutex, portMAX_DELAY);
  xSemaphoreTake(mutex, portMAX_DELAY);

  UartCtrl_Runtime* u = &uarts[id];
  UartCtrl_Config previous = u->cfg;
  bool previousRts = u->rtsActive;
  bool previousDtr = u->dtrActive;
  bool wasInstalled = u->driverInstalled;
  bool stayUp = false;
  bool keepBridge = false;
  bool nextRts = false;
  bool nextDtr = false;
  esp_err_t err = ESP_OK;

  if (fromBridge) {
    bool live = previous.enable && wasInstalled && cfg->enable && previous.mode == UART_CTRL_MODE_RFC2217 &&
                cfg->mode == UART_CTRL_MODE_RFC2217 && UartCtrl_SamePins(&previous, cfg);
    if (!live) {
      snprintf(reason, reasonLen, "RFC2217 client can only change baudRate, dataBits, parity, and stopBits.");
      err = ESP_ERR_INVALID_ARG;
      goto done;
    }
    if (!UartCtrl_SameFraming(&previous, cfg)) {
      err = UartCtrl_ApplyFraming(id, cfg, reason, reasonLen);
      if (err != ESP_OK) goto done;
    }
    u->cfg = *cfg;
    u->cfg.enable = true;
    goto done;
  }

  if (UartCtrl_SameConfig(&previous, cfg) && (!cfg->enable || wasInstalled)) {
    if (cfg->enable && cfg->mode == UART_CTRL_MODE_RFC2217 && wasInstalled && !Tcp_Rfc2217IsRunning()) {
      err = UartCtrl_StartBridge(id, reason, reasonLen);
    }
    goto done;
  }

  stayUp = previous.enable && wasInstalled && cfg->enable && UartCtrl_SamePins(&previous, cfg);
  keepBridge = stayUp && previous.mode == UART_CTRL_MODE_RFC2217 && cfg->mode == UART_CTRL_MODE_RFC2217;
  if (Tcp_Rfc2217IsRunning() && !keepBridge) UartCtrl_DropBridge();

  if (!cfg->enable) {
    UartCtrl_StopDriverLocked(id);
    UartCtrl_StoreDisabled(u, cfg);
    goto done;
  }

  if (stayUp) {
    if (!UartCtrl_SameFraming(&previous, cfg)) {
      err = UartCtrl_ApplyFraming(id, cfg, reason, reasonLen);
      if (err != ESP_OK) goto done;
    }
    u->cfg = *cfg;
    u->cfg.enable = true;
    u->rtsActive = cfg->rtsPin >= 0 && previousRts;
    u->dtrActive = cfg->dtrPin >= 0 && previousDtr;
    if (cfg->mode == UART_CTRL_MODE_RFC2217 && !Tcp_Rfc2217IsRunning()) {
      err = UartCtrl_StartBridge(id, reason, reasonLen);
      if (err != ESP_OK) {
        char ignored[160];
        if (!UartCtrl_SameFraming(&previous, cfg)) (void)UartCtrl_ApplyFraming(id, &previous, ignored, sizeof(ignored));
        u->cfg = previous;
        u->rtsActive = previousRts;
        u->dtrActive = previousDtr;
      }
    }
    goto done;
  }

  UartCtrl_StopDriverLocked(id);
  if (cfg->rtsPin >= 0 && wasInstalled && previous.enable && previous.rtsPin == cfg->rtsPin) nextRts = previousRts;
  if (cfg->dtrPin >= 0 && wasInstalled && previous.enable && previous.dtrPin == cfg->dtrPin) nextDtr = previousDtr;

  err = UartCtrl_StartDriver(id, cfg, nextRts, nextDtr, reason, reasonLen);
  if (err != ESP_OK) {
    if (wasInstalled && previous.enable) (void)UartCtrl_RestoreLocked(id, &previous, previousRts, previousDtr);
    else UartCtrl_StoreDisabled(u, cfg);
    goto done;
  }
  if (cfg->mode == UART_CTRL_MODE_RFC2217) {
    err = UartCtrl_StartBridge(id, reason, reasonLen);
    if (err != ESP_OK) {
      UartCtrl_StopDriverLocked(id);
      if (wasInstalled && previous.enable) (void)UartCtrl_RestoreLocked(id, &previous, previousRts, previousDtr);
      else UartCtrl_StoreDisabled(u, cfg);
    }
  }

done:
  xSemaphoreGive(mutex);
  if (!fromBridge) xSemaphoreGive(configMutex);
  return err;
}

esp_err_t UartCtrl_GetRfc2217Status(int id, bool cloud, UartCtrl_Rfc2217Status* out)
{
  if (!UartCtrl_IsValidId(id) || out == NULL) return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(mutex, portMAX_DELAY);
  UartCtrl_Runtime* u = &uarts[id];
  bool running = u->cfg.enable && u->driverInstalled && u->cfg.mode == UART_CTRL_MODE_RFC2217 && Tcp_Rfc2217IsRunning() &&
                 Tcp_Rfc2217UartId() == id;
  xSemaphoreGive(mutex);
  if (!running) return ESP_ERR_INVALID_STATE;

  memset(out, 0, sizeof(*out));
  out->cloud = cloud;
  if (cloud) {
    out->prompt = CONFIG_RFC2217_CLOUD_STATUS_PROMPT;
    if (!Cloud_FormatTcpStreamUrl("rfc2217", out->url, sizeof out->url)) return ESP_FAIL;
    return ESP_OK;
  }
  out->prompt = CONFIG_RFC2217_LOCAL_STATUS_PROMPT;
  out->port = CONFIG_RFC2217_TCP_PORT;
  char ip[16];
  ip[0] = '\0';
  if (Wifi_GetPairStatus(ip, sizeof(ip), NULL, 0) != WIFI_PAIR_STATE_CONNECTED || ip[0] == '\0' ||
      strcmp(ip, "0.0.0.0") == 0) {
    out->ip[0] = '\0';
  } else {
    snprintf(out->ip, sizeof(out->ip), "%s", ip);
  }
  return ESP_OK;
}

static esp_err_t UartCtrl_SetControlLine(int id, bool isDtr, bool active, char* reason, size_t reasonLen)
{
  if (!UartCtrl_IsValidId(id)) return ESP_ERR_INVALID_ARG;

  xSemaphoreTake(mutex, portMAX_DELAY);
  UartCtrl_Runtime* u = &uarts[id];
  if (!u->cfg.enable || !u->driverInstalled) {
    snprintf(reason, reasonLen, "UART %d is not enabled. POST /uart/%d/config with enable true first.", id, id);
    xSemaphoreGive(mutex);
    return ESP_ERR_INVALID_STATE;
  }
  int pin = isDtr ? u->cfg.dtrPin : u->cfg.rtsPin;
  const char* name = isDtr ? "dtr" : "rts";
  if (pin < 0) {
    snprintf(reason, reasonLen, "UART %d has no %s pin. POST /uart/%d/config with pins.%s set.", id, name, id, name);
    xSemaphoreGive(mutex);
    return ESP_ERR_INVALID_STATE;
  }
  esp_err_t err = UartCtrl_ApplyControlPin(pin, active, reason, reasonLen);
  if (err != ESP_OK) {
    xSemaphoreGive(mutex);
    return err;
  }
  if (isDtr) {
    u->dtrActive = active;
  } else {
    u->rtsActive = active;
  }
  xSemaphoreGive(mutex);
  return ESP_OK;
}

esp_err_t UartCtrl_SetDtr(int id, bool active, char* reason, size_t reasonLen)
{
  return UartCtrl_SetControlLine(id, true, active, reason, reasonLen);
}

esp_err_t UartCtrl_SetRts(int id, bool active, char* reason, size_t reasonLen)
{
  return UartCtrl_SetControlLine(id, false, active, reason, reasonLen);
}

esp_err_t UartCtrl_Transmit(int id, const uint8_t* data, size_t len, char* reason, size_t reasonLen)
{
  if (!UartCtrl_IsValidId(id)) return ESP_ERR_INVALID_ARG;
  if (data == NULL && len > 0) return ESP_ERR_INVALID_ARG;
  if (len > CONFIG_UART_MAX_PAYLOAD_BYTES) {
    snprintf(reason, reasonLen, "data is too large. Send at most %u bytes per request.",
             (unsigned)CONFIG_UART_MAX_PAYLOAD_BYTES);
    return ESP_ERR_INVALID_SIZE;
  }

  xSemaphoreTake(mutex, portMAX_DELAY);
  UartCtrl_Runtime* u = &uarts[id];
  if (!u->cfg.enable || !u->driverInstalled) {
    snprintf(reason, reasonLen, "UART %d is not enabled. POST /uart/%d/config with enable true first.", id, id);
    xSemaphoreGive(mutex);
    return ESP_ERR_INVALID_STATE;
  }
  if (u->cfg.txPin < 0) {
    snprintf(reason, reasonLen, "UART %d has no tx pin. POST /uart/%d/config with pins.tx set.", id, id);
    xSemaphoreGive(mutex);
    return ESP_ERR_INVALID_STATE;
  }
  if (len == 0) {
    xSemaphoreGive(mutex);
    return ESP_OK;
  }
  int written = uart_write_bytes(UartCtrl_HwPort(id), data, len);
  xSemaphoreGive(mutex);
  if (written < 0 || (size_t)written != len) {
    snprintf(reason, reasonLen, "internal");
    return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t UartCtrl_Receive(int id, uint8_t* out, size_t maxLen, size_t* outLen, char* reason, size_t reasonLen)
{
  if (!UartCtrl_IsValidId(id) || out == NULL || outLen == NULL) return ESP_ERR_INVALID_ARG;
  *outLen = 0;

  xSemaphoreTake(mutex, portMAX_DELAY);
  UartCtrl_Runtime* u = &uarts[id];
  if (!u->cfg.enable || !u->driverInstalled) {
    snprintf(reason, reasonLen, "UART %d is not enabled. POST /uart/%d/config with enable true first.", id, id);
    xSemaphoreGive(mutex);
    return ESP_ERR_INVALID_STATE;
  }
  if (u->cfg.rxPin < 0) {
    snprintf(reason, reasonLen, "UART %d has no rx pin. POST /uart/%d/config with pins.rx set.", id, id);
    xSemaphoreGive(mutex);
    return ESP_ERR_INVALID_STATE;
  }
  size_t want = maxLen;
  if (want > CONFIG_UART_MAX_PAYLOAD_BYTES) want = CONFIG_UART_MAX_PAYLOAD_BYTES;
  int n = uart_read_bytes(UartCtrl_HwPort(id), out, want, 0);
  xSemaphoreGive(mutex);
  if (n < 0) {
    snprintf(reason, reasonLen, "internal");
    return ESP_FAIL;
  }
  *outLen = (size_t)n;
  return ESP_OK;
}

esp_err_t UartCtrl_Flush(int id, char* reason, size_t reasonLen)
{
  if (!UartCtrl_IsValidId(id)) return ESP_ERR_INVALID_ARG;

  xSemaphoreTake(mutex, portMAX_DELAY);
  UartCtrl_Runtime* u = &uarts[id];
  if (!u->cfg.enable || !u->driverInstalled) {
    snprintf(reason, reasonLen, "UART %d is not enabled. POST /uart/%d/config with enable true first.", id, id);
    xSemaphoreGive(mutex);
    return ESP_ERR_INVALID_STATE;
  }
  esp_err_t err = uart_flush_input(UartCtrl_HwPort(id));
  xSemaphoreGive(mutex);
  if (err != ESP_OK) {
    snprintf(reason, reasonLen, "internal");
    return err;
  }
  return ESP_OK;
}
