/**
 * @name CMSIS-DAP control module
 * @file dap_ctrl.c
 * @author xqe2011
 *
 * One debugger session: CMSIS-DAP commands and SWCLK/SWDIO bit timing.
 * Transports pass connect, disconnect, and one complete request at a time.
 */
#include "dap_ctrl.h"

#include "config.h"
#include "gpio_ctrl.h"
#include "lock.h"
#include "tcp.h"
#include "wifi.h"

#include <driver/dedic_gpio.h>
#include <driver/gpio.h>
#include <esp_app_desc.h>
#include <esp_clk_tree.h>
#include <esp_cpu.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <hal/dedic_gpio_cpu_ll.h>
#include <hal/gpio_ll.h>
#include <soc/clk_tree_defs.h>
#include <soc/gpio_struct.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/semphr.h>
#include <stdio.h>
#include <string.h>

static const char* tag = "SAIHub-Dap";

enum {
  CMD_INFO = 0x00,
  CMD_HOST_STATUS = 0x01,
  CMD_CONNECT = 0x02,
  CMD_DISCONNECT = 0x03,
  CMD_TRANSFER_CONFIGURE = 0x04,
  CMD_TRANSFER = 0x05,
  CMD_TRANSFER_BLOCK = 0x06,
  CMD_TRANSFER_ABORT = 0x07,
  CMD_WRITE_ABORT = 0x08,
  CMD_DELAY = 0x09,
  CMD_RESET_TARGET = 0x0A,
  CMD_SWJ_PINS = 0x10,
  CMD_SWJ_CLOCK = 0x11,
  CMD_SWJ_SEQUENCE = 0x12,
  CMD_SWD_CONFIGURE = 0x13,
  CMD_SWD_SEQUENCE = 0x1D,
  CMD_QUEUE = 0x7E,
  CMD_EXECUTE = 0x7F,
  DAP_OK = 0x00,
  DAP_ERROR = 0xFF,
  ACK_OK = 0x01,
  ACK_WAIT = 0x02,
  ACK_FAULT = 0x04,
  ACK_ERROR = 0x08,
  ACK_MISMATCH = 0x10,
  PIN_SWCLK = 0x01,
  PIN_SWDIO = 0x02,
  PIN_NRESET = 0x80,
  INFO_VENDOR = 0x01,
  INFO_PRODUCT = 0x02,
  INFO_SERIAL = 0x03,
  INFO_FW_VER = 0x04,
  INFO_PRODUCT_FW = 0x09,
  INFO_CAPABILITIES = 0xF0,
  INFO_TIMESTAMP = 0xF1,
  INFO_PACKET_COUNT = 0xFE,
  INFO_PACKET_SIZE = 0xFF,
};

static bool session;
static DapCtrl_SendFn sendFn;
static void* sendUser;
static DapCtrl_Config config;
static SemaphoreHandle_t mutex;

static dedic_gpio_bundle_handle_t bundle;
static bool swdPort;
static int swclkGpio;
static int swdioGpio;
static int nresetGpio = -1;
static uint32_t swclkMask;
static uint32_t swdioMask;
static uint32_t swclkInMask;
static uint32_t swdioInMask;
static uint32_t halfCycles;
static uint32_t clockHz = CONFIG_DAP_DEFAULT_CLOCK_HZ;
static uint32_t swdTimestamp;
static uint8_t turnaround = 1;
static uint8_t idleCycles;
static uint16_t waitRetry = 100;
static uint16_t matchRetry;
static bool dataPhase;
static volatile bool transferAbort;
static bool debuggerConnected;
static bool targetRunning;

static uint8_t queue[CONFIG_DAP_PACKET_SIZE];
static uint8_t queueCopy[CONFIG_DAP_PACKET_SIZE];
static size_t queueLen;
static uint8_t queueCount;
static uint8_t response[CONFIG_DAP_PACKET_SIZE];

static portMUX_TYPE dapMux = portMUX_INITIALIZER_UNLOCKED;

static uint16_t DapCtrl_U16(const uint8_t* p)
{
  uint16_t v;
  memcpy(&v, p, sizeof v);
  return v;
}

static uint32_t DapCtrl_U32(const uint8_t* p)
{
  uint32_t v;
  memcpy(&v, p, sizeof v);
  return v;
}

static void DapCtrl_PutU16(uint8_t* p, uint16_t v)
{
  memcpy(p, &v, sizeof v);
}

static void DapCtrl_PutU32(uint8_t* p, uint32_t v)
{
  memcpy(p, &v, sizeof v);
}

static uint32_t DapCtrl_CpuHz(void)
{
  uint32_t hz = 0;
  if (esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &hz) != ESP_OK || hz == 0) {
    hz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000U;
  }
  return hz;
}

static void DapCtrl_ApplyClock(uint32_t hz)
{
  uint32_t half = (uint32_t)(((uint64_t)DapCtrl_CpuHz() + 2ULL * hz - 1U) / (2ULL * hz));
  /* Faster pin operations must not turn a small requested delay into an uncapped clock. */
  halfCycles = half <= 1U ? 0U : half;
}

static inline __attribute__((always_inline)) void DapCtrl_Wait(uint32_t start)
{
  while ((esp_cpu_get_cycle_count() - start) < halfCycles) {
  }
}

static inline __attribute__((always_inline)) void DapCtrl_Drive(bool swdioOut)
{
  dedic_gpio_cpu_ll_enable_output(swdioOut ? (swclkMask | swdioMask) : swclkMask);
}

