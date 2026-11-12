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
#define CONFIG_WIFI_PORTAL_URL "http://192.168.4.1/wifi/page"
#define CONFIG_WIFI_PAIRING_BUZZER_SEQUENCE "."

#define CONFIG_NTP_SERVER "pool.ntp.org"
#define CONFIG_NTP_TIMEOUT_MS (15 * 1000)

/* Set to the deployed worker origin, e.g. wss://saihub-cloud.example.workers.dev. */
#define CONFIG_CLOUD_URL "wss://saihub.xqe2011.com"
#define CONFIG_CLOUD_MAX_MESSAGE_BYTES (72 * 1024)
/* Cloud TCP stream pool. Ring is allocated per connection; initial byteCredits match the ring size. */
#define CONFIG_CLOUD_TCP_MAX_CONNECTIONS 4
#define CONFIG_CLOUD_TCP_STREAM_RX_BYTES (4 * 1024)
#define CONFIG_CLOUD_GRANT_SECRET_LEN 32
#define CONFIG_CLOUD_GRANT_NAME_MAX 32
#define CONFIG_CLOUD_GRANT_MAX 16
#define CONFIG_CLOUD_PAIRING_TTL_S 30
#define CONFIG_CLOUD_PAIRING_BUZZER_SEQUENCE "."

#define CONFIG_LOCK_TTL_US (30ULL * 1000 * 1000)
#define CONFIG_LOCK_MAX_COUNT 16
#define CONFIG_LOCK_MAX_RESOURCES 16
#define CONFIG_LOCK_MAX_PERIPHERAL 8

/* Must fit in GpioCtrl_TraceEvent.deltaUs (27 bits, max ~134s). */
#define CONFIG_GPIO_TRACE_MAX_DURATION_US (60ULL * 1000 * 1000)
#define CONFIG_GPIO_TRACE_MAX_EVENTS 1024
#define CONFIG_GPIO_PULSE_MAX_WIDTH_US (1ULL * 1000 * 1000)
#define CONFIG_GPIO_PWM_MAX_OUTPUTS 4
#define CONFIG_GPIO_PWM_MAX_FREQ_HZ 50000U
#define CONFIG_GPIO_PWM_DEFAULT_FREQ_HZ 1000U
#define CONFIG_GPIO_LOGICAL_TO_HW {10, 1, 0, 2, 4, 5, 6, 7}
#define CONFIG_GPIO_POWER_3V3_PIN 8
#define CONFIG_GPIO_POWER_5V_PIN 9
#define CONFIG_GPIO_POWER_3V3_FAULT_PIN 15
#define CONFIG_GPIO_POWER_5V_FAULT_PIN 3

#define CONFIG_BUTTON_PIN 28
#define CONFIG_BUTTON_CLOUD_PAIRING_HOLD_US (3LL * 1000 * 1000)

#define CONFIG_LED_PIN 26
#define CONFIG_LED_FAST_MS 100
#define CONFIG_LED_SLOW_MS 500
#define CONFIG_LED_PAUSE_MS 1000

#define CONFIG_BUZZER_PIN 12
#define CONFIG_BUZZER_FREQ_HZ 4000U
#define CONFIG_BUZZER_DUTY_PERCENT 50U
#define CONFIG_BUZZER_WPM 10U
#define CONFIG_BUZZER_MAX_SEQUENCE 64

/* Product UART id -> hardware UART port number (controller 0 reserved for console). */
#define CONFIG_UART_ID_TO_NUM {1}
#define CONFIG_UART_RX_BUF_BYTES 4096
#define CONFIG_UART_MAX_PAYLOAD_BYTES 2048

/* RFC2217 serial-over-TCP. One listener; the UART id is chosen when mode is rfc2217. */
#define CONFIG_RFC2217_TCP_PORT 4442
#define CONFIG_RFC2217_LOCAL_STATUS_PROMPT \
  "RFC2217 serial-over-TCP is running. Connect a compatible client using the ip and port from this status."
#define CONFIG_RFC2217_CLOUD_STATUS_PROMPT \
  "RFC2217 serial-over-TCP is running. Run this Node.js bridge (npm i ws), then point an RFC2217 client at 127.0.0.1:4442. Replace status.url with the url from this status.\n" \
  "import { createServer } from 'node:net';\n" \
  "import WebSocket from 'ws';\n" \
  "createServer((tcp) => {\n" \
  "  const ws = new WebSocket(status.url);\n" \
  "  const ping = setInterval(() => { if (ws.readyState === WebSocket.OPEN) ws.send('ping'); }, 10000);\n" \
  "  const stop = () => { clearInterval(ping); tcp.destroy(); ws.close(); };\n" \
  "  tcp.pause();\n" \
  "  tcp.on('error', stop); tcp.on('close', stop); ws.on('error', stop); ws.on('close', stop);\n" \
  "  ws.on('open', () => { tcp.resume(); tcp.on('data', (buf) => ws.send(buf)); });\n" \
  "  ws.on('message', (data, isBinary) => { if (isBinary) tcp.write(data); });\n" \
  "}).listen(4442, '127.0.0.1');\n"

