/**
 * @name WIFI module
 * @file wifi.c
 * @author xqe2011
 */
#include "wifi.h"

#include "config.h"
#include "dns.h"
#include "nvs.h"
#include "tool.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <lwip/ip4_addr.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* tag = "SAIHub-Wifi";

#define WIFI_PAIR_AP_CLOSE_DELAY_MS 5000
#define WIFI_SCAN_MAX_AP 32

static bool allowReconnect = true;
static bool isConnected = false;
static bool isPairing = false;
static bool portalConnectInFlight = false;
static bool ignorePairingStaLeave = false;
static Wifi_PairState pairState = WIFI_PAIR_STATE_IDLE;
static char pairIp[16] = {0};
static char pairReason[96] = {0};
static char lastRequestConnectSSID[33] = {0};
static char lastRequestConnectPassword[65] = {0};
static char pairingApSsid[32] = {0};
static Wifi_ConnectedCallback connectedCallbacks[4];
static Wifi_DisconnectedCallback disconnectedCallbacks[4];
static Wifi_PairingStartedCallback pairingStartedCallbacks[4];
static Wifi_PairingStoppedCallback pairingStoppedCallbacks[4];
static TimerHandle_t reconnectTimer;
static TimerHandle_t pairCloseTimer;
static esp_netif_t* staNetif = NULL;
static esp_netif_t* apNetif = NULL;
static const char portalDhcpUri[] = CONFIG_WIFI_PORTAL_URL;

bool Wifi_IsConnected(void)
{
  return isConnected;
}

bool Wifi_IsPairing(void)
{
  return isPairing;
}

esp_err_t Wifi_RegisterConnectedCallback(Wifi_ConnectedCallback callback)
{
  TOOL_REGISTER_CALLBACK(connectedCallbacks, callback, "connected");
}

esp_err_t Wifi_RegisterDisconnectedCallback(Wifi_DisconnectedCallback callback)
{
  TOOL_REGISTER_CALLBACK(disconnectedCallbacks, callback, "disconnected");
}

esp_err_t Wifi_RegisterPairingStartedCallback(Wifi_PairingStartedCallback callback)
{
  TOOL_REGISTER_CALLBACK(pairingStartedCallbacks, callback, "pairing-started");
}

esp_err_t Wifi_RegisterPairingStoppedCallback(Wifi_PairingStoppedCallback callback)
{
  TOOL_REGISTER_CALLBACK(pairingStoppedCallbacks, callback, "pairing-stopped");
}

Wifi_PairState Wifi_GetPairStatus(char* ipOut, size_t ipLen, char* reasonOut, size_t reasonLen)
{
  if (ipOut != NULL && ipLen > 0) {
    strncpy(ipOut, pairIp, ipLen - 1);
    ipOut[ipLen - 1] = '\0';
  }
  if (reasonOut != NULL && reasonLen > 0) {
    strncpy(reasonOut, pairReason, reasonLen - 1);
    reasonOut[reasonLen - 1] = '\0';
  }
  return pairState;
}

