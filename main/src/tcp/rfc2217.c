/**
 * @name RFC2217 TCP service
 * @file rfc2217.c
 * @author xqe2011
 *
 * Telnet COM Port Control (RFC 2217) over one TCP client. Bytes and line changes go through UartCtrl.
 */
#include "tcp.h"

#include "config.h"
#include "uart_ctrl.h"

#include <errno.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <fcntl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char* tag = "SAIHub-Rfc2217";

#define IAC 255u
#define DONT 254u
#define DO 253u
#define WONT 252u
#define WILL 251u
#define SB 250u
#define SE 240u

#define OPT_BINARY 0u
#define OPT_SGA 3u
#define OPT_COM 44u

#define CMD_SIGNATURE 0u
#define CMD_BAUDRATE 1u
#define CMD_DATASIZE 2u
#define CMD_PARITY 3u
#define CMD_STOPSIZE 4u
#define CMD_CONTROL 5u
#define CMD_FLOW_SUSPEND 8u
#define CMD_FLOW_RESUME 9u
#define CMD_LINE_MASK 10u
#define CMD_MODEM_MASK 11u
#define CMD_PURGE 12u
#define CMD_SERVER 100u

#define CTL_FLOW_NONE 1u
#define CTL_BREAK_OFF 6u
#define CTL_DTR_ON 8u
#define CTL_DTR_OFF 9u
#define CTL_RTS_ON 11u
#define CTL_RTS_OFF 12u
#define CTL_IN_FLOW_NONE 14u

#define PARITY_NONE 1u
#define PARITY_ODD 2u
#define PARITY_EVEN 3u

enum {
  ST_DATA = 0,
  ST_IAC,
  ST_OPT,
  ST_SB,
  ST_SB_IAC,
};

static int clientFd = -1;
static volatile int listenFd = -1;
static volatile bool stopRequested;
static volatile bool running;
static TaskHandle_t bridgeTask;
static TaskHandle_t starter;
static SemaphoreHandle_t stopped;
static int bridgeUartId = -1;
static bool sendFailed;
static bool outputSuspended;
static bool restoreWifiPs;
static wifi_ps_type_t savedWifiPs;

static int state;
static uint8_t optCmd;
static uint8_t sb[48];
static size_t sbLen;
static uint8_t txBuf[256];
static size_t txLen;
static bool willBinary, doBinary, willSga, doSga, willCom, doCom;
static uint8_t wontBits[32];
static uint8_t dontBits[32];

static void Rfc_SendRaw(const uint8_t* data, size_t len)
{
  size_t off = 0;
  while (off < len) {
    if (clientFd < 0 || sendFailed || stopRequested) {
      sendFailed = true;
      return;
    }
    int n = send(clientFd, data + off, len - off, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      sendFailed = true;
      return;
    }
    if (n == 0) {
      sendFailed = true;
      return;
    }
    off += (size_t)n;
  }
}

static void Rfc_SendOpt(uint8_t cmd, uint8_t opt)
{
  uint8_t pkt[3] = {IAC, cmd, opt};
  Rfc_SendRaw(pkt, sizeof pkt);
}

static void Rfc_Reply(uint8_t cmd, const uint8_t* arg, size_t argLen)
{
  uint8_t pkt[64];
  if (argLen > sizeof(pkt) - 6) argLen = sizeof(pkt) - 6;
  size_t n = 0;
  pkt[n++] = IAC;
  pkt[n++] = SB;
  pkt[n++] = OPT_COM;
  pkt[n++] = cmd;
  if (argLen > 0 && arg != NULL) {
    memcpy(pkt + n, arg, argLen);
    n += argLen;
  }
  pkt[n++] = IAC;
  pkt[n++] = SE;
  Rfc_SendRaw(pkt, n);
}

