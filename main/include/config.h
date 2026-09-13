/**
 * @file config.h
 * Global product configuration.
 */
#ifndef CONFIG_H__
#define CONFIG_H__

#define CONFIG_NVS_NAMESPACE "saihub"

/* Temporary WiFi seed (first flash only; live creds persist in NVS) */
#define CONFIG_WIFI_SSID "CHANGE_ME"
#define CONFIG_WIFI_PASSWORD "CHANGE_ME"
#define CONFIG_WIFI_RECONNECT_INTERVAL_MS 3000

#define CONFIG_NTP_SERVER "pool.ntp.org"
#define CONFIG_NTP_WAIT_TIMEOUT_MS (60 * 1000)

#define CONFIG_LOCK_TTL_US (30ULL * 1000 * 1000)
#define CONFIG_LOCK_MAX_COUNT 8

#define CONFIG_GPIO_TRACE_MAX_DURATION_US (60ULL * 1000 * 1000)
#define CONFIG_GPIO_TRACE_MAX_EVENTS 1024
#define CONFIG_GPIO_PULSE_MAX_WIDTH_US (1ULL * 1000 * 1000)
#define CONFIG_GPIO_LOGICAL_TO_HW {0, 1, 2, 3, 4, 5, 6, 7}
#define CONFIG_GPIO_POWER_3V3_PIN 8
#define CONFIG_GPIO_POWER_5V_PIN 9

#endif