static inline __attribute__((always_inline)) void DapCtrl_Clock(void)
{
  if (halfCycles == 0) {
    RV_CLEAR_CSR(CSR_GPIO_OUT_USER, swclkMask);
    RV_SET_CSR(CSR_GPIO_OUT_USER, swclkMask);
    return;
  }
  /* Count the pin write toward the half-period instead of adding its cost to the delay. */
  uint32_t start = esp_cpu_get_cycle_count();
  RV_CLEAR_CSR(CSR_GPIO_OUT_USER, swclkMask);
  DapCtrl_Wait(start);
  start = esp_cpu_get_cycle_count();
  RV_SET_CSR(CSR_GPIO_OUT_USER, swclkMask);
  DapCtrl_Wait(start);
}

static inline __attribute__((always_inline)) void DapCtrl_WriteBit(uint32_t bit)
{
  if (bit) RV_SET_CSR(CSR_GPIO_OUT_USER, swdioMask);
  else RV_CLEAR_CSR(CSR_GPIO_OUT_USER, swdioMask);
  DapCtrl_Clock();
}

static inline __attribute__((always_inline)) uint32_t DapCtrl_ReadBit(void)
{
  if (halfCycles == 0) {
    RV_CLEAR_CSR(CSR_GPIO_OUT_USER, swclkMask);
    uint32_t bit = (dedic_gpio_cpu_ll_read_in() & swdioInMask) ? 1U : 0U;
    RV_SET_CSR(CSR_GPIO_OUT_USER, swclkMask);
    return bit;
  }
  uint32_t start = esp_cpu_get_cycle_count();
  RV_CLEAR_CSR(CSR_GPIO_OUT_USER, swclkMask);
  DapCtrl_Wait(start);
  uint32_t bit = (dedic_gpio_cpu_ll_read_in() & swdioInMask) ? 1U : 0U;
  start = esp_cpu_get_cycle_count();
  RV_SET_CSR(CSR_GPIO_OUT_USER, swclkMask);
  DapCtrl_Wait(start);
  return bit;
}

static void DapCtrl_ReleasePins(void)
{
  if (bundle != NULL) {
    dedic_gpio_del_bundle(bundle);
    bundle = NULL;
  }
  swdPort = false;
  swclkMask = swdioMask = swclkInMask = swdioInMask = 0;
  GpioCtrl_SetConfig(config.swclkPin, GPIO_CTRL_MODE_DISABLE, false, false, false);
  GpioCtrl_SetConfig(config.swdioPin, GPIO_CTRL_MODE_DISABLE, false, false, false);
  if (config.nresetPin >= 0 && GpioCtrl_IsValidLogicalPin(config.nresetPin)) {
    GpioCtrl_SetConfig(config.nresetPin, GPIO_CTRL_MODE_DISABLE, false, false, false);
  }
  nresetGpio = -1;
}

static bool DapCtrl_PortOn(void)
{
  DapCtrl_ReleasePins();
  if (GpioCtrl_SetConfig(config.swclkPin, GPIO_CTRL_MODE_DIGITAL_INPUT_OUTPUT, false, false, false) != ESP_OK ||
      GpioCtrl_SetConfig(config.swdioPin, GPIO_CTRL_MODE_DIGITAL_INPUT_OUTPUT, false, true, false) != ESP_OK) {
    DapCtrl_ReleasePins();
    return false;
  }
  swclkGpio = GpioCtrl_GetHwPin(config.swclkPin);
  swdioGpio = GpioCtrl_GetHwPin(config.swdioPin);
  if (swclkGpio == swdioGpio) {
    DapCtrl_ReleasePins();
    return false;
  }
  int pins[2] = {swclkGpio, swdioGpio};
  dedic_gpio_bundle_config_t cfg = {
      .gpio_array = pins,
      .array_size = 2,
      .flags = {.in_en = 1, .out_en = 1},
  };
  if (dedic_gpio_new_bundle(&cfg, &bundle) != ESP_OK) {
    DapCtrl_ReleasePins();
    return false;
  }

  uint32_t outMask = 0;
  uint32_t inMask = 0;
  uint32_t outOff = 0;
  uint32_t inOff = 0;
  dedic_gpio_get_out_mask(bundle, &outMask);
  dedic_gpio_get_in_mask(bundle, &inMask);
  dedic_gpio_get_out_offset(bundle, &outOff);
  dedic_gpio_get_in_offset(bundle, &inOff);
  swclkMask = 1U << outOff;
  swdioMask = 1U << (outOff + 1U);
  swclkInMask = 1U << inOff;
  swdioInMask = 1U << (inOff + 1U);
  if ((swclkMask | swdioMask) != outMask || (swclkInMask | swdioInMask) != inMask) {
    DapCtrl_ReleasePins();
    return false;
  }

  /* Pad output enable follows the dedicated-GPIO output-enable CSR. */
  gpio_ll_set_output_enable_ctrl(&GPIO, (uint8_t)swclkGpio, true, false);
  gpio_ll_set_output_enable_ctrl(&GPIO, (uint8_t)swdioGpio, true, false);
  dedic_gpio_cpu_ll_write_mask(swclkMask | swdioMask, swclkMask | swdioMask);
  DapCtrl_Drive(true);

  if (config.nresetPin >= 0) {
    if (GpioCtrl_SetConfig(config.nresetPin, GPIO_CTRL_MODE_DIGITAL_INPUT, false, true, false) != ESP_OK) {
      DapCtrl_ReleasePins();
      return false;
    }
    nresetGpio = GpioCtrl_GetHwPin(config.nresetPin);
  }

  swdPort = true;
  DapCtrl_ApplyClock(clockHz);
  return true;
}

static void IRAM_ATTR DapCtrl_Level(uint32_t mask, uint32_t value)
{
  dedic_gpio_cpu_ll_write_mask(mask, value);
}

