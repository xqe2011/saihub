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

Building for a board other than SAIHub-Mini: [bring your own board](bring-your-own-board.md).

Pushing a git tag runs [`.github/workflows/firmware-release.yml`](../.github/workflows/firmware-release.yml). The tag name is written to `version.txt` (the firmware version string) and the merged image is published as a GitHub Release. If GitHub Environment `cloud` has variable `ADMIN_URL` (admin origin including `/admin`, e.g. `https://example.com/admin`) and secret `ADMIN_TOKEN`, a parallel job runs `bun run upload-file-cache -- <adminUrl> <token> <version>` from `cloud/cloudflare` (POSTs `mcp.json` and `openapi.json` to `{ADMIN_URL}/file-cache`). If either is unset, the upload is skipped.

## Cloudflare Worker

Requires [Bun](https://bun.sh) (or Node) and [Wrangler](https://developers.cloudflare.com/workers/wrangler/). Production deploy steps: [cookbook §5](cookbook.md#5-self-host-the-cloudflare-relay).

```bash
cd cloud/cloudflare
bun install
bun run dev            # applies local D1 migrations, then wrangler dev → http://127.0.0.1:8787
```

For a dev loop, point the firmware's `CONFIG_CLOUD_URL` at your machine over the LAN, e.g. `ws://<your-lan-ip>:8787`. Copy `.dev.vars.example` to `.dev.vars` for local `ROUTING_TOKEN_SECRET` and `ADMIN_TOKEN`. Production secrets are set on the Deploy to Cloudflare setup page — never commit a real secret.

`wrangler.jsonc` binds the `DEVICE` Durable Object (SQLite class), a D1 database (`device_whitelist` and `file_cache`), and `AUTH_TIMEOUT_MS` / `REQUEST_TIMEOUT_MS` (10 s / 55 s); devices not in the whitelist get 403. `file_cache` is keyed by firmware `version` plus filename (`mcp.json`, `openapi.json`); a hit answers MCP `tools/list` and `GET /openapi.json` without waking the board. `wrangler.test.jsonc` deploys a separate `saihub-cloud-test` worker with 2 s timeouts for integration tests.

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
| `GET /cloud/oauth/echo` | OAuth callback that displays the routing token |
| `POST /cloud/pairing/session`, `POST /cloud/pairing/token` | Button pairing flow, proxied to the device |
| `GET /cloud/landing/{digest}/page` | Public landing page with MCP, REST, and OpenAPI URLs |
| `GET /cloud/landing/{digest}/online` | `{ "online": true or false }` — authenticated WebSocket attached |
| `GET /cloud/device/{digest}` | Device WebSocket — no bearer; the challenge handshake authenticates |
| `/device/{digest}/…` | Proxied REST + `/mcp` — bearer routing token, JSON only |
| `GET /admin` | Admin UI — enter `ADMIN_TOKEN` to manage the device whitelist and file cache |
| `GET /admin/devices/whitelist?page=` | whitelist (`Authorization: Bearer <ADMIN_TOKEN>`) |
| `POST /admin/devices/whitelist` | Add `{ "digest" }` to the whitelist |
| `DELETE /admin/devices/whitelist/{digest}` | Remove a digest from the whitelist |
| `GET /admin/file-cache?page=` | Cached `mcp.json` / `openapi.json` rows (`version`, `filename`, `bytes`) |
| `POST /admin/file-cache` | Upload `{ "version", "filename", "content" }` (`mcp.json` or `openapi.json`; upsert) |
| `DELETE /admin/file-cache/{version}/{filename}` | Remove a cached file |
