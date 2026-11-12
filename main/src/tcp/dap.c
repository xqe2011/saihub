/**
 * @name CMSIS-DAP TCP service
 * @file dap.c
 * @author xqe2011
 *
 * OpenOCD cmsis-dap TCP framing. Each request payload is one CMSIS-DAP command.
 */
#include "tcp.h"

#include "cloud.h"
#include "config.h"
#include "dap_ctrl.h"

#include <errno.h>
#include <esp_log.h>
#include <esp_vfs.h>
#include <esp_wifi.h>
#include <fcntl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static const char* tag = "SAIHub-Dap";

#define TCP_HEADER 8
#define TCP_SIGNATURE 0x00504144u
#define TCP_REQUEST 1u
#define TCP_RESPONSE 2u

static int clientFd = -1;
static volatile int listenFd = -1;
static volatile bool stopRequested;
static volatile bool running;
static TaskHandle_t dapTask;
static TaskHandle_t starter;
static SemaphoreHandle_t stopped;
static bool sendFailed;
static bool restoreWifiPs;
static wifi_ps_type_t savedWifiPs;
static bool cloudClient;
static int eventFd = -1;
static uint8_t stream[2 * (TCP_HEADER + CONFIG_DAP_PACKET_SIZE)];
static uint8_t frame[TCP_HEADER + CONFIG_DAP_PACKET_SIZE];
static size_t frameLen;

static void Tcp_DapFlush(void)
{
  if (frameLen == 0) return;
  if (cloudClient) {
    if (Cloud_TcpSend(clientFd, frame, frameLen) != ESP_OK) sendFailed = true;
    frameLen = 0;
    return;
  }
  size_t off = 0;
  while (off < frameLen) {
    int n = send(clientFd, frame + off, frameLen - off, 0);
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
  frameLen = 0;
}

static void Tcp_DapSend(void* user, const uint8_t* data, size_t length)
{
  (void)user;
  if (clientFd < 0 || length > CONFIG_DAP_PACKET_SIZE || sendFailed) {
    sendFailed = true;
    return;
  }
  if (frameLen + TCP_HEADER + length > sizeof frame) Tcp_DapFlush();
  if (sendFailed) return;
  uint8_t* p = frame + frameLen;
  uint32_t signature = TCP_SIGNATURE;
  uint16_t payload = (uint16_t)length;
  memcpy(p, &signature, sizeof signature);
  memcpy(p + 4, &payload, sizeof payload);
  p[6] = TCP_RESPONSE;
  p[7] = 0;
  memcpy(p + TCP_HEADER, data, length);
  frameLen += TCP_HEADER + length;
}

static void Tcp_DapDrop(size_t* buffered)
{
  if (clientFd < 0) return;
  DapCtrl_OnDisconnect();
  if (cloudClient) Cloud_TcpClose(clientFd);
  else close(clientFd);
  clientFd = -1;
  cloudClient = false;
  if (restoreWifiPs) {
    esp_wifi_set_ps(savedWifiPs);
    restoreWifiPs = false;
  }
  sendFailed = false;
  frameLen = 0;
  *buffered = 0;
  ESP_LOGI(tag, "client disconnected");
}

static void Tcp_DapExit(int result)
{
  if (starter != NULL) {
    xTaskNotify(starter, (uint32_t)result, eSetValueWithOverwrite);
  }
  running = false;
  listenFd = -1;
  if (stopped != NULL) xSemaphoreGive(stopped);
  dapTask = NULL;
  vTaskDelete(NULL);
}

static void Tcp_DapTask(void* arg)
{
  (void)arg;
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (fd < 0) {
    ESP_LOGE(tag, "socket failed: %s", strerror(errno));
    Tcp_DapExit(0);
    return;
  }

  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(CONFIG_DAP_TCP_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, (struct sockaddr*)&addr, sizeof addr) < 0 || listen(fd, 1) < 0) {
    ESP_LOGE(tag, "listen %d failed: %s", CONFIG_DAP_TCP_PORT, strerror(errno));
    close(fd);
    Tcp_DapExit(0);
    return;
  }

  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) flags = 0;
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  listenFd = fd;
  eventFd = Cloud_TcpEventFd("dap");
  if (eventFd < 0) {
    ESP_LOGE(tag, "cloud wake setup failed");
    close(fd);
    listenFd = -1;
    Tcp_DapExit(0);
    return;
  }
  running = true;
  ESP_LOGI(tag, "CMSIS-DAP listening on %d", CONFIG_DAP_TCP_PORT);
  if (starter != NULL) xTaskNotify(starter, 1, eSetValueWithOverwrite);

  size_t buffered = 0;
  while (!stopRequested) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(fd, &readable);
    int maxFd = fd;
    if (clientFd >= 0) {
      FD_SET(clientFd, &readable);
      if (clientFd > maxFd) maxFd = clientFd;
    }
    if (eventFd >= 0) {
      FD_SET(eventFd, &readable);
      if (eventFd > maxFd) maxFd = eventFd;
    }

    struct timeval wait = {.tv_sec = 0, .tv_usec = 200000};
    int ready = esp_vfs_select(maxFd + 1, &readable, NULL, NULL, &wait);
    if (stopRequested) break;
    if (ready < 0) {
      if (errno == EINTR) continue;
      ESP_LOGE(tag, "select failed: %s", strerror(errno));
      break;
    }
    if (ready == 0) continue;

    if (eventFd >= 0 && FD_ISSET(eventFd, &readable)) {
      uint64_t count = 0;
      (void)read(eventFd, &count, sizeof count);
      if (clientFd >= 0 || stopRequested) {
        (void)Cloud_TcpAccept("dap", ESP_ERR_INVALID_STATE, "client already connected");
      } else if (DapCtrl_OnConnect(Tcp_DapSend, NULL) != ESP_OK) {
        (void)Cloud_TcpAccept("dap", ESP_FAIL, "could not start debugger session");
      } else {
        int accepted = Cloud_TcpAccept("dap", ESP_OK, NULL);
        if (accepted < 0) {
          DapCtrl_OnDisconnect();
        } else {
          clientFd = accepted;
          cloudClient = true;
          buffered = 0;
          sendFailed = false;
          frameLen = 0;
          if (esp_wifi_get_ps(&savedWifiPs) == ESP_OK && savedWifiPs != WIFI_PS_NONE) {
            restoreWifiPs = esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK;
          }
          ESP_LOGI(tag, "cloud client connected");
        }
      }
    }

    int readyClient = -1;
    if (clientFd >= 0 && FD_ISSET(clientFd, &readable)) readyClient = clientFd;

    if (FD_ISSET(fd, &readable)) {
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
          cloudClient = false;
          buffered = 0;
          sendFailed = false;
          if (DapCtrl_OnConnect(Tcp_DapSend, NULL) != ESP_OK) {
            close(accepted);
            clientFd = -1;
          } else {
            /* Keep a connected debugger responsive; restore the previous mode on disconnect. */
            if (esp_wifi_get_ps(&savedWifiPs) == ESP_OK && savedWifiPs != WIFI_PS_NONE) {
              restoreWifiPs = esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK;
            }
            ESP_LOGI(tag, "client connected");
          }
        }
      } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        ESP_LOGW(tag, "accept failed: %s", strerror(errno));
      }
    }

    if (readyClient >= 0 && clientFd == readyClient) {
      if (buffered >= sizeof stream) {
        ESP_LOGW(tag, "receive buffer full");
        Tcp_DapDrop(&buffered);
      } else {
        int n = cloudClient ? Cloud_TcpRecv(clientFd, stream + buffered, sizeof stream - buffered)
                            : recv(clientFd, stream + buffered, sizeof stream - buffered, 0);
        if (n < 0 || (!cloudClient && n == 0)) {
          Tcp_DapDrop(&buffered);
        } else if (n > 0) {
          buffered += (size_t)n;
          size_t consumed = 0;
          while (clientFd >= 0 && buffered - consumed >= TCP_HEADER) {
            const uint8_t* p = stream + consumed;
            uint32_t signature = 0;
            uint16_t length = 0;
            memcpy(&signature, p, sizeof signature);
            memcpy(&length, p + 4, sizeof length);
            uint8_t type = p[6];
            if (signature != TCP_SIGNATURE || type != TCP_REQUEST || length > CONFIG_DAP_PACKET_SIZE) {
              ESP_LOGW(tag, "bad DAP frame");
              Tcp_DapDrop(&buffered);
              break;
            }
            if (buffered - consumed < TCP_HEADER + (size_t)length) break;
            DapCtrl_OnData(p + TCP_HEADER, length);
            consumed += TCP_HEADER + (size_t)length;
            if (sendFailed) {
              ESP_LOGW(tag, "response send failed");
              Tcp_DapDrop(&buffered);
              break;
            }
          }
          if (clientFd >= 0) {
            memmove(stream, stream + consumed, buffered - consumed);
            buffered -= consumed;
            /* Coalesce already-buffered replies without waiting for another request. */
            Tcp_DapFlush();
            if (sendFailed) {
              ESP_LOGW(tag, "response send failed");
              Tcp_DapDrop(&buffered);
            }
          }
        }
      }
    }
  }

  Tcp_DapDrop(&buffered);
  Cloud_TcpRelease("dap");
  eventFd = -1;
  if (listenFd >= 0) {
    close(listenFd);
    listenFd = -1;
  } else {
    close(fd);
  }
  Tcp_DapExit(1);
}