static uint8_t IRAM_ATTR DapCtrl_Transfer(uint32_t request, uint32_t* data)
{
  bool read = (request & 0x02U) != 0U;
  /* Prepare the request and write data before masking interrupts; keep all wire clocks together. */
  uint32_t requestBits = request & 0x0FU;
  uint32_t requestParity = (uint32_t)__builtin_parity(requestBits);
  uint32_t writeValue = !read && data != NULL ? *data : 0;
  uint32_t writeParity = read ? 0 : (uint32_t)__builtin_parity(writeValue);

  portENTER_CRITICAL(&dapMux);
  DapCtrl_Drive(true);

  DapCtrl_WriteBit(1U);
  for (int i = 0; i < 4; i++) {
    DapCtrl_WriteBit(requestBits & 1U);
    requestBits >>= 1;
  }
  DapCtrl_WriteBit(requestParity);
  DapCtrl_WriteBit(0U);
  DapCtrl_WriteBit(1U);

  DapCtrl_Drive(false);
  for (uint8_t i = 0; i < turnaround; i++) DapCtrl_Clock();

  uint32_t ack = 0;
  for (int i = 0; i < 3; i++) ack |= DapCtrl_ReadBit() << i;

  if (ack == ACK_OK) {
    if (read) {
      uint32_t value = 0;
      uint32_t parity = 0;
      for (int i = 0; i < 32; i++) {
        uint32_t bit = DapCtrl_ReadBit();
        value |= bit << i;
        parity ^= bit;
      }
      uint32_t parityBit = DapCtrl_ReadBit();
      for (uint8_t i = 0; i < turnaround; i++) DapCtrl_Clock();
      DapCtrl_Drive(true);
      if (data != NULL) *data = value;
      if (parity != parityBit) ack = ACK_ERROR;
    } else {
      for (uint8_t i = 0; i < turnaround; i++) DapCtrl_Clock();
      DapCtrl_Drive(true);
      for (int i = 0; i < 32; i++) {
        DapCtrl_WriteBit(writeValue & 1U);
        writeValue >>= 1;
      }
      DapCtrl_WriteBit(writeParity);
    }
    if (ack == ACK_OK) {
      swdTimestamp = esp_cpu_get_cycle_count();
      if (idleCycles != 0) {
        DapCtrl_Level(swdioMask, 0);
        for (uint8_t i = 0; i < idleCycles; i++) DapCtrl_Clock();
      }
      DapCtrl_Level(swdioMask, swdioMask);
    }
    portEXIT_CRITICAL(&dapMux);
    return (uint8_t)ack;
  }

  if (ack == ACK_WAIT || ack == ACK_FAULT) {
    if (dataPhase && read) {
      for (int i = 0; i < 33; i++) DapCtrl_Clock();
    }
    for (uint8_t i = 0; i < turnaround; i++) DapCtrl_Clock();
    DapCtrl_Drive(true);
    if (dataPhase && !read) {
      DapCtrl_Level(swdioMask, 0);
      for (int i = 0; i < 33; i++) DapCtrl_Clock();
    }
    DapCtrl_Level(swdioMask, swdioMask);
    portEXIT_CRITICAL(&dapMux);
    return (uint8_t)ack;
  }

  for (int i = 0; i < (int)turnaround + 33; i++) DapCtrl_Clock();
  DapCtrl_Drive(true);
  DapCtrl_Level(swdioMask, swdioMask);
  portEXIT_CRITICAL(&dapMux);
  return ACK_ERROR;
}

static uint8_t DapCtrl_TransferRetry(uint32_t request, uint32_t* data)
{
  uint16_t left = waitRetry;
  for (;;) {
    if (transferAbort) return ACK_ERROR;
    uint8_t ack = DapCtrl_Transfer(request, data);
    if (ack != ACK_WAIT || left == 0) return ack;
    left--;
  }
}

/* CMSIS-DAP returns completed register accesses, not raw SWD posted reads. */
static uint8_t DapCtrl_TransferRegister(uint32_t request, uint32_t* data)
{
  uint8_t ack = DapCtrl_TransferRetry(request, data);
  if (ack != ACK_OK) return ack;
  if ((request & 0x03U) == 0x03U) {
    /* AP reads post a new access; its result is retrieved through DP RDBUFF. */
    return DapCtrl_TransferRetry(0x0EU, data);
  }
  if ((request & 0x02U) == 0) {
    /* Drain writes so deferred errors are reported before acknowledging them. */
    return DapCtrl_TransferRetry(0x0EU, NULL);
  }
  return ACK_OK;
}

static uint8_t DapCtrl_TransferBlock(uint32_t request, uint16_t count, const uint8_t* words, uint8_t* out, uint16_t* done)
{
  *done = 0;
  if (count == 0) return ACK_OK;
  bool read = (request & 0x02U) != 0;
  bool ap = (request & 0x01U) != 0;
  uint8_t ack;
  if (read && ap) {
    /* Post once, then each AP read returns the previous word while posting the next. */
    ack = DapCtrl_TransferRetry(request, NULL);
    if (ack != ACK_OK) return ack;
  }
  for (uint16_t i = 0; i < count; i++) {
    uint32_t value = read ? 0 : DapCtrl_U32(words + (size_t)i * 4);
    uint32_t wireRequest = read && ap && i == count - 1U ? 0x0EU : request;
    ack = DapCtrl_TransferRetry(wireRequest, &value);
    if (ack != ACK_OK) return ack;
    if (read) DapCtrl_PutU32(out + (size_t)i * 4, value);
    (*done)++;
  }
  /* Complete the last posted write once for the whole block. */
  return read ? ACK_OK : DapCtrl_TransferRetry(0x0EU, NULL);
}

static uint8_t DapCtrl_MatchRead(uint32_t request, uint32_t expect, uint32_t mask, uint32_t* value)
{
  uint16_t left = matchRetry;
  for (;;) {
    uint8_t ack = DapCtrl_TransferRegister(request, value);
    if (ack != ACK_OK) return ack;
    if ((*value & mask) == (expect & mask)) return ACK_OK;
    if (left == 0) return (uint8_t)(ACK_OK | ACK_MISMATCH);
    left--;
  }
}