/* Product SPI id -> hardware host number. Id 0 is host 2. */
#define CONFIG_SPI_ID_TO_HOST {1}
/* Variable-length slave events. The oldest events are dropped when this buffer is full. */
#define CONFIG_SPI_SLAVE_EVENT_BYTES 4096
#define CONFIG_SPI_SLAVE_MAP_BYTES 2048
/* Maximum transfer length in bytes: 0-255. */
#define CONFIG_SPI_MAX_TRANSFER_BYTES 255 
#define CONFIG_SPI_CLOCK_HZ_MIN 1
#define CONFIG_SPI_CLOCK_HZ_MAX 10000000

/* Product I2C id -> hardware port number. Id 0 is port 0. */
#define CONFIG_I2C_ID_TO_PORT {0}
#define CONFIG_I2C_SLAVE_EVENT_BYTES 4096
#define CONFIG_I2C_SLAVE_MAP_BYTES 2048
#define CONFIG_I2C_MAX_TRANSFER_BYTES 255
#define CONFIG_I2C_CLOCK_HZ_MIN 10000
#define CONFIG_I2C_CLOCK_HZ_MAX 1000000
#define CONFIG_I2C_TRANSFER_TIMEOUT_MS_MIN 1
#define CONFIG_I2C_TRANSFER_TIMEOUT_MS_MAX 60000
#define CONFIG_I2C_SCL_WAIT_US_MAX 1000000

#define CONFIG_SCRIPT_MAX_CALLS 256
#define CONFIG_SCRIPT_MAX_TIMEOUT_US (10ULL * 1000 * 1000)
#define CONFIG_SCRIPT_MAX_SOURCE_BYTES 8192
#define CONFIG_SCRIPT_LUA_HEAP_BYTES (32 * 1024)
#define CONFIG_SCRIPT_STACK_BYTES 8192
#define CONFIG_SCRIPT_WAITER_STACK_BYTES 4096
#define CONFIG_SCRIPT_MAX_OUTPUT_BYTES 4096

/* OTA: latest-release manifest (app images only). Target must match CI image target. */
#define CONFIG_OTA_MANIFEST_URL "https://github.com/xqe2011/saihub/releases/latest/download/manifest.json"
/* On manifest fetch timeout, retry with this prefix (same as the flash page gh-proxy). */
#define CONFIG_OTA_GH_PROXY_PREFIX "https://v4.gh-proxy.org/"
#define CONFIG_OTA_IMAGE_TARGET "esp32c5"
#define CONFIG_OTA_HTTP_METADATA_TIMEOUT_MS 5000
#define CONFIG_OTA_HTTP_IMAGE_TIMEOUT_MS 60000
#define CONFIG_OTA_MANIFEST_MAX_BYTES (8 * 1024)
#define CONFIG_OTA_URL_MAX_BYTES 512

/* CMSIS-DAP over TCP. Logical pins follow CONFIG_GPIO_LOGICAL_TO_HW. nRESET -1 leaves reset unwired. */
#define CONFIG_DAP_TCP_PORT 4441
#define CONFIG_DAP_PACKET_SIZE 1024
#define CONFIG_DAP_PACKET_COUNT 8
#define CONFIG_DAP_SWCLK_LOGICAL 0
#define CONFIG_DAP_SWDIO_LOGICAL 1
#define CONFIG_DAP_NRESET_LOGICAL -1
#define CONFIG_DAP_DEFAULT_CLOCK_HZ 1000000
#define CONFIG_DAP_LOCAL_STATUS_PROMPT \
  "CMSIS-DAP over TCP Service is running. If you can use OpenOCD, connect with cmsis-dap TCP backend using the ip and port from this status."
#define CONFIG_DAP_CLOUD_STATUS_PROMPT \
  "CMSIS-DAP over TCP is running. Run this Node.js bridge (npm i ws), then point OpenOCD (cmsis-dap TCP) or any DAP client at 127.0.0.1:4441. Replace status.url with the url from this status.\n" \
  "import { createServer } from 'node:net';\n" \
  "import WebSocket from 'ws';\n" \
  "createServer((tcp) => {\n" \
  "  const ws = new WebSocket(status.url);\n" \
  "  const ping = setInterval(() => { if (ws.readyState === WebSocket.OPEN) ws.send('ping'); }, 10000);\n" \
  "  const stop = () => { clearInterval(ping); tcp.destroy(); ws.close(); };\n" \
  "  tcp.pause();\n" \
  "  tcp.on('error', stop); tcp.on('close', stop); ws.on('error', stop); ws.on('close', stop);\n" \
  "  ws.on('open', () => { tcp.resume(); tcp.on('data', (buf) => ws.send(buf)); });\n" \
  "  ws.on('message', (data, isBinary) => { if (isBinary) tcp.write(data); });\n" \
  "}).listen(4441, '127.0.0.1');\n"


#endif