bool Tcp_DapIsRunning(void)
{
  return running;
}

esp_err_t Tcp_DapStart(void)
{
  if (running || dapTask != NULL) return ESP_ERR_INVALID_STATE;
  if (stopped == NULL) {
    stopped = xSemaphoreCreateBinary();
    if (stopped == NULL) return ESP_FAIL;
  }
  stopRequested = false;
  starter = xTaskGetCurrentTaskHandle();
  xTaskNotifyStateClear(starter);
  if (xTaskCreatePinnedToCore(Tcp_DapTask, "saihub_dap", 6144, NULL, 5, &dapTask, 0) != pdPASS) {
    dapTask = NULL;
    starter = NULL;
    ESP_LOGE(tag, "task create failed");
    return ESP_FAIL;
  }
  uint32_t result = 0;
  if (xTaskNotifyWait(0, UINT32_MAX, &result, pdMS_TO_TICKS(2000)) != pdTRUE || result == 0) {
    stopRequested = true;
    if (listenFd >= 0) shutdown(listenFd, SHUT_RDWR);
    xSemaphoreTake(stopped, portMAX_DELAY);
    dapTask = NULL;
    starter = NULL;
    return ESP_FAIL;
  }
  starter = NULL;
  return ESP_OK;
}

esp_err_t Tcp_DapStop(void)
{
  if (!running && dapTask == NULL) return ESP_OK;
  stopRequested = true;
  if (listenFd >= 0) shutdown(listenFd, SHUT_RDWR);
  if (eventFd >= 0) {
    uint64_t one = 1;
    (void)write(eventFd, &one, sizeof one);
  }
  if (stopped != NULL) xSemaphoreTake(stopped, portMAX_DELAY);
  dapTask = NULL;
  eventFd = -1;
  stopRequested = false;
  return ESP_OK;
}
