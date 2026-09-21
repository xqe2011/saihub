# Bring your own board

The firmware is not tied to SAIHub-Mini. Any ESP32-C5 board runs it — an Espressif devkit or your own carrier board — as long as it has Wi-Fi, an antenna, and enough free GPIO. Everything the agent talks to (MCP, REST, the web UI, Lua) lives in the chip.

[中文](bring-your-own-board.zh.md) · [README](../README.md) · [Hardware](hardware.md)

## What works on any ESP32-C5

- MCP server (Streamable HTTP, port 80), REST + OpenAPI, Wi-Fi pairing portal, and the web control UI
- GPIO: digital in / out / open-drain, pulse, PWM (4 channels, up to 50 kHz), edge trace (up to 60 s / 1024 events)
- The product UART — UART1 routes through the GPIO matrix, so TX / RX can sit on any pins you map
- Sandboxed Lua 5.4 scripts and resource locks
- Cloud relay: the ECDSA identity key is provisioned into each chip's eFuse at first boot, so the relay works from any board — point `CONFIG_CLOUD_URL` at a worker you self-host (the managed cloud comes with boards bought from us)

## What you lose without the SAIHub-Mini hardware

| Function | Why | Options |
| --- | --- | --- |
| Switched 3V3 / 5V rails (`set_output_power_state`) | Needs the TPS2553 load-switch circuit | Remap the enable pins to your own load switch, or ignore the power tools |
| ~1 A output current limit, fused 3 A input | Carrier-board protection parts | Design your own supply path; don't hang heavy loads off devkit rails |
| BOOT button UX (click → Wi-Fi pairing) | Button wired to `CONFIG_BUTTON_PIN` | Many C5 devkits wire BOOT to GPIO28 as well — check your schematic. Otherwise remap the pin, or bake credentials into `CONFIG_WIFI_SSID` / `CONFIG_WIFI_PASSWORD`: they are used whenever NVS has none saved, and portal-paired credentials persist in NVS |
| Buzzer feedback | 5020 buzzer on GPIO12 | Remap or ignore |
| Managed cloud | A service included with boards bought from us | Self-host the worker ([cookbook §5](cookbook.md#5-self-host-the-cloudflare-relay)) |

## Where to update the GPIO map

All board-specific pins live in `main/include/config.h`:

```c
#define CONFIG_GPIO_LOGICAL_TO_HW {10, 1, 0, 23, 4, 5, 6, 24} /* IO0…IO7 → chip GPIOs */
#define CONFIG_BUTTON_PIN 28            /* BOOT button; click = Wi-Fi pairing */
#define CONFIG_GPIO_POWER_3V3_PIN 8     /* 3V3_SW load-switch enable */
#define CONFIG_GPIO_POWER_5V_PIN 9      /* 5V_SW load-switch enable */
#define CONFIG_BUZZER_PIN 12
#define CONFIG_CLOUD_URL "wss://…"      /* relay origin; empty disables the relay */
#define CONFIG_WIFI_SSID "CHANGE_ME"    /* optional Wi-Fi seed, see above */
#define CONFIG_WIFI_PASSWORD "CHANGE_ME"
```

`CONFIG_GPIO_LOGICAL_TO_HW` is the map agents see: entry *N* is the logical pin `IO<N>` used by `list_pins`, `set_pin_levels`, and friends; the value is the physical chip GPIO. The array length decides how many pins are exposed.

The prebuilt `.bin` in [Releases](https://github.com/xqe2011/saihub/releases) is built for the SAIHub-Mini map and a 4 MB flash layout. For your own board, rebuild:

```bash
idf.py set-target esp32c5
idf.py build
idf.py merge-bin   # merged image for the web flasher
```

### Picking pins

- 3.3 V logic only.
- Avoid strapping pins **GPIO2 / GPIO7 / GPIO28** for anything that drives a level at reset — input-only use is fine (that is why the BOOT button sits on GPIO28).
- Avoid **GPIO13 / GPIO14** (USB D- / D+) if you use native USB Serial/JTAG.
- Avoid the console UART0 pins and any pins your module hides for flash / PSRAM — check the module datasheet and your `sdkconfig`.
- Everything else is fair game: UART1, PWM, and trace all route through the GPIO matrix.
