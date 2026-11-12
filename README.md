<h1 align="center">
  <img src="docs/assets/robot.svg" width="46" height="46" alt="SAIHub robot" valign="middle"> SAIHub
</h1>

<p align="center">
  <img src="https://count.getloli.com/@saihub?theme=booru-jaypee" height="80" alt="counter">
</p>

<p align="center"><strong>Give your agent a hand.</strong></p>

<p align="center">
  An MIT-licensed, agent-ready IO board.<br>
  ESP32-C5 firmware that speaks <a href="https://modelcontextprotocol.io/">MCP</a>,
  so Cursor, Claude Code, and Codex can drive real hardware.
</p>

<p align="center">
  <a href="README.zh.md">中文</a>
  · <a href="docs/cookbook.md">Cookbook</a>
  · <a href="docs/hardware.md">Hardware</a>
  · <a href="docs/design-notes.md">Protocol</a>
  · <a href="docs/build.md">Build</a>
  · <a href="https://github.com/xqe2011/saihub/releases">Releases</a>
  · <a href="LICENSE">MIT</a>
</p>

<p align="center">
  Watch the video: <a href="https://youtu.be/VpJexyDZVVk"><img src="docs/assets/youtube.svg" width="16" height="16" alt="" valign="middle"> YouTube</a>
  · <a href="https://www.bilibili.com/video/BV1MVn66EEgq"><img src="docs/assets/bilibili.svg" width="16" height="16" alt="" valign="middle"> Bilibili</a>
</p>

---

## What it is

SAIHub is a pocket IO and debug companion for coding agents. Flash it, join Wi-Fi, paste an MCP URL, and the agent gets a hand on real hardware:

- **Wireless flashing & debugging** — expose CMSIS-DAP (SWD) and RFC2217 (serial-over-TCP) on the LAN or through the cloud relay, so OpenOCD, GDB, and serial flashers can program and step-debug targets like STM32, RP2040, nRF52, Arduino, or ESP32 remotely.
- **Bus debugging & emulation** — probe and drive UART, SPI, and I2C buses directly, or run SPI and I2C in slave mode to capture bus transactions from the target and emulate sensors or peripherals with programmable response maps.
- **GPIO / PWM / trace / programmable power** — toggle pins, generate pulses and PWM up to 50 kHz, capture multi-pin edges like a logic analyzer, and switch fault-protected 3.3 V / 5 V rails on and off, all over MCP.
- **On-device scripting** — the agent can upload Lua scripts and run them on the board itself, so timing-critical or multi-step logic executes locally without a network round-trip per operation.

The board is **SAIHub-Mini**: 48 × 28 mm, ESP32-C5, USB-C, a 12-pin header, fault-latched programmable 3.3 V / 5 V outputs, and an onboard buzzer. Pinout and electrical details: [docs/hardware.md](docs/hardware.md). Firmware, KiCad design, and the relay server all live in this repo.

You do not have to use our board — the firmware runs on any ESP32-C5. See [bring your own board](docs/bring-your-own-board.md) for what changes and where to remap the GPIOs.

## What you can do with it

**Zero-PC firmware development with cloud agents.** You don't even need a computer at the bench. Power SAIHub from a USB charger, wire it to your target board, and connect **Cursor Cloud Agents**, **Claude Code cloud sessions**, or **OpenAI Codex Cloud** through the cloud relay. Running entirely in the cloud, the agent can:

1. **Provision the build & debug environment** in its cloud sandbox — install toolchains, OpenOCD, GDB, or serial flashers, then write and compile the firmware.
2. **Remotely flash and step-debug** the target over tunneled CMSIS-DAP (SWD) or RFC2217 serial-over-TCP.
3. **Automated hardware testing** — power-cycle the 3.3 V / 5 V rails, watch pin signals and PWM timing with `trace_pins`, inspect UART logs, capture SPI/I2C traffic, or **fake SPI/I2C sensors** in slave mode to verify driver behavior against synthetic data.
4. **Deliver tested, runnable firmware** directly in a pull request — verified on real hardware with zero manual probing or local setup.

**Give your local agent a hand on the bench.** Drop SAIHub next to a target board and let Cursor, Claude Code, or Codex CLI drive GPIO, generate PWM, capture waveforms, probe I2C/SPI, and flash over SWD/UART instead of asking you to move jumpers and scope probes by hand.

**An agent-ready IO board.** The simplest way to hang real-world IO off an agent:

- Sweep a **servo** from a Lua script while the agent watches the mechanism.
- Dim a **light**, blink a status LED over PWM, or chime the onboard **buzzer** when a build or test finishes.
- Close an **electromagnetic relay** to switch a pump, a lock, or a heater.
- Power a 3.3 V or 5 V sensor from the switched, fault-protected rails, then read it over I2C, SPI, UART, or GPIO.

## Quick start

The [cookbook](docs/cookbook.md) walks through every step:

1. Open the [SAIHub web flasher](https://xqe2011.github.io/saihub) in Chrome or Edge, pick a release, and flash the board.
2. Click **BOOT** to open Wi-Fi pairing, join the `SAIHUB-xxxxxx` AP, and pick your network.
3. Point Cursor / Claude Code / Codex CLI at `http://<device-ip>/mcp`. The agent now has a hand on your bench.

## Ways to connect

The LAN URL is all you need at the bench. To reach the board from anywhere, put a relay in front of it — our managed cloud, or one you deploy yourself:

| | Local LAN | Self-hosted relay | Managed cloud |
| --- | --- | --- | --- |
| Best for | Bench work, zero setup | Remote access under your own account | Remote access, zero ops |
| MCP URL | `http://<device-ip>/mcp` | `https://<your-worker>/device/<digest>/mcp` | `https://saihub.xqe2011.com/device/<digest>/mcp` |
| Reach | Same LAN only | Anywhere | Anywhere |
| Setup | None after Wi-Fi | Deploy to Cloudflare then rebuild firmware with your origin ([details](docs/cookbook.md#5-self-host-the-cloudflare-relay)) | None — boards bought from us connect to the managed cloud |
| Auth | None (LAN trust) | OAuth through your relay | OAuth in the browser |
| Traffic path | Direct to the board | Through your Cloudflare account | Through our infrastructure |
| Cost | Free | Cloudflare free tier | Included with the board |

After the board authenticates with the relay, the serial log prints its landing page — `https://<origin>/cloud/landing/<digest>/page` — with live online status, copyable MCP or REST/OpenAPI URLs, and a button to mint a authentication token.

The relay protocol is server-agnostic — the Cloudflare Worker in this repo is the reference implementation ([design notes](docs/design-notes.md)).

## No vendor lock-in

Nothing here ties you to us:

- **Any ESP32-C5 board works.** SAIHub-Mini is just one carrier; an off-the-shelf DevKit or your own PCB runs the same firmware. See [bring your own board](docs/bring-your-own-board.md).
- **You don't need our managed cloud.** The relay is a Cloudflare Worker you can deploy under your own account — or replace entirely, since the protocol is documented and server-agnostic.
- **Everything is open source under MIT.** Firmware, KiCad hardware design, and the cloud relay all live in this repo.
- **LAN mode needs no service at all.** Wi-Fi plus the local MCP URL is a complete setup; the cloud is optional convenience, not a requirement.

## Capabilities

| Area | What the agent gets |
| --- | --- |
| GPIO 0–7 | Digital in / out / open-drain, microsecond pulse, PWM up to 50 kHz |
| Trace | Edge capture on one or more pins (up to 60 s), like a pocket logic analyzer |
| SWD debug | CMSIS-DAP over TCP for OpenOCD / GDB on LAN or cloud relay |
| UART | One product UART with RTS / DTR for auto-flashing circuits, plus RFC2217 serial-over-TCP |
| SPI | Master (up to 10 MHz) and Slave — capture, generate, and emulate bus traffic |
| I2C | Master (up to 1 MHz) and Slave, 7 / 10-bit addressing — capture, generate, and emulate bus traffic |
| Power | Switched **3V3** and **5V** rails with overcurrent / fault protection |
| Buzzer | Onboard Morse-code passive buzzer |

Logic pins are **3.3 V only** — full electrical limits in [docs/hardware.md](docs/hardware.md).

## Repo layout

| Path | What |
| --- | --- |
| `main/` | ESP-IDF firmware (ESP32-C5, IDF ≥ 5.5) |
| `mcp.json` | MCP tool schema served by the device |
| `openapi.json` | REST API for the same tools |
| `cloud/cloudflare/` | Cloudflare Worker relay + OAuth (reference server) |
| `hardware/` | SAIHub-Mini KiCad project |
| `docs/` | Cookbook, hardware, build, protocol, and BYO-board docs |

## Community

- [Join our Discord](https://discord.gg/aZerprjxsc)
- QQ group: **499504696**

## License

[MIT](LICENSE).