static void Wifi_BuildApSsid(void)
{
  uint8_t mac[6] = {0};
  if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) {
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
  }
  snprintf(pairingApSsid, sizeof(pairingApSsid), "SAIHub-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

static esp_err_t Wifi_ApplyStaConfig(const char* ssid, const char* password)
{
  if (ssid == NULL || password == NULL || strlen(ssid) == 0 || strlen(ssid) > 32 || strlen(password) > 64) {
    ESP_LOGE(tag, "SSID or password invalid length");
    return ESP_FAIL;
  }

  wifi_config_t config = {0};
  memcpy(config.sta.ssid, ssid, strlen(ssid));
  memcpy(config.sta.password, password, strlen(password));
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(esp_wifi_set_config(WIFI_IF_STA, &config), "set wifi config failed");

  strncpy(lastRequestConnectSSID, ssid, sizeof(lastRequestConnectSSID) - 1);
  lastRequestConnectSSID[sizeof(lastRequestConnectSSID) - 1] = '\0';
  strncpy(lastRequestConnectPassword, password, sizeof(lastRequestConnectPassword) - 1);
  lastRequestConnectPassword[sizeof(lastRequestConnectPassword) - 1] = '\0';
  return ESP_OK;
}

esp_err_t Wifi_ConnectWifi(const char* ssid, const char* password)
{
  TOOL_CHECK_ESP_OK_OR_RETURN(Wifi_ApplyStaConfig(ssid, password));
  allowReconnect = true;
  portalConnectInFlight = false;
  ignorePairingStaLeave = false;
  esp_wifi_disconnect();
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(esp_wifi_connect(), "wifi connect request failed");
  ESP_LOGI(tag, "Connecting to SSID: %s", ssid);
  return ESP_OK;
}

esp_err_t Wifi_ConnectWifiAsync(const char* ssid, const char* password)
{
  if (!isPairing) {
    ESP_LOGW(tag, "async connect rejected; not pairing");
    return ESP_ERR_INVALID_STATE;
  }
  if (Wifi_ApplyStaConfig(ssid, password) != ESP_OK) {
    pairState = WIFI_PAIR_STATE_FAILED;
    snprintf(pairReason, sizeof(pairReason), "SSID or password is invalid.");
    return ESP_ERR_INVALID_ARG;
  }

  pairState = WIFI_PAIR_STATE_CONNECTING;
  pairIp[0] = '\0';
  pairReason[0] = '\0';
  portalConnectInFlight = true;
  allowReconnect = false;
  xTimerStop(reconnectTimer, 0);

  /* Leaving the current STA AP posts DISCONNECTED; that is not a pairing failure. */
  ignorePairingStaLeave = isConnected;
  esp_wifi_disconnect();
  esp_err_t err = esp_wifi_connect();
  if (err != ESP_OK) {
    ignorePairingStaLeave = false;
    portalConnectInFlight = false;
    pairState = WIFI_PAIR_STATE_FAILED;
    snprintf(pairReason, sizeof(pairReason), "Could not start connection.");
    return err;
  }
  ESP_LOGI(tag, "Async connecting to SSID: %s", ssid);
  return ESP_OK;
}

esp_err_t Wifi_ScanNetworks(Wifi_Network* out, size_t maxOut, size_t* countOut)
{
  if (out == NULL || countOut == NULL || maxOut == 0) return ESP_ERR_INVALID_ARG;
  *countOut = 0;

  wifi_scan_config_t scanConfig = {
      .ssid = NULL,
      .bssid = NULL,
      .channel = 0,
      .show_hidden = false,
      .scan_type = WIFI_SCAN_TYPE_ACTIVE,
      .scan_time.active.min = 100,
      .scan_time.active.max = 300,
  };
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(esp_wifi_scan_start(&scanConfig, true), "wifi scan failed");

  uint16_t apCount = 0;
  TOOL_CHECK_ESP_OK_OR_RETURN(esp_wifi_scan_get_ap_num(&apCount));
  if (apCount == 0) return ESP_OK;

  uint16_t fetch = apCount > WIFI_SCAN_MAX_AP ? WIFI_SCAN_MAX_AP : apCount;
  wifi_ap_record_t* records = calloc(fetch, sizeof(wifi_ap_record_t));
  if (records == NULL) return ESP_ERR_NO_MEM;
  esp_err_t err = esp_wifi_scan_get_ap_records(&fetch, records);
  if (err != ESP_OK) {
    free(records);
    return err;
  }

  size_t n = 0;
  for (uint16_t i = 0; i < fetch && n < maxOut; i++) {
    if (records[i].ssid[0] == '\0') continue;
    bool dup = false;
    for (size_t j = 0; j < n; j++) {
      if (strcmp(out[j].ssid, (const char*)records[i].ssid) == 0) {
        if (records[i].rssi > out[j].rssi) out[j].rssi = records[i].rssi;
        dup = true;
        break;
      }
    }
    if (dup) continue;
    strncpy(out[n].ssid, (const char*)records[i].ssid, sizeof(out[n].ssid) - 1);
    out[n].ssid[sizeof(out[n].ssid) - 1] = '\0';
    out[n].rssi = records[i].rssi;
    out[n].secure = records[i].authmode != WIFI_AUTH_OPEN;
    n++;
  }
  free(records);
  *countOut = n;
  return ESP_OK;
}

static void Wifi_ApplyPairingDhcp(void)
{
  if (apNetif == NULL) apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (apNetif == NULL) {
    ESP_LOGW(tag, "AP netif missing; captive DHCP not set");
    return;
  }

  esp_err_t err = esp_netif_dhcps_stop(apNetif);
  if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
    ESP_LOGW(tag, "dhcps stop failed %s", esp_err_to_name(err));
  }

  esp_netif_dns_info_t dns = {0};
  dns.ip.type = ESP_IPADDR_TYPE_V4;
  dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(192, 168, 4, 1);
  err = esp_netif_set_dns_info(apNetif, ESP_NETIF_DNS_MAIN, &dns);
  if (err != ESP_OK) ESP_LOGW(tag, "set DNS info failed %s", esp_err_to_name(err));

  uint8_t offerDns = 1;
  err = esp_netif_dhcps_option(apNetif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offerDns, sizeof(offerDns));
  if (err != ESP_OK) ESP_LOGW(tag, "DHCP DNS option failed %s", esp_err_to_name(err));

  err = esp_netif_dhcps_option(apNetif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, (void*)portalDhcpUri,
                               strlen(portalDhcpUri));
  if (err != ESP_OK) ESP_LOGW(tag, "DHCP portal URI failed %s", esp_err_to_name(err));

  err = esp_netif_dhcps_start(apNetif);
  if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
    ESP_LOGW(tag, "dhcps start failed %s", esp_err_to_name(err));
  }
}