static void IRAM_ATTR DapCtrl_WriteBits(const uint8_t* bytes, unsigned count)
{
  portENTER_CRITICAL(&dapMux);
  DapCtrl_Drive(true);
  unsigned left = 0;
  uint32_t value = 0;
  for (unsigned i = 0; i < count; i++) {
    if (left == 0) {
      value = *bytes++;
      left = 8;
    }
    DapCtrl_WriteBit(value & 1U);
    value >>= 1;
    left--;
  }
  portEXIT_CRITICAL(&dapMux);
}

static void IRAM_ATTR DapCtrl_ReadBits(uint8_t* bytes, unsigned count)
{
  portENTER_CRITICAL(&dapMux);
  DapCtrl_Drive(false);
  uint8_t value = 0;
  unsigned shift = 0;
  for (unsigned i = 0; i < count; i++) {
    value |= (uint8_t)(DapCtrl_ReadBit() << shift);
    shift++;
    if (shift == 8) {
      *bytes++ = value;
      value = 0;
      shift = 0;
    }
  }
  if (shift != 0) *bytes = value;
  portEXIT_CRITICAL(&dapMux);
}

static void DapCtrl_Nreset(bool released)
{
  if (nresetGpio < 0) return;
  if (released) {
    gpio_set_direction(nresetGpio, GPIO_MODE_INPUT);
    return;
  }
  gpio_set_level(nresetGpio, 0);
  gpio_set_direction(nresetGpio, GPIO_MODE_OUTPUT);
}

static uint8_t DapCtrl_SamplePins(void)
{
  uint8_t pins = 0;
  if (swdPort) {
    uint32_t in = dedic_gpio_cpu_ll_read_in();
    if (in & swclkInMask) pins |= PIN_SWCLK;
    if (in & swdioInMask) pins |= PIN_SWDIO;
  }
  if (nresetGpio < 0 || gpio_get_level(nresetGpio)) pins |= PIN_NRESET;
  return pins;
}

static void DapCtrl_ResetSession(void)
{
  queueLen = 0;
  queueCount = 0;
  transferAbort = false;
  turnaround = 1;
  idleCycles = 0;
  waitRetry = 100;
  matchRetry = 0;
  dataPhase = false;
  clockHz = CONFIG_DAP_DEFAULT_CLOCK_HZ;
  debuggerConnected = false;
  targetRunning = false;
}

static bool DapCtrl_TransferItem(const uint8_t* p, size_t avail, size_t* itemLen, bool* read, bool* match, bool* masked,
                             bool* timestamp)
{
  if (avail < 1) return false;
  uint8_t req = p[0];
  size_t n = 1;
  bool isRead = (req & 0x02U) != 0;
  bool valueMatch = isRead && (req & 0x10U) != 0;
  bool matchMask = valueMatch && (req & 0x20U) != 0;
  if (!isRead) n += 4;
  else if (matchMask) n += 8;
  else if (valueMatch) n += 4;
  if (avail < n) return false;
  if (itemLen != NULL) *itemLen = n;
  if (read != NULL) *read = isRead;
  if (match != NULL) *match = valueMatch;
  if (masked != NULL) *masked = matchMask;
  if (timestamp != NULL) *timestamp = (req & 0x80U) != 0;
  return true;
}

static size_t DapCtrl_Span(const uint8_t* in, size_t len)
{
  if (len < 1) return 0;
  switch (in[0]) {
    case CMD_INFO:
    case CMD_CONNECT:
    case CMD_SWD_CONFIGURE:
      return len >= 2 ? 2 : 0;
    case CMD_HOST_STATUS:
    case CMD_DELAY:
      return len >= 3 ? 3 : 0;
    case CMD_DISCONNECT:
    case CMD_TRANSFER_ABORT:
    case CMD_RESET_TARGET:
      return 1;
    case CMD_TRANSFER_CONFIGURE:
      return len >= 6 ? 6 : 0;
    case CMD_WRITE_ABORT:
      return len >= 6 ? 6 : 0;
    case CMD_SWJ_CLOCK:
      return len >= 5 ? 5 : 0;
    case CMD_SWJ_PINS:
      return len >= 7 ? 7 : 0;
    case CMD_SWJ_SEQUENCE: {
      if (len < 2) return 0;
      unsigned bits = in[1] == 0 ? 256U : in[1];
      size_t bytes = (bits + 7U) / 8U;
      return len < 2 + bytes ? 0 : 2 + bytes;
    }
    case CMD_TRANSFER: {
      if (len < 3) return 0;
      size_t ip = 3;
      for (uint8_t i = 0; i < in[2]; i++) {
        size_t item = 0;
        if (!DapCtrl_TransferItem(in + ip, len - ip, &item, NULL, NULL, NULL, NULL)) return 0;
        ip += item;
      }
      return ip;
    }
    case CMD_TRANSFER_BLOCK: {
      if (len < 5) return 0;
      uint16_t count = DapCtrl_U16(in + 2);
      bool read = (in[4] & 0x02U) != 0;
      if (read) return 5;
      if ((size_t)count > (len - 5) / 4) return 0;
      return 5 + (size_t)count * 4;
    }
    case CMD_SWD_SEQUENCE: {
      if (len < 2) return 0;
      size_t ip = 2;
      for (uint8_t i = 0; i < in[1]; i++) {
        if (ip >= len) return 0;
        uint8_t info = in[ip++];
        unsigned clocks = info & 0x3FU;
        if (clocks == 0) clocks = 64;
        if ((info & 0x80U) == 0) {
          size_t bytes = (clocks + 7U) / 8U;
          if (len - ip < bytes) return 0;
          ip += bytes;
        }
      }
      return ip;
    }
    case CMD_QUEUE:
    case CMD_EXECUTE:
      return 0;
    default:
      return 1;
  }
}