static void Rfc_FlushTx(void)
{
  size_t off = 0;
  while (off < txLen) {
    size_t n = txLen - off;
    if (n > CONFIG_UART_MAX_PAYLOAD_BYTES) n = CONFIG_UART_MAX_PAYLOAD_BYTES;
    char reason[128];
    if (UartCtrl_Transmit(bridgeUartId, txBuf + off, n, reason, sizeof(reason)) != ESP_OK) {
      ESP_LOGW(tag, "transmit failed: %s", reason);
      break;
    }
    off += n;
  }
  txLen = 0;
}

static void Rfc_Emit(uint8_t byte)
{
  if (txLen == sizeof txBuf) Rfc_FlushTx();
  if (txLen < sizeof txBuf) txBuf[txLen++] = byte;
}

static bool Rfc_Bit(const uint8_t* bits, uint8_t opt)
{
  return (bits[opt >> 3] & (uint8_t)(1u << (opt & 7))) != 0;
}

static void Rfc_SetBit(uint8_t* bits, uint8_t opt)
{
  bits[opt >> 3] |= (uint8_t)(1u << (opt & 7));
}

static void Rfc_OnOption(uint8_t cmd, uint8_t opt)
{
  bool known = opt == OPT_BINARY || opt == OPT_SGA || opt == OPT_COM;
  bool* will = opt == OPT_BINARY ? &willBinary : opt == OPT_SGA ? &willSga : &willCom;
  bool* agreed = opt == OPT_BINARY ? &doBinary : opt == OPT_SGA ? &doSga : &doCom;
  if (cmd == DO) {
    if (known) {
      if (!*will) {
        Rfc_SendOpt(WILL, opt);
        *will = true;
      }
    } else if (!Rfc_Bit(wontBits, opt)) {
      Rfc_SendOpt(WONT, opt);
      Rfc_SetBit(wontBits, opt);
    }
  } else if (cmd == WILL) {
    if (known) {
      if (!*agreed) {
        Rfc_SendOpt(DO, opt);
        *agreed = true;
      }
    } else if (!Rfc_Bit(dontBits, opt)) {
      Rfc_SendOpt(DONT, opt);
      Rfc_SetBit(dontBits, opt);
    }
  } else if (cmd == DONT) {
    if (!Rfc_Bit(wontBits, opt)) {
      Rfc_SendOpt(WONT, opt);
      Rfc_SetBit(wontBits, opt);
    }
    if (known) *will = false;
  } else if (cmd == WONT) {
    if (!Rfc_Bit(dontBits, opt)) {
      Rfc_SendOpt(DONT, opt);
      Rfc_SetBit(dontBits, opt);
    }
    if (known) *agreed = false;
  }
}

static void Rfc_Greet(void)
{
  Rfc_SendOpt(WILL, OPT_COM);
  Rfc_SendOpt(WILL, OPT_BINARY);
  Rfc_SendOpt(DO, OPT_BINARY);
  Rfc_SendOpt(WILL, OPT_SGA);
  Rfc_SendOpt(DO, OPT_SGA);
  willCom = willBinary = doBinary = willSga = doSga = true;
}