static void Wifi_FinishPairingStop(void)
{
  if (!isPairing) return;
  bool fireConnected = isConnected && pairState == WIFI_PAIR_STATE_CONNECTED;
  isPairing = false;
  portalConnectInFlight = false;
  ignorePairingStaLeave = false;
  xTimerStop(pairCloseTimer, 0);
  Dns_Stop();

  esp_wifi_set_mode(WIFI_MODE_STA);

  TOOL_EXECUTE_CALLBACKS(pairingStoppedCallbacks);
  if (fireConnected) {
    TOOL_EXECUTE_CALLBACKS(connectedCallbacks);
  }
}

static void Wifi_PairCloseCallback(TimerHandle_t timer)
{
  (void)timer;
  ESP_LOGI(tag, "Closing pairing AP after successful connect");
  Wifi_FinishPairingStop();
}

esp_err_t Wifi_StartPairing(void)
{
  if (isPairing) return ESP_OK;

  Wifi_BuildApSsid();
  wifi_config_t apConfig = {0};
  strncpy((char*)apConfig.ap.ssid, pairingApSsid, sizeof(apConfig.ap.ssid) - 1);
  apConfig.ap.ssid_len = strlen(pairingApSsid);
  apConfig.ap.channel = 1;
  apConfig.ap.authmode = WIFI_AUTH_OPEN;
  apConfig.ap.max_connection = 4;
  apConfig.ap.ssid_hidden = 0;

  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(esp_wifi_set_mode(WIFI_MODE_APSTA), "set APSTA failed");
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(esp_wifi_set_config(WIFI_IF_AP, &apConfig), "set AP config failed");

  Wifi_ApplyPairingDhcp();
  if (Dns_Start() != ESP_OK) {
    ESP_LOGW(tag, "DNS hijack failed; captive prompt may not appear");
  }

  isPairing = true;
  pairState = WIFI_PAIR_STATE_IDLE;
  pairIp[0] = '\0';
  pairReason[0] = '\0';
  portalConnectInFlight = false;
  ignorePairingStaLeave = false;
  xTimerStop(pairCloseTimer, 0);

  ESP_LOGI(tag, "Pairing AP started: %s", pairingApSsid);
  TOOL_EXECUTE_CALLBACKS(pairingStartedCallbacks);
  return ESP_OK;
}

esp_err_t Wifi_StopPairing(void)
{
  if (!isPairing) return ESP_OK;
  Wifi_FinishPairingStop();
  allowReconnect = true;
  return ESP_OK;
}

static void Wifi_ReconnectCallback(TimerHandle_t timer)
{
  (void)timer;
  if (allowReconnect && !portalConnectInFlight) {
    esp_wifi_connect();
  }
}

static void Wifi_OnGotIp(void)
{
  Nvs_SetString("wifi.ssid", lastRequestConnectSSID);
  Nvs_SetString("wifi.password", lastRequestConnectPassword);
  isConnected = true;

  esp_netif_ip_info_t ipInfo;
  if (staNetif != NULL && esp_netif_get_ip_info(staNetif, &ipInfo) == ESP_OK) {
    snprintf(pairIp, sizeof(pairIp), IPSTR, IP2STR(&ipInfo.ip));
  } else {
    pairIp[0] = '\0';
  }

  ESP_LOGI(tag, "WiFi connected ip=%s", pairIp[0] ? pairIp : "?");

  if (isPairing) {
    pairState = WIFI_PAIR_STATE_CONNECTED;
    pairReason[0] = '\0';
    portalConnectInFlight = false;
    ignorePairingStaLeave = false;
    allowReconnect = true;
    xTimerStop(pairCloseTimer, 0);
    xTimerStart(pairCloseTimer, portMAX_DELAY);
    /* Defer connected callbacks until AP/portal close so port 80 is free. */
    return;
  }

  TOOL_EXECUTE_CALLBACKS(connectedCallbacks);
}