static size_t DapCtrl_Info(uint8_t id, uint8_t* out, size_t outMax)
{
  if (outMax < 2) return 0;
  out[0] = CMD_INFO;
  uint8_t payload[64];
  size_t n = 0;
  switch (id) {
    case INFO_VENDOR:
      n = strlen("SAIHub") + 1;
      memcpy(payload, "SAIHub", n);
      break;
    case INFO_PRODUCT:
      n = strlen("SAIHub CMSIS-DAP") + 1;
      memcpy(payload, "SAIHub CMSIS-DAP", n);
      break;
    case INFO_SERIAL: {
      uint8_t mac[6] = {0};
      esp_read_mac(mac, ESP_MAC_WIFI_STA);
      snprintf((char*)payload, sizeof payload, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
               mac[5]);
      n = strlen((char*)payload) + 1;
      break;
    }
    case INFO_FW_VER:
      n = strlen("2.1.0") + 1;
      memcpy(payload, "2.1.0", n);
      break;
    case INFO_PRODUCT_FW: {
      const esp_app_desc_t* desc = esp_app_get_description();
      snprintf((char*)payload, sizeof payload, "%s", desc->version);
      n = strlen((char*)payload) + 1;
      break;
    }
    case INFO_CAPABILITIES:
      payload[0] = 0x31;
      n = 1;
      break;
    case INFO_TIMESTAMP:
      DapCtrl_PutU32(payload, DapCtrl_CpuHz());
      n = 4;
      break;
    case INFO_PACKET_COUNT:
      payload[0] = CONFIG_DAP_PACKET_COUNT;
      n = 1;
      break;
    case INFO_PACKET_SIZE:
      DapCtrl_PutU16(payload, CONFIG_DAP_PACKET_SIZE);
      n = 2;
      break;
    default:
      n = 0;
      break;
  }
  if (n > outMax - 2) n = outMax - 2;
  out[1] = (uint8_t)n;
  if (n != 0) memcpy(out + 2, payload, n);
  return 2 + n;
}