static uint32_t Rfc_Be32(const uint8_t* p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void Rfc_PutBe32(uint8_t* p, uint32_t value)
{
  p[0] = (uint8_t)(value >> 24);
  p[1] = (uint8_t)(value >> 16);
  p[2] = (uint8_t)(value >> 8);
  p[3] = (uint8_t)value;
}

static bool Rfc_Load(UartCtrl_Config* cfg)
{
  return UartCtrl_GetConfig(bridgeUartId, cfg) == ESP_OK;
}

static void Rfc_Store(UartCtrl_Config* cfg)
{
  char reason[160];
  if (UartCtrl_SetConfig(bridgeUartId, cfg, reason, sizeof(reason)) != ESP_OK) {
    ESP_LOGW(tag, "line change rejected: %s", reason);
    (void)Rfc_Load(cfg);
  }
}

static uint8_t Rfc_ParityByte(UartCtrl_Parity parity)
{
  if (parity == UART_CTRL_PARITY_EVEN) return PARITY_EVEN;
  if (parity == UART_CTRL_PARITY_ODD) return PARITY_ODD;
  return PARITY_NONE;
}

static bool Rfc_LineActive(bool isDtr)
{
  bool active = false;
  char reason[96];
  esp_err_t err = isDtr ? UartCtrl_GetDtrActive(bridgeUartId, &active, reason, sizeof(reason))
                        : UartCtrl_GetRtsActive(bridgeUartId, &active, reason, sizeof(reason));
  return err == ESP_OK && active;
}

static void Rfc_SetLine(bool isDtr, bool active)
{
  char reason[96];
  esp_err_t err = isDtr ? UartCtrl_SetDtr(bridgeUartId, active, reason, sizeof(reason))
                        : UartCtrl_SetRts(bridgeUartId, active, reason, sizeof(reason));
  if (err != ESP_OK) ESP_LOGW(tag, "%s rejected: %s", isDtr ? "dtr" : "rts", reason);
}

static void Rfc_OnControl(uint8_t value)
{
  uint8_t reply = CTL_IN_FLOW_NONE;
  switch (value) {
    case 0:
    case 1:
    case 2:
    case 3:
      reply = CTL_FLOW_NONE;
      break;
    case 4:
    case 5:
    case 6:
      reply = CTL_BREAK_OFF;
      break;
    case 7:
      reply = Rfc_LineActive(true) ? CTL_DTR_ON : CTL_DTR_OFF;
      break;
    case CTL_DTR_ON:
    case CTL_DTR_OFF:
      Rfc_SetLine(true, value == CTL_DTR_ON);
      reply = Rfc_LineActive(true) ? CTL_DTR_ON : CTL_DTR_OFF;
      break;
    case 10:
      reply = Rfc_LineActive(false) ? CTL_RTS_ON : CTL_RTS_OFF;
      break;
    case CTL_RTS_ON:
    case CTL_RTS_OFF:
      Rfc_SetLine(false, value == CTL_RTS_ON);
      reply = Rfc_LineActive(false) ? CTL_RTS_ON : CTL_RTS_OFF;
      break;
    default:
      reply = CTL_IN_FLOW_NONE;
      break;
  }
  Rfc_Reply(CMD_CONTROL + CMD_SERVER, &reply, 1);
}

static void Rfc_OnSub(void)
{
  if (sbLen < 2 || sb[0] != OPT_COM) return;
  uint8_t cmd = sb[1];
  const uint8_t* arg = sb + 2;
  size_t argLen = sbLen - 2;
  if (cmd >= CMD_SERVER) return;

  if (cmd == CMD_FLOW_SUSPEND) {
    outputSuspended = true;
    return;
  }
  if (cmd == CMD_FLOW_RESUME) {
    outputSuspended = false;
    return;
  }
  if (cmd == CMD_SIGNATURE) {
    static const uint8_t name[] = "saihub";
    Rfc_Reply(CMD_SIGNATURE + CMD_SERVER, name, sizeof(name) - 1);
    return;
  }
  if (cmd == CMD_LINE_MASK || cmd == CMD_MODEM_MASK) {
    Rfc_Reply(cmd + CMD_SERVER, arg, argLen > 16 ? 16 : argLen);
    return;
  }
  if (cmd == CMD_PURGE) {
    if (argLen >= 1 && (arg[0] == 1 || arg[0] == 3)) {
      char reason[96];
      (void)UartCtrl_Flush(bridgeUartId, reason, sizeof(reason));
    }
    if (argLen >= 1) Rfc_Reply(CMD_PURGE + CMD_SERVER, arg, 1);
    return;
  }

  UartCtrl_Config cfg;
  if (!Rfc_Load(&cfg)) return;

  if (cmd == CMD_BAUDRATE && argLen >= 4) {
    uint32_t baud = Rfc_Be32(arg);
    if (baud >= 1 && baud <= 10000000u && (int)baud != cfg.baudRate) {
      cfg.baudRate = (int)baud;
      Rfc_Store(&cfg);
    }
    uint8_t out[4];
    Rfc_PutBe32(out, (uint32_t)cfg.baudRate);
    Rfc_Reply(CMD_BAUDRATE + CMD_SERVER, out, sizeof out);
    return;
  }
  if (cmd == CMD_DATASIZE && argLen >= 1) {
    if (arg[0] >= 5 && arg[0] <= 8 && arg[0] != (uint8_t)cfg.dataBits) {
      cfg.dataBits = arg[0];
      Rfc_Store(&cfg);
    }
    uint8_t out = (uint8_t)cfg.dataBits;
    Rfc_Reply(CMD_DATASIZE + CMD_SERVER, &out, 1);
    return;
  }
  if (cmd == CMD_PARITY && argLen >= 1) {
    UartCtrl_Parity parity = cfg.parity;
    bool known = true;
    if (arg[0] == PARITY_NONE) parity = UART_CTRL_PARITY_NONE;
    else if (arg[0] == PARITY_ODD) parity = UART_CTRL_PARITY_ODD;
    else if (arg[0] == PARITY_EVEN) parity = UART_CTRL_PARITY_EVEN;
    else known = false;
    if (known && parity != cfg.parity) {
      cfg.parity = parity;
      Rfc_Store(&cfg);
    }
    uint8_t out = Rfc_ParityByte(cfg.parity);
    Rfc_Reply(CMD_PARITY + CMD_SERVER, &out, 1);
    return;
  }
  if (cmd == CMD_STOPSIZE && argLen >= 1) {
    if ((arg[0] == 1 || arg[0] == 2) && arg[0] != (uint8_t)cfg.stopBits) {
      cfg.stopBits = arg[0];
      Rfc_Store(&cfg);
    }
    uint8_t out = (uint8_t)cfg.stopBits;
    Rfc_Reply(CMD_STOPSIZE + CMD_SERVER, &out, 1);
    return;
  }
  if (cmd == CMD_CONTROL && argLen >= 1) Rfc_OnControl(arg[0]);
}

static void Rfc_Byte(uint8_t byte)
{
  switch (state) {
    case ST_DATA:
      if (byte == IAC) state = ST_IAC;
      else Rfc_Emit(byte);
      break;
    case ST_IAC:
      if (byte == IAC) {
        Rfc_Emit(IAC);
        state = ST_DATA;
      } else if (byte == SB) {
        Rfc_FlushTx();
        sbLen = 0;
        state = ST_SB;
      } else if (byte == WILL || byte == WONT || byte == DO || byte == DONT) {
        Rfc_FlushTx();
        optCmd = byte;
        state = ST_OPT;
      } else {
        state = ST_DATA;
      }
      break;
    case ST_OPT:
      Rfc_OnOption(optCmd, byte);
      state = ST_DATA;
      break;
    case ST_SB:
      if (byte == IAC) state = ST_SB_IAC;
      else if (sbLen < sizeof sb) sb[sbLen++] = byte;
      break;
    case ST_SB_IAC:
      if (byte == IAC) {
        if (sbLen < sizeof sb) sb[sbLen++] = IAC;
        state = ST_SB;
      } else if (byte == SE) {
        Rfc_OnSub();
        state = ST_DATA;
      } else {
        state = ST_DATA;
      }
      break;
    default:
      state = ST_DATA;
      break;
  }
}

static void Rfc_SendEscaped(const uint8_t* data, size_t len)
{
  uint8_t out[256];
  size_t n = 0;
  for (size_t i = 0; i < len; i++) {
    if (n + 2 > sizeof out) {
      Rfc_SendRaw(out, n);
      n = 0;
      if (sendFailed) return;
    }
    out[n++] = data[i];
    if (data[i] == IAC) out[n++] = IAC;
  }
  if (n > 0) Rfc_SendRaw(out, n);
}

static void Rfc_DrainUart(void)
{
  if (clientFd < 0 || outputSuspended || sendFailed) return;
  for (int i = 0; i < 8; i++) {
    uint8_t raw[256];
    size_t n = 0;
    char reason[64];
    if (UartCtrl_Receive(bridgeUartId, raw, sizeof raw, &n, reason, sizeof(reason)) != ESP_OK || n == 0) return;
    Rfc_SendEscaped(raw, n);
    if (sendFailed || n < sizeof raw) return;
  }
}

static void Rfc_ResetSession(void)
{
  state = ST_DATA;
  sbLen = 0;
  txLen = 0;
  sendFailed = false;
  outputSuspended = false;
  willBinary = doBinary = willSga = doSga = willCom = doCom = false;
  memset(wontBits, 0, sizeof wontBits);
  memset(dontBits, 0, sizeof dontBits);
}

static void Rfc_Drop(void)
{
  if (clientFd < 0) return;
  Rfc_FlushTx();
  close(clientFd);
  clientFd = -1;
  if (restoreWifiPs) {
    esp_wifi_set_ps(savedWifiPs);
    restoreWifiPs = false;
  }
  Rfc_ResetSession();
  ESP_LOGI(tag, "client disconnected");
}

static void Rfc_Exit(int result)
{
  if (starter != NULL) xTaskNotify(starter, (uint32_t)result, eSetValueWithOverwrite);
  running = false;
  listenFd = -1;
  bridgeUartId = -1;
  bridgeTask = NULL;
  if (stopped != NULL) xSemaphoreGive(stopped);
  vTaskDelete(NULL);
}

static void Rfc_Task(void* arg)
{
  (void)arg;
  bridgeTask = xTaskGetCurrentTaskHandle();
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (fd < 0) {
    ESP_LOGE(tag, "socket failed: %s", strerror(errno));
    Rfc_Exit(0);
    return;
  }

  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(CONFIG_RFC2217_TCP_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, (struct sockaddr*)&addr, sizeof addr) < 0 || listen(fd, 1) < 0) {
    ESP_LOGE(tag, "listen %d failed: %s", CONFIG_RFC2217_TCP_PORT, strerror(errno));
    close(fd);
    Rfc_Exit(0);
    return;
  }

  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) flags = 0;
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  listenFd = fd;
  running = true;
  ESP_LOGI(tag, "RFC2217 listening on %d", CONFIG_RFC2217_TCP_PORT);
  if (starter != NULL) xTaskNotify(starter, 1, eSetValueWithOverwrite);

  while (!stopRequested) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(fd, &readable);
    int maxFd = fd;
    if (clientFd >= 0) {
      FD_SET(clientFd, &readable);
      if (clientFd > maxFd) maxFd = clientFd;
    }

    struct timeval wait = {.tv_sec = 0, .tv_usec = 20000};
    int ready = select(maxFd + 1, &readable, NULL, NULL, &wait);
    if (stopRequested) break;
    if (ready < 0) {
      if (errno == EINTR) continue;
      ESP_LOGE(tag, "select failed: %s", strerror(errno));
      break;
    }

    if (ready > 0 && FD_ISSET(fd, &readable)) {
      struct sockaddr_in peer = {0};
      socklen_t peerLen = sizeof peer;
      int accepted = accept(fd, (struct sockaddr*)&peer, &peerLen);
      if (accepted >= 0) {
        if (clientFd >= 0 || stopRequested) {
          if (clientFd >= 0) ESP_LOGW(tag, "dropping extra client");
          close(accepted);
        } else {
          int nodelay = 1;
          setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);
          int keepalive = 1;
          setsockopt(accepted, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof keepalive);
          int idle = 1;
          setsockopt(accepted, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
          setsockopt(accepted, IPPROTO_TCP, TCP_KEEPINTVL, &idle, sizeof idle);
          int count = 3;
          setsockopt(accepted, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof count);
          struct timeval sendTimeout = {.tv_sec = 2, .tv_usec = 0};
          setsockopt(accepted, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof sendTimeout);
          clientFd = accepted;
          Rfc_ResetSession();
          if (esp_wifi_get_ps(&savedWifiPs) == ESP_OK && savedWifiPs != WIFI_PS_NONE) {
            restoreWifiPs = esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK;
          }
          Rfc_Greet();
          ESP_LOGI(tag, "client connected");
          if (sendFailed) Rfc_Drop();
        }
      } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        ESP_LOGW(tag, "accept failed: %s", strerror(errno));
      }
    }

    if (clientFd >= 0 && ready > 0 && FD_ISSET(clientFd, &readable)) {
      uint8_t buf[512];
      int n = recv(clientFd, buf, sizeof buf, 0);
      if (n <= 0) {
        Rfc_Drop();
      } else {
        for (int i = 0; i < n && clientFd >= 0 && !sendFailed; i++) Rfc_Byte(buf[i]);
        Rfc_FlushTx();
        if (sendFailed) Rfc_Drop();
      }
    }

    if (clientFd >= 0 && !stopRequested) {
      Rfc_DrainUart();
      if (sendFailed) Rfc_Drop();
    }
  }

  Rfc_Drop();
  if (listenFd >= 0) {
    close(listenFd);
    listenFd = -1;
  } else {
    close(fd);
  }
  Rfc_Exit(1);
}

