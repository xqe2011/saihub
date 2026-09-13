/**
 * @file config.h
 * Global product configuration.
 */
#ifndef CONFIG_H__
#define CONFIG_H__

#include <stdint.h>

#define CONFIG_NVS_NAMESPACE "saihub"

/* Temporary WiFi seed (first flash only; live creds persist in NVS) */
#define CONFIG_WIFI_SSID "CHANGE_ME"
#define CONFIG_WIFI_PASSWORD "CHANGE_ME"
#define CONFIG_WIFI_RECONNECT_INTERVAL_MS 3000

#define CONFIG_HTTP_PORT 80
#define CONFIG_NTP_SERVER "pool.ntp.org"
#define CONFIG_NTP_WAIT_TIMEOUT_MS (60 * 1000)

#define CONFIG_LOCK_TTL_US (30ULL * 1000 * 1000)
#define CONFIG_LOCK_MAX_COUNT 8
#define CONFIG_LOCK_MAX_RESOURCES 8
#define CONFIG_LOCK_ID_LEN 16

#define CONFIG_TRACE_MAX_DURATION_US (60ULL * 1000 * 1000)
#define CONFIG_TRACE_DEFAULT_DURATION_US (1ULL * 1000 * 1000)
#define CONFIG_TRACE_MAX_EVENTS 512
#define CONFIG_PULSE_MAX_WIDTH_US 1000000ULL

#define CONFIG_GPIO_LOGICAL_COUNT 8

#endif