static size_t DapCtrl_Exec(const uint8_t* in, size_t inLen, uint8_t* out, size_t outMax, size_t* used)
{
  size_t span = DapCtrl_Span(in, inLen);
  if (span == 0 || outMax < 2) {
    *used = 0;
    return 0;
  }
  *used = span;
  uint8_t cmd = in[0];

  switch (cmd) {
    case CMD_INFO:
      return DapCtrl_Info(in[1], out, outMax);
    case CMD_HOST_STATUS:
      if (in[1] == 0) debuggerConnected = in[2] != 0;
      else if (in[1] == 1) targetRunning = in[2] != 0;
      out[0] = cmd;
      out[1] = DAP_OK;
      return 2;
    case CMD_CONNECT: {
      uint8_t port = 0;
      if (in[1] == 0 || in[1] == 1) port = DapCtrl_PortOn() ? 1 : 0;
      else DapCtrl_ReleasePins();
      out[0] = cmd;
      out[1] = port;
      return 2;
    }
    case CMD_DISCONNECT:
      DapCtrl_ReleasePins();
      out[0] = cmd;
      out[1] = DAP_OK;
      return 2;
    case CMD_TRANSFER_CONFIGURE:
      idleCycles = in[1];
      waitRetry = DapCtrl_U16(in + 2);
      matchRetry = DapCtrl_U16(in + 4);
      out[0] = cmd;
      out[1] = DAP_OK;
      return 2;
    case CMD_DELAY:
      esp_rom_delay_us(DapCtrl_U16(in + 1));
      out[0] = cmd;
      out[1] = DAP_OK;
      return 2;
    case CMD_RESET_TARGET:
      out[0] = cmd;
      out[1] = 0;
      return 2;
    case CMD_SWD_CONFIGURE:
      turnaround = (uint8_t)((in[1] & 0x03U) + 1U);
      dataPhase = (in[1] & 0x04U) != 0;
      out[0] = cmd;
      out[1] = DAP_OK;
      return 2;
    case CMD_SWJ_CLOCK: {
      uint32_t hz = DapCtrl_U32(in + 1);
      out[0] = cmd;
      if (hz == 0) {
        out[1] = DAP_ERROR;
        return 2;
      }
      clockHz = hz;
      if (swdPort) DapCtrl_ApplyClock(hz);
      out[1] = DAP_OK;
      return 2;
    }
    case CMD_TRANSFER_ABORT:
      transferAbort = true;
      out[0] = cmd;
      out[1] = DAP_OK;
      return 2;
    case CMD_SWJ_SEQUENCE: {
      out[0] = cmd;
      if (!swdPort) {
        out[1] = DAP_ERROR;
        return 2;
      }
      unsigned bits = in[1] == 0 ? 256U : in[1];
      DapCtrl_WriteBits(in + 2, bits);
      out[1] = DAP_OK;
      return 2;
    }
    case CMD_SWD_SEQUENCE: {
      if (outMax < 2) {
        *used = 0;
        return 0;
      }
      out[0] = cmd;
      if (!swdPort) {
        out[1] = DAP_ERROR;
        return 2;
      }
      size_t ip = 2;
      size_t op = 2;
      out[1] = DAP_OK;
      for (uint8_t i = 0; i < in[1]; i++) {
        uint8_t info = in[ip++];
        unsigned clocks = info & 0x3FU;
        if (clocks == 0) clocks = 64;
        size_t bytes = (clocks + 7U) / 8U;
        if (info & 0x80U) {
          if (op + bytes > outMax) {
            out[1] = DAP_ERROR;
            return op;
          }
          DapCtrl_ReadBits(out + op, clocks);
          op += bytes;
        } else {
          DapCtrl_WriteBits(in + ip, clocks);
          ip += bytes;
        }
      }
      return op;
    }
    case CMD_SWJ_PINS: {
      uint8_t value = in[1];
      uint8_t select = in[2];
      uint32_t waitUs = DapCtrl_U32(in + 3);
      if (swdPort && (select & (PIN_SWCLK | PIN_SWDIO))) {
        portENTER_CRITICAL(&dapMux);
        DapCtrl_Drive(true);
        if (select & PIN_SWCLK) DapCtrl_Level(swclkMask, (value & PIN_SWCLK) ? swclkMask : 0);
        if (select & PIN_SWDIO) DapCtrl_Level(swdioMask, (value & PIN_SWDIO) ? swdioMask : 0);
        portEXIT_CRITICAL(&dapMux);
      }
      if (select & PIN_NRESET) DapCtrl_Nreset((value & PIN_NRESET) != 0);
      uint8_t pins = DapCtrl_SamplePins();
      if (waitUs != 0) {
        int64_t deadline = esp_timer_get_time() + (int64_t)waitUs;
        while ((pins & select) != (value & select) && esp_timer_get_time() < deadline) pins = DapCtrl_SamplePins();
      }
      out[0] = cmd;
      out[1] = pins;
      return 2;
    }
    case CMD_WRITE_ABORT: {
      out[0] = cmd;
      if (!swdPort || in[1] != 0) {
        out[1] = DAP_ERROR;
        return 2;
      }
      uint32_t word = DapCtrl_U32(in + 2);
      uint8_t ack = DapCtrl_Transfer(0x00, &word);
      out[1] = ack == ACK_OK ? DAP_OK : DAP_ERROR;
      return 2;
    }
    case CMD_TRANSFER:
    case CMD_TRANSFER_BLOCK: {
      bool block = cmd == CMD_TRANSFER_BLOCK;
      uint16_t count = block ? DapCtrl_U16(in + 2) : in[2];
      uint8_t index = in[1];
      size_t header = block ? 4 : 3;
      if (outMax < header) {
        *used = 0;
        return 0;
      }
      transferAbort = false;
      out[0] = cmd;
      if (!swdPort || index != 0) {
        if (block) {
          DapCtrl_PutU16(out + 1, 0);
          out[3] = ACK_ERROR;
        } else {
          out[1] = 0;
          out[2] = ACK_ERROR;
        }
        return header;
      }

      if (!block) {
        size_t need = 3;
        size_t ip = 3;
        for (uint8_t i = 0; i < (uint8_t)count; i++) {
          size_t item = 0;
          bool read = false;
          bool timestamp = false;
          if (!DapCtrl_TransferItem(in + ip, span - ip, &item, &read, NULL, NULL, &timestamp)) break;
          if (read) need += 4;
          if (timestamp) need += 4;
          ip += item;
        }
        if (need > outMax) {
          out[1] = 0;
          out[2] = ACK_ERROR;
          return 3;
        }
        ip = 3;
        size_t op = 3;
        uint8_t done = 0;
        uint8_t finalAck = ACK_OK;
        for (uint8_t i = 0; i < (uint8_t)count; i++) {
          size_t item = 0;
          bool read, match, masked, timestamp;
          if (!DapCtrl_TransferItem(in + ip, span - ip, &item, &read, &match, &masked, &timestamp)) {
            finalAck = ACK_ERROR;
            break;
          }
          uint8_t req = in[ip];
          /* Regular transfer packets can also contain long runs of the same register access. */
          if ((req & 0xF0U) == 0) {
            size_t stride = read ? 1 : 5;
            uint16_t run = 1;
            while (i + run < count && in[ip + (size_t)run * stride] == req) run++;
            if (run > 1 && (read || (size_t)run * 4 <= outMax - op)) {
              /* Writes have no payload response, so its unused tail can hold the packed words. */
              if (!read) {
                for (uint16_t j = 0; j < run; j++) {
                  DapCtrl_PutU32(out + op + (size_t)j * 4, DapCtrl_U32(in + ip + (size_t)j * stride + 1));
                }
              }
              uint16_t completed = 0;
              uint8_t ack = DapCtrl_TransferBlock(req, run, read ? NULL : out + op, out + op, &completed);
              done += completed;
              if (read) op += (size_t)completed * 4;
              ip += (size_t)run * stride;
              i += run - 1;
              if (ack != ACK_OK) {
                finalAck = ack;
                break;
              }
              continue;
            }
          }
          uint32_t value = 0;
          uint8_t ack;
          if (!read) {
            value = DapCtrl_U32(in + ip + 1);
            ack = DapCtrl_TransferRegister(req & 0x0FU, &value);
          } else if (match) {
            uint32_t expect = DapCtrl_U32(in + ip + 1);
            uint32_t mask = masked ? DapCtrl_U32(in + ip + 5) : 0xFFFFFFFFU;
            ack = DapCtrl_MatchRead(req & 0x0FU, expect, mask, &value);
          } else {
            ack = DapCtrl_TransferRegister(req & 0x0FU, &value);
          }
          ip += item;
          if (ack == ACK_OK || (ack & ACK_MISMATCH) != 0) {
            if (read) {
              DapCtrl_PutU32(out + op, value);
              op += 4;
            }
            if (timestamp) {
              DapCtrl_PutU32(out + op, swdTimestamp);
              op += 4;
            }
            done++;
            if (ack & ACK_MISMATCH) {
              finalAck = ack;
              break;
            }
          } else {
            finalAck = ack;
            break;
          }
        }
        out[1] = done;
        out[2] = finalAck;
        return op;
      }

      bool read = (in[4] & 0x02U) != 0;
      size_t need = 4 + (read ? (size_t)count * 4 : 0);
      if (need > outMax) {
        DapCtrl_PutU16(out + 1, 0);
        out[3] = ACK_ERROR;
        return 4;
      }
      uint8_t req = in[4] & 0x0FU;
      const uint8_t* words = in + 5;
      uint16_t done = 0;
      uint8_t finalAck = DapCtrl_TransferBlock(req, count, words, out + 4, &done);
      DapCtrl_PutU16(out + 1, done);
      out[3] = finalAck;
      return 4 + (read ? (size_t)done * 4 : 0);
    }
    default:
      out[0] = cmd;
      out[1] = DAP_ERROR;
      return 2;
  }
}