bool Tcp_Rfc2217IsRunning(void)
{
  return running;
}

bool Tcp_Rfc2217InTask(void)
{
  return bridgeTask != NULL && xTaskGetCurrentTaskHandle() == bridgeTask;
}

int Tcp_Rfc2217UartId(void)
{
  return running ? bridgeUartId : -1;
}

esp_err_t Tcp_Rfc2217Start(int uartId)
{
  if (!UartCtrl_IsValidId(uartId)) return ESP_ERR_INVALID_ARG;
  if (running || bridgeTask != NULL) return ESP_ERR_INVALID_STATE;
  if (stopped == NULL) {
    stopped = xSemaphoreCreateBinary();
    if (stopped == NULL) return ESP_FAIL;
  }
  bridgeUartId = uartId;
  stopRequested = false;
  starter = xTaskGetCurrentTaskHandle();
  xTaskNotifyStateClear(starter);
  if (xTaskCreatePinnedToCore(Rfc_Task, "saihub_rfc2217", 8192, NULL, 5, &bridgeTask, 0) != pdPASS) {
    bridgeTask = NULL;
    bridgeUartId = -1;
    starter = NULL;
    ESP_LOGE(tag, "task create failed");
    return ESP_FAIL;
  }
  uint32_t result = 0;
  if (xTaskNotifyWait(0, UINT32_MAX, &result, pdMS_TO_TICKS(2000)) != pdTRUE || result == 0) {
    stopRequested = true;
    if (listenFd >= 0) shutdown(listenFd, SHUT_RDWR);
    xSemaphoreTake(stopped, portMAX_DELAY);
    bridgeTask = NULL;
    bridgeUartId = -1;
    starter = NULL;
    stopRequested = false;
    return ESP_FAIL;
  }
  starter = NULL;
  return ESP_OK;
}

esp_err_t Tcp_Rfc2217Stop(void)
{
  if (!running && bridgeTask == NULL) return ESP_OK;
  stopRequested = true;
  int client = clientFd;
  if (client >= 0) shutdown(client, SHUT_RDWR);
  if (listenFd >= 0) shutdown(listenFd, SHUT_RDWR);
  if (stopped != NULL) xSemaphoreTake(stopped, portMAX_DELAY);
  bridgeTask = NULL;
  bridgeUartId = -1;
  stopRequested = false;
  return ESP_OK;
}