static void Wifi_EventHandler(void* arg, esp_event_base_t eventBase, int32_t eventId, void* eventData)
{
  (void)arg;

  if (eventBase == WIFI_EVENT) {
    if (eventId == WIFI_EVENT_STA_START) {
      char ssid[33] = {0};
      char password[65] = {0};
      if (Nvs_GetString("wifi.ssid", ssid, sizeof(ssid)) == ESP_OK &&
          Nvs_GetString("wifi.password", password, sizeof(password)) == ESP_OK) {
        Wifi_ConnectWifi(ssid, password);
      } else if (strcmp(CONFIG_WIFI_SSID, "CHANGE_ME") != 0) {
        Wifi_ConnectWifi(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD);
      } else {
        ESP_LOGW(tag, "No WiFi credentials in NVS or config.h");
      }
    } else if (eventId == WIFI_EVENT_STA_DISCONNECTED) {
      wifi_event_sta_disconnected_t* disc = (wifi_event_sta_disconnected_t*)eventData;
      bool wasConnected = isConnected;
      isConnected = false;
      if (wasConnected) {
        TOOL_EXECUTE_CALLBACKS(disconnectedCallbacks);
      }

      if (portalConnectInFlight) {
        uint8_t reason = disc ? disc->reason : 0;
        if (ignorePairingStaLeave) {
          ignorePairingStaLeave = false;
          ESP_LOGI(tag, "Left previous AP; pairing connect continues reason=%u", (unsigned)reason);
          return;
        }
        portalConnectInFlight = false;
        pairState = WIFI_PAIR_STATE_FAILED;
        if (reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
            reason == WIFI_REASON_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_NO_AP_FOUND) {
          snprintf(pairReason, sizeof(pairReason), "Could not join that network. Check the password and try again.");
        } else {
          snprintf(pairReason, sizeof(pairReason), "Connection failed. Try again.");
        }
        ESP_LOGW(tag, "Pairing connect failed reason=%u", (unsigned)reason);
        return;
      }

      if (allowReconnect) {
        xTimerStop(reconnectTimer, portMAX_DELAY);
        xTimerStart(reconnectTimer, portMAX_DELAY);
      }
    }
  }

  if (eventBase == IP_EVENT && eventId == IP_EVENT_STA_GOT_IP) {
    Wifi_OnGotIp();
  }
}

esp_err_t Wifi_Init(void)
{
  reconnectTimer = xTimerCreate("wifi-reconn", pdMS_TO_TICKS(CONFIG_WIFI_RECONNECT_INTERVAL_MS), pdFALSE, NULL,
                                  Wifi_ReconnectCallback);
  TOOL_CHECK_OR_LOG_RETURN(reconnectTimer == NULL, "create reconnect timer failed");

  pairCloseTimer = xTimerCreate("wifi-pair-close", pdMS_TO_TICKS(WIFI_PAIR_AP_CLOSE_DELAY_MS), pdFALSE, NULL,
                                Wifi_PairCloseCallback);
  TOOL_CHECK_OR_LOG_RETURN(pairCloseTimer == NULL, "create pair close timer failed");

  TOOL_CHECK_ESP_OK_OR_RETURN(esp_netif_init());
  TOOL_CHECK_ESP_OK_OR_RETURN(esp_event_loop_create_default());
  staNetif = esp_netif_create_default_wifi_sta();
  apNetif = esp_netif_create_default_wifi_ap();
  TOOL_CHECK_OR_LOG_RETURN(apNetif == NULL || staNetif == NULL, "create wifi netif failed");

  TOOL_CHECK_ESP_OK_OR_RETURN(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &Wifi_EventHandler, NULL));
  TOOL_CHECK_ESP_OK_OR_RETURN(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &Wifi_EventHandler, NULL));

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  TOOL_CHECK_ESP_OK_OR_RETURN(esp_wifi_init(&cfg));
  TOOL_CHECK_ESP_OK_OR_RETURN(esp_wifi_set_mode(WIFI_MODE_STA));
  TOOL_CHECK_ESP_OK_OR_RETURN(esp_wifi_start());
  return ESP_OK;
}