static void DapCtrl_RunList(const uint8_t* cmds, size_t cmdsLen, uint8_t* out, size_t outMax, size_t* op, uint8_t* executed)
{
  size_t ip = 0;
  while (ip < cmdsLen && *executed < 255) {
    if (*op + 2 > outMax) break;
    size_t used = 0;
    size_t wrote = DapCtrl_Exec(cmds + ip, cmdsLen - ip, out + *op, outMax - *op, &used);
    if (used == 0 || wrote == 0) break;
    ip += used;
    *op += wrote;
    (*executed)++;
  }
}

static size_t DapCtrl_Queue(const uint8_t* in, size_t inLen, uint8_t* out, size_t outMax)
{
  if (outMax < 2) return 0;
  out[0] = CMD_QUEUE;
  if (inLen < 2) {
    out[1] = DAP_ERROR;
    return 2;
  }
  uint8_t count = in[1];
  size_t ip = 2;
  for (uint8_t i = 0; i < count; i++) {
    size_t span = DapCtrl_Span(in + ip, inLen - ip);
    if (span == 0) {
      out[1] = DAP_ERROR;
      return 2;
    }
    ip += span;
  }
  size_t bytes = ip - 2;
  if (queueLen + bytes > sizeof queue || (unsigned)queueCount + count > 255U) {
    out[1] = DAP_ERROR;
    return 2;
  }
  if (bytes != 0) memcpy(queue + queueLen, in + 2, bytes);
  queueLen += bytes;
  queueCount = (uint8_t)(queueCount + count);
  out[1] = queueCount;
  return 2;
}

static size_t DapCtrl_Execute(const uint8_t* in, size_t inLen, uint8_t* out, size_t outMax)
{
  if (outMax < 2 || inLen < 2) return 0;
  memcpy(queueCopy, queue, queueLen);
  size_t savedLen = queueLen;
  queueLen = 0;
  queueCount = 0;
  uint8_t executed = 0;
  size_t op = 2;
  DapCtrl_RunList(queueCopy, savedLen, out, outMax, &op, &executed);

  uint8_t count = in[1];
  size_t ip = 2;
  for (uint8_t i = 0; i < count; i++) {
    if (op + 2 > outMax) break;
    size_t used = 0;
    size_t wrote = DapCtrl_Exec(in + ip, inLen - ip, out + op, outMax - op, &used);
    if (used == 0 || wrote == 0) break;
    ip += used;
    op += wrote;
    executed++;
  }
  out[0] = CMD_EXECUTE;
  out[1] = executed;
  return op;
}

static size_t DapCtrl_Run(const uint8_t* in, size_t inLen, uint8_t* out, size_t outMax)
{
  if (inLen < 1 || outMax < 2) return 0;
  if (in[0] == CMD_QUEUE) return DapCtrl_Queue(in, inLen, out, outMax);
  size_t n = 0;
  if (in[0] == CMD_EXECUTE) {
    n = DapCtrl_Execute(in, inLen, out, outMax);
  } else {
    size_t used = 0;
    n = DapCtrl_Exec(in, inLen, out, outMax, &used);
  }
  if (n != 0) return n;
  out[0] = in[0];
  out[1] = DAP_ERROR;
  return 2;
}

static esp_err_t DapCtrl_ValidateConfig(const DapCtrl_Config* cfg, char* reason, size_t reasonLen)
{
  if (!GpioCtrl_IsValidLogicalPin(cfg->swclkPin)) {
    snprintf(reason, reasonLen, "pins.swclk is invalid. Use a pin from GET /pin/.");
    return ESP_ERR_INVALID_ARG;
  }
  if (!GpioCtrl_IsValidLogicalPin(cfg->swdioPin)) {
    snprintf(reason, reasonLen, "pins.swdio is invalid. Use a pin from GET /pin/.");
    return ESP_ERR_INVALID_ARG;
  }
  if (cfg->swclkPin == cfg->swdioPin) {
    snprintf(reason, reasonLen, "pins.swclk and pins.swdio must be different pins.");
    return ESP_ERR_INVALID_ARG;
  }
  if (cfg->nresetPin >= 0) {
    if (!GpioCtrl_IsValidLogicalPin(cfg->nresetPin)) {
      snprintf(reason, reasonLen, "pins.nreset is invalid. Use a pin from GET /pin/, or null.");
      return ESP_ERR_INVALID_ARG;
    }
    if (cfg->nresetPin == cfg->swclkPin || cfg->nresetPin == cfg->swdioPin) {
      snprintf(reason, reasonLen, "pins.nreset must differ from pins.swclk and pins.swdio.");
      return ESP_ERR_INVALID_ARG;
    }
  }
  return ESP_OK;
}

