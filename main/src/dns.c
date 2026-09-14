/**
 * @name Pairing DNS hijack
 * @file dns.c
 * @author xqe2011
 *
 * Answers every A query with the pairing AP IP so OS captive-portal
 * checks reach this device. AAAA gets an empty NOERROR so clients
 * fall back to A without waiting out a timeout.
 */
#include "dns.h"

#include <errno.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char* tag = "SAIHUB-Dns";

#define DNS_PORT 53
#define DNS_MAX_LEN 512
#define DNS_QR 0x8000
#define DNS_AA 0x0400
#define DNS_TYPE_A 1
#define DNS_TYPE_AAAA 28

static volatile bool started = false;
static volatile int dnsSock = -1;
static TaskHandle_t dnsTask = NULL;

static const uint8_t* Dns_SkipName(const uint8_t* p, const uint8_t* end)
{
  while (p < end) {
    uint8_t len = *p;
    if (len == 0) return p + 1;
    if ((len & 0xC0) == 0xC0) {
      if (p + 2 > end) return NULL;
      return p + 2;
    }
    if (p + 1 + len > end) return NULL;
    p += 1 + len;
  }
  return NULL;
}

static int Dns_BuildReply(const uint8_t* req, int reqLen, uint8_t* reply, int replyMax)
{
  if (reqLen < 12 || reqLen > replyMax) return -1;

  memcpy(reply, req, (size_t)reqLen);
  uint16_t flags = (uint16_t)((reply[2] << 8) | reply[3]);
  if ((flags & 0x7800) != 0) return -1;

  uint16_t qdCount = (uint16_t)((reply[4] << 8) | reply[5]);
  if (qdCount == 0) return -1;

  flags = (uint16_t)((flags & 0x7FFF) | DNS_QR | DNS_AA);
  reply[2] = (uint8_t)(flags >> 8);
  reply[3] = (uint8_t)(flags & 0xFF);
  reply[6] = 0;
  reply[7] = 0;
  reply[8] = 0;
  reply[9] = 0;
  reply[10] = 0;
  reply[11] = 0;

  const uint8_t* qName = reply + 12;
  const uint8_t* qEnd = Dns_SkipName(qName, reply + reqLen);
  if (qEnd == NULL || qEnd + 4 > reply + reqLen) return -1;

  uint16_t qType = (uint16_t)((qEnd[0] << 8) | qEnd[1]);
  if (qType != DNS_TYPE_A && qType != DNS_TYPE_AAAA) return reqLen;

  if (qType == DNS_TYPE_AAAA) return reqLen;

  if (reqLen + 16 > replyMax) return -1;
  uint8_t* ans = reply + reqLen;
  uint16_t namePtr = (uint16_t)(0xC000 | (qName - reply));
  ans[0] = (uint8_t)(namePtr >> 8);
  ans[1] = (uint8_t)(namePtr & 0xFF);
  ans[2] = 0;
  ans[3] = DNS_TYPE_A;
  ans[4] = qEnd[2];
  ans[5] = qEnd[3];
  ans[6] = 0;
  ans[7] = 0;
  ans[8] = 0;
  ans[9] = 30;
  ans[10] = 0;
  ans[11] = 4;
  uint32_t ip = ESP_IP4TOADDR(192, 168, 4, 1);
  memcpy(ans + 12, &ip, 4);
  reply[6] = 0;
  reply[7] = 1;
  return reqLen + 16;
}

static void Dns_Task(void* arg)
{
  (void)arg;
  uint8_t rx[DNS_MAX_LEN];
  uint8_t tx[DNS_MAX_LEN];

  while (started) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
      ESP_LOGW(tag, "DNS socket failed");
      break;
    }
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
      ESP_LOGW(tag, "DNS bind failed");
      close(sock);
      break;
    }
    dnsSock = sock;
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ESP_LOGI(tag, "DNS hijack listening");

    while (started) {
      struct sockaddr_in from = {0};
      socklen_t fromLen = sizeof(from);
      int len = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr*)&from, &fromLen);
      if (!started) break;
      if (len < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) continue;
        ESP_LOGW(tag, "DNS recv failed");
        break;
      }
      int replyLen = Dns_BuildReply(rx, len, tx, (int)sizeof(tx));
      if (replyLen > 0) {
        sendto(sock, tx, replyLen, 0, (struct sockaddr*)&from, fromLen);
      }
    }

    dnsSock = -1;
    shutdown(sock, 0);
    close(sock);
  }

  started = false;
  dnsTask = NULL;
  vTaskDelete(NULL);
}

esp_err_t Dns_Start(void)
{
  if (started) return ESP_OK;
  started = true;
  BaseType_t ok = xTaskCreate(Dns_Task, "wifi-dns", 4096, NULL, 5, &dnsTask);
  if (ok != pdPASS) {
    started = false;
    dnsTask = NULL;
    ESP_LOGW(tag, "DNS task create failed");
    return ESP_FAIL;
  }
  return ESP_OK;
}

void Dns_Stop(void)
{
  if (!started && dnsTask == NULL) return;
  started = false;
  for (int i = 0; i < 50 && dnsTask != NULL; i++) {
    int sock = dnsSock;
    if (sock >= 0) shutdown(sock, SHUT_RDWR);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
