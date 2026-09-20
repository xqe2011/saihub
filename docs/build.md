# Build guide

Developer reference for the firmware and the Cloudflare Worker relay. End-user setup lives in the [cookbook](cookbook.md); the relay wire protocol is documented in the [design notes](design-notes.md).

[中文](build.zh.md) · [README](../README.md)

## Firmware

ESP-IDF ≥ 5.5, target ESP32-C5:

```bash
idf.py set-target esp32c5
idf.py build
idf.py merge-bin   # merged image for the web flasher (flash at 0x0)
```

Separate images, if not using a merged binary: bootloader `0x2000`, partition table `0x8000`, app `0x10000`.

Board-specific configuration is concentrated in `main/include/config.h`:

| Define | Purpose |
| --- | --- |
| `CONFIG_GPIO_LOGICAL_TO_HW` | Logical IO0–IO7 → chip GPIO map; array length = number of exposed pins |
| `CONFIG_BUTTON_PIN` | BOOT button (click = Wi-Fi pairing) |
| `CONFIG_GPIO_POWER_3V3_PIN` / `CONFIG_GPIO_POWER_5V_PIN` | Load-switch enables for the 3V3 / 5V rails |
| `CONFIG_BUZZER_PIN` | Buzzer |
| `CONFIG_CLOUD_URL` | Relay origin, `ws://` or `wss://` without a path; empty disables the relay. Self-hosting requires changing it and rebuilding |
| `CONFIG_WIFI_SSID` / `CONFIG_WIFI_PASSWORD` | Wi-Fi seed credentials, used whenever NVS has none saved |

Building for a board other than Saihub-Mini: [bring your own board](bring-your-own-board.md).

## Cloudflare Worker (reference relay)

Requires [Bun](https://bun.sh) (or Node) and [Wrangler](https://developers.cloudflare.com/workers/wrangler/). Production deploy steps: [cookbook §5](cookbook.md#5-self-host-the-cloudflare-relay).

```bash
cd cloud/cloudflare
bun install
bun run dev            # wrangler dev → http://127.0.0.1:8787
```

For a dev loop, point the firmware's `CONFIG_CLOUD_URL` at your machine over the LAN, e.g. `ws://<your-lan-ip>:8787`. `.dev.vars` supplies a local `ROUTING_TOKEN_SECRET` to `wrangler dev` only; production keeps it in `wrangler secret put ROUTING_TOKEN_SECRET` — never commit a real secret.

`wrangler.jsonc` binds the `DEVICE` Durable Object (SQLite class) and defaults `AUTH_TIMEOUT_MS` / `REQUEST_TIMEOUT_MS` (10 s / 55 s). `wrangler.test.jsonc` deploys a separate `saihub-cloud-test` worker with 2 s timeouts for integration tests.

### Checks and tests

```bash
bun run typecheck       # tsc --noEmit
bun run test            # tests/cloud/cloudflare/*.test.ts (client flow, headroom)
bun run test:hardware   # tests/hardware/smoke.ts
```

### Local fake device

```bash
bun run fake-device     # defaults to http://127.0.0.1:8787; pass another origin as an argument
```

`tests/cloud/cloudflare/fake-device.ts` implements the full relay protocol (ECDSA auth, proxy, MCP) without a physical board; its identity is persisted in `.fake-device-key.json` so the digest stays stable across runs. `fragment-device.mjs` is a Node helper the tests use to exercise fragmented (streamed) device responses.

### Routes the worker exposes

| Route | Role |
| --- | --- |
| `GET /.well-known/oauth-authorization-server` | OAuth metadata |
| `GET /.well-known/oauth-protected-resource[/device/<digest>/mcp]` | RFC 9728 resource metadata |
| `POST /register`, `POST /token` | MCP OAuth — static client, routing-token code exchange |
| `GET /cloud/oauth/redirect` | Browser pairing page / OAuth redirect |
| `POST /cloud/pairing/session`, `POST /cloud/pairing/token` | Button pairing flow, proxied to the device |
| `GET /cloud/landing/{digest}/page` | Public landing page with MCP, REST, and OpenAPI URLs |
| `GET /cloud/landing/{digest}/online` | `{ "online": true or false }` — authenticated WebSocket attached |
| `GET /cloud/device/{digest}` | Device WebSocket — no bearer; the challenge handshake authenticates |
| `/device/{digest}/…` | Proxied REST + `/mcp` — bearer routing token, JSON only |