static void DapCtrl_FillResources(const DapCtrl_Config* cfg, Lock_Resource* out, size_t* countOut)
{
  size_t n = 0;
  out[n++] = (Lock_Resource){
      .kind = LOCK_KIND_GPIO, .pin = cfg->swclkPin, .methods = LOCK_METHOD_READ | LOCK_METHOD_WRITE};
  out[n++] = (Lock_Resource){
      .kind = LOCK_KIND_GPIO, .pin = cfg->swdioPin, .methods = LOCK_METHOD_READ | LOCK_METHOD_WRITE};
  if (cfg->nresetPin >= 0) {
    out[n++] = (Lock_Resource){
        .kind = LOCK_KIND_GPIO, .pin = cfg->nresetPin, .methods = LOCK_METHOD_READ | LOCK_METHOD_WRITE};
  }
  *countOut = n;
}

static esp_err_t DapCtrl_ClaimAndStart(const DapCtrl_Config* next, char* reason, size_t reasonLen)
{
  Lock_Resource resources[3];
  size_t count = 0;
  DapCtrl_FillResources(next, resources, &count);
  Lock_Conflict conflict;
  memset(&conflict, 0, sizeof(conflict));
  esp_err_t aret = Lock_AcquirePeripheral("dap", resources, count, &conflict);
  if (aret == ESP_ERR_INVALID_STATE) {
    if (conflict.isPeripheral) {
      const char* label = conflict.peripheralLabel[0] ? conflict.peripheralLabel : "a peripheral";
      snprintf(reason, reasonLen, "Pin %d is reserved by %s. Disable %s or use other pins.", conflict.pin, label, label);
    } else {
      snprintf(reason, reasonLen,
               "Pin %d is already held by a lock. DELETE that lock or wait until it expires before enabling CMSIS-DAP.",
               conflict.pin);
    }
    return ESP_ERR_INVALID_STATE;
  }
  if (aret != ESP_OK) {
    snprintf(reason, reasonLen, "internal");
    return aret;
  }
  if (Tcp_DapStart() != ESP_OK) {
    Lock_ReleasePeripheral("dap");
    snprintf(reason, reasonLen, "CMSIS-DAP TCP service failed to start.");
    return ESP_FAIL;
  }
  return ESP_OK;
}

static void DapCtrl_StopAndRelease(void)
{
  Tcp_DapStop();
  DapCtrl_ReleasePins();
  Lock_ReleasePeripheral("dap");
}

esp_err_t DapCtrl_Init(void)
{
  memset(&config, 0, sizeof(config));
  config.enable = false;
  config.swclkPin = CONFIG_DAP_SWCLK_LOGICAL;
  config.swdioPin = CONFIG_DAP_SWDIO_LOGICAL;
  config.nresetPin = CONFIG_DAP_NRESET_LOGICAL;
  session = false;
  mutex = xSemaphoreCreateMutex();
  if (mutex == NULL) {
    ESP_LOGE(tag, "mutex create failed");
    return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t DapCtrl_GetConfig(DapCtrl_Config* out)
{
  if (out == NULL) return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(mutex, portMAX_DELAY);
  *out = config;
  xSemaphoreGive(mutex);
  return ESP_OK;
}

esp_err_t DapCtrl_SetConfig(const DapCtrl_Config* cfg, char* reason, size_t reasonLen)
{
  if (cfg == NULL) return ESP_ERR_INVALID_ARG;
  if (DapCtrl_ValidateConfig(cfg, reason, reasonLen) != ESP_OK) return ESP_ERR_INVALID_ARG;

  xSemaphoreTake(mutex, portMAX_DELAY);
  DapCtrl_Config previous = config;
  bool wasEnabled = config.enable;

  if (!cfg->enable) {
    if (wasEnabled) DapCtrl_StopAndRelease();
    config = *cfg;
    config.enable = false;
    xSemaphoreGive(mutex);
    return ESP_OK;
  }

  if (wasEnabled) DapCtrl_StopAndRelease();
  config = *cfg;
  config.enable = true;

  esp_err_t err = DapCtrl_ClaimAndStart(&config, reason, reasonLen);
  if (err != ESP_OK) {
    if (wasEnabled && previous.enable) {
      config = previous;
      char restoreReason[160];
      if (DapCtrl_ClaimAndStart(&config, restoreReason, sizeof(restoreReason)) != ESP_OK) {
        ESP_LOGW(tag, "Failed to restore CMSIS-DAP after config error: %s", restoreReason);
        config.enable = false;
      }
    } else {
      config = *cfg;
      config.enable = false;
    }
    xSemaphoreGive(mutex);
    return err;
  }
  config.enable = true;
  xSemaphoreGive(mutex);
  return ESP_OK;
}

esp_err_t DapCtrl_GetStatus(DapCtrl_Status* out)
{
  if (out == NULL) return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(mutex, portMAX_DELAY);
  bool running = config.enable && Tcp_DapIsRunning();
  xSemaphoreGive(mutex);
  if (!running) return ESP_ERR_INVALID_STATE;

  memset(out, 0, sizeof(*out));
  out->prompt = CONFIG_DAP_STATUS_PROMPT;
  out->port = CONFIG_DAP_TCP_PORT;
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

esp_err_t DapCtrl_OnConnect(DapCtrl_SendFn send, void* user)
{
  if (send == NULL) return ESP_ERR_INVALID_ARG;
  if (session || !config.enable) return ESP_ERR_INVALID_STATE;
  session = true;
  sendFn = send;
  sendUser = user;
  DapCtrl_ResetSession();
  return ESP_OK;
}

void DapCtrl_OnData(const uint8_t* data, size_t length)
{
  if (!session || sendFn == NULL || data == NULL || length == 0) return;
  size_t n = DapCtrl_Run(data, length, response, sizeof response);
  if (n > 0) sendFn(sendUser, response, n);
}

void DapCtrl_OnDisconnect(void)
{
  if (!session) return;
  DapCtrl_ReleasePins();
  session = false;
  sendFn = NULL;
  sendUser = NULL;
  DapCtrl_ResetSession();
}
