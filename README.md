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
  · <a href="https://github.com/xqe2011/saihub/releases">Releases</a>
  · <a href="LICENSE">MIT</a>
</p>

---

## What it is

SAIHub is a pocket IO companion for coding agents. Flash it, join Wi-Fi, paste an MCP URL, and the agent gets a hand on real hardware:

- **GPIO / PWM / programmable power** — toggle pins, generate PWM up to 50 kHz, capture edges like a logic analyzer, and switch the 3.3 V / 5 V rails on and off, all over MCP.
- **On-device scripting** — the agent can upload Lua scripts and run them on the board itself, so timing-critical or multi-step logic executes locally without a round-trip per pin.

The board is **SAIHub-Mini**: 48 × 28 mm, ESP32-C5, USB-C, a BOOT button, a 12-pin header, and an external U.FL antenna. Pinout and electrical details: [docs/hardware.md](docs/hardware.md). Firmware, KiCad design, and the relay server all live in this repo.

You do not have to use our board — the firmware runs on any ESP32-C5. See [bring your own board](docs/bring-your-own-board.md) for what changes and where to remap the GPIOs.

## What you can do with it

**Buy an agent a hand.** Drop SAIHub next to a DUT and let the agent debug a real embedded board: drive GPIO, generate PWM, capture traces, and speak UART instead of asking you to probe every line.

**An agent-ready IO board.** The simplest way to hang other IO off an agent:

- Sweep a **servo** from a Lua script while the agent watches the mechanism.
- Dim a **light** or blink a status LED over PWM.
- Close an **electromagnetic relay** to switch a pump, a lock, or a heater.
- Power a 3.3 V sensor from the switched rail, then read it over UART or GPIO.

## Quick start

The [cookbook](docs/cookbook.md) walks through every step:

1. Download the firmware `.bin` from [Releases](https://github.com/xqe2011/saihub/releases) and flash it with the [ESP web flasher](https://espressif.github.io/esptool-js/).
2. Click **BOOT** to open Wi-Fi pairing, join the `SAIHUB-xxxxxx` AP, and pick your network.
3. Point Cursor / Claude Code / Codex CLI at `http://<device-ip>/mcp`. The agent now has a hand on your bench.

## Ways to connect

The LAN URL is all you need at the bench. To reach the board from anywhere, put a relay in front of it — our managed cloud, or one you deploy yourself:

| | Local LAN | Self-hosted relay | Managed cloud |
| --- | --- | --- | --- |
| Best for | Bench work, zero setup | Remote access under your own account | Remote access, zero ops |
| MCP URL | `http://<device-ip>/mcp` | `https://<your-worker>/device/<digest>/mcp` | `https://<managed-cloud-origin>/device/<digest>/mcp` |
| Reach | Same LAN only | Anywhere | Anywhere |
| Setup | None after Wi-Fi | Deploy the [worker](docs/cookbook.md#5-self-host-the-cloudflare-relay), rebuild firmware with your origin | None — boards bought from us connect to the managed cloud |
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
| GPIO 0–7 | Digital in / out / open-drain, pulse, PWM up to 50 kHz |
| Trace | Edge capture on one or more pins (up to 60 s) |
| UART | One product UART, UTF-8 or raw bytes |
| Power | Switched **3V3** and **5V** rails (off by default) |
| Scripts | Sandboxed Lua 5.4 on-device |
| Access | Streamable HTTP MCP on port 80, REST + OpenAPI, optional cloud relay |

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

## License

[MIT](LICENSE).
