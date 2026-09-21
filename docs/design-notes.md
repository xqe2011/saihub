# Cloud relay protocol — design notes

The wire protocol between a Saihub device and a cloud relay server. The protocol is **server-agnostic**: the device only needs a WebSocket endpoint and the message contract below. The current reference implementation is a Cloudflare Worker with one Durable Object per device ([`cloud/cloudflare/src/`](../cloud/cloudflare/src/)); the firmware side lives in [`main/src/cloud.c`](../main/src/cloud.c) and [`main/include/cloud.h`](../main/include/cloud.h).

User-facing setup: [cookbook §5](cookbook.md#5-self-host-the-cloudflare-relay).

## Topology

```
MCP client ──HTTPS──▶ Relay server ──internal──▶ per-device state ◀──WebSocket── ESP32-C5
                        (routing, OAuth)                              (outbound dial)
```

The device always dials out; no inbound connections or port forwarding are needed. The relay keeps per-device state keyed by the device digest, owns the single WebSocket to that board, and multiplexes client HTTP requests over it. In the reference implementation that state is a `Device` Durable Object addressed by `idFromName(digest)`.

### What any relay server must provide

- WebSocket endpoint at `<origin>/cloud/device/<digest>` (upgrade required, no bearer auth — the device proves key ownership during the handshake).
- Client HTTP surface at `<origin>/device/<digest>/<path>` guarded by a routing token bound to that digest, JSON-only.
- The auth handshake, request/response message contract, limits, heartbeat eviction, and timeouts described below.
- OAuth endpoints for MCP clients (metadata, register, token, browser pairing page) per the MCP authorization spec.

## Device identity

- **Key**: ECDSA P-256 key pair. The private key is provisioned into an ESP32-C5 eFuse key block (`ESP_EFUSE_KEY_PURPOSE_ECDSA_KEY`) on first boot if none exists: generated from the RNG (bootloader entropy source, warmed up and discarded), validated through PSA import, then written little-endian via `esp_efuse_write_key`. This is **irreversible**; the block is read- and write-disabled afterwards and the key can only be used through the hardware ECDSA peripheral.
- **Public key**: SEC1 uncompressed point, 65 bytes (`0x04 || X || Y`).
- **Digest (device id)**: `Base58Check(SHA-256(publicKey)[0..20])` — 20-byte payload plus a 4-byte checksum `SHA-256(SHA-256(payload))[0..4]`, Bitcoin alphabet. 26–33 characters (`DIGEST_RE` in `protocol.ts`). The digest is the board's public address, not a secret.
- **Signing**: deterministic ECDSA (RFC 6979, `PSA_ALG_DETERMINISTIC_ECDSA(SHA_256)`) over `SHA-256(M)` where `M = UTF8("saihub/cloud-auth/v1") || 0x00 || challenge`. Signature is raw `r || s`, 64 bytes. The domain-separation prefix is `CLOUD_AUTH_DOMAIN` (firmware) / `AUTH_DOMAIN_PREFIX` (worker).
- Firmware logs the public key (hex) and digest at boot; a factory self-test signs and verifies a random challenge right after provisioning.

## Transport and connection lifecycle

- Firmware origin: `CONFIG_CLOUD_URL` in [`main/include/config.h`](../main/include/config.h) (`ws://` or `wss://`, no path; empty disables the relay). The device connects to `<origin>/cloud/device/<digest>` using `esp_websocket_client` with the cert bundle, 2048-byte buffer, 10 s network timeout, 3 s reconnect backoff.
- The relay task only runs while **Wi-Fi connected, not pairing, and NTP synced** (TLS verification needs valid time). Any protocol violation sets a restart flag: the socket is stopped under the send mutex, the generation counter is bumped, and the task re-dials after 3 s.
- **One connection per digest**: a second WebSocket while one is live must be rejected (reference: `409 device already connected`).
- **Heartbeat**: the firmware sends the text frame `ping` every 10 s and ignores `pong`. The relay should answer `ping` with `pong` (reference: edge-level `setWebSocketAutoResponse`, no object wake-up) and evict a socket after **33 s** of silence (`HEARTBEAT_TIMEOUT_MS`), rejecting all pending requests with 503.
- **Auth timeout**: if authentication is not complete within `AUTH_TIMEOUT_MS` (default 10 s), close the socket (reference: DO alarm, close 1008).
- All protocol messages are **JSON text frames**. The firmware reassembles fragments up to `CONFIG_CLOUD_MAX_MESSAGE_BYTES` (72 KB) and queues at most 8 complete messages for the relay task.
- The reference DO is hibernation-friendly: socket and auth state persist via `serializeAttachment` (`{connectedAt, digest, authenticated, challenge}`) and are restored from `ctx.getWebSockets()` after eviction.

## Authentication handshake

Immediately after accepting the socket, the relay sends:

```json
{ "type": "authRequest", "challenge": "<base64url, 32 random bytes>" }
```

The device answers (challenge must be exactly 43 base64url chars = 32 bytes):

```json
{
  "type": "authResponse",
  "devicePublicKey": "<base64url, 65 bytes>",
  "devicePublicKeyDigest": "<Base58Check digest>",
  "version": "<firmware app version, ≤64 chars>",
  "response": "<base64url, 64-byte signature>"
}
```

Verification (`verifyDeviceAuth` in `crypto.ts`), in order:

1. `devicePublicKeyDigest` equals the digest from the WebSocket URL.
2. `devicePublicKey` decodes to 65 bytes starting with `0x04`.
3. Recomputed `publicKeyDigest(devicePublicKey)` equals the expected digest.
4. ECDSA-P256/SHA-256 signature valid over `"saihub/cloud-auth/v1\0" || challenge`.

Result:

```json
{ "type": "authResult", "success": true }
{ "type": "authResult", "success": false, "reason": "…" }
```

On failure the relay closes the socket (reference: code 1008); the firmware treats rejection as a restart trigger. Messages from an unauthenticated socket other than `authResponse` are refused (`not authenticated`). Stale sockets (not the current one) are closed with 1008 `stale connection`.

## Request / response relay

### Client-facing HTTP surface

- Route: `https://<origin>/device/<digest>/<path>`, guarded by `Authorization: Bearer <routingToken>` whose sealed `devicePublicKeyDigest` must match the URL digest.
- **JSON only**: if a content-type or non-empty body is present it must be `application/json`; `Accept` must permit JSON; violations get 415/406. The device enforces the same rule on its side.
- `GET` / `DELETE` on `/device/<digest>/mcp` are rejected with `405 Allow: POST, OPTIONS` — the relay does not carry SSE streams, so MCP works in Streamable-HTTP POST mode only.
- The reference worker forwards to its DO as `https://device/proxy?path=<path+query>`.

### Relay → device message

```json
{
  "type": "request",
  "requestId": "<hex seq>-<base64url 8 random bytes>",
  "method": "POST",
  "path": "/mcp",
  "headers": { "content-type": "application/json", "…": "…" },
  "body": { },
  "grantSecret": "<32 URL-safe chars>"
}
```

- `grantSecret` is a top-level envelope field (not inside `body`). The relay copies it from the unsealed routing token. The device returns 401 `{reason:"grantSecret not found"}` unless that secret is stored in NVS.
- Header allowlist (`FORWARDED_HEADERS`): `content-type`, `accept`, `x-lock-id`, `mcp-protocol-version`, `mcp-session-id`, `origin`.
- Limits: path ≤ 1024 chars and must start with `/`; body ≤ 64 KB (`MAX_BODY_BYTES`), must be a JSON object or null; ≤ 16 headers, name ≤ 64 chars, value ≤ 512 chars.
- **Headroom**: at most 8 in-flight requests per device (`MAX_PENDING_REQUESTS`); further requests wait in a FIFO queue. The device timeout (`REQUEST_TIMEOUT_MS`, default 55 s) starts **only when a request is admitted**, so queued requests do not time out early; queued fetches keep the reference DO awake.

### Device → relay response

```json
{
  "type": "response",
  "requestId": "<echoed>",
  "status": 200,
  "headers": { "content-type": "application/json" },
  "body": { }
}
```

- The device dispatches the request through the **same route table as its local HTTP server** (`HttpServer_DispatchCloud`), so cloud and LAN behavior are identical. All API routes registered outside Wi-Fi pairing are cloud-exposed.
- Large responses are **streamed as one fragmented WebSocket text message**: `HTTP_CLOUD_BEGIN` sends a partial frame, `HTTP_CLOUD_CHUNK` continuation frames, `HTTP_CLOUD_END` the FIN. A send mutex plus a single stream owner serializes fragmentation; a connection-generation counter aborts stale writers across reconnects (`HTTP_CLOUD_ABORT` triggers a relay restart).
- Relay-side validation (reference `device.ts`): status integer 200–599; serialized body ≤ 1 MB; if a body is present, `content-type` must be JSON; `204/205/304` must carry `body: null`; `content-length` and `transfer-encoding` are stripped before responding to the client.
- Error mapping: device offline / socket lost ⇒ 503 `device offline`; admitted request not answered in time ⇒ 504 `device timeout`; malformed device response ⇒ 502.

## Pairing and routing tokens

The bearer token used by MCP clients is a **routing token**: AES-256-GCM over the JSON `{devicePublicKeyDigest, grantSecret}`, key = `SHA-256(ROUTING_TOKEN_SECRET)`, random 12-byte IV, encoded `base64url(iv || ciphertext || tag)` (`sealRoutingToken` / `openRoutingToken`). The secret is relay-side configuration (reference: a Wrangler secret); alternative servers can use any equivalent sealed-token scheme as long as the token binds a client grant to one digest.

OAuth (MCP authorization) is implemented by the relay:

- `/.well-known/oauth-authorization-server` — metadata: authorization endpoint `/cloud/oauth/redirect`, token endpoint `/token`, registration endpoint `/register`, `authorization_code` + PKCE `S256`, token auth `none`.
- `/register` — returns a static client (`saihub-static-client`), echoing requested redirect URIs.
- `/.well-known/oauth-protected-resource[/device/<digest>/mcp]` — RFC 9728 metadata; 401s carry `WWW-Authenticate: Bearer … resource_metadata=…`.
- `/token` — `grant_type=authorization_code`, `code=<routingToken>`; the seal is validated and the same string is returned as `access_token` (`expires_in` declared as 10 years).

Browser pairing flow (`/cloud/oauth/redirect`, page served by `oauth.ts`):

1. The page asks for a client **name** (1–32 characters), then `POST /cloud/pairing/session` with `{devicePublicKeyDigest, name}`.
2. The relay sends a WebSocket `pairingSessionRequest` `{name}` (no `requestId`; not `type: "request"` / not HTTP `/pairing/*`). The device keeps **one** RAM session (`name`, `expiredAt`) and replies `pairingSessionResponse` with `{sessionToken, expiredAt}`. The Durable Object stores the latest `sessionToken` and TTL. A second session while TTL is live is 409 `a pairing session is already active`. A full grant table (16) is 422 `grant secret limit reached (16)`.
3. The page shows a countdown and asks the user to **hold BOOT for 3 s**. That hold is valid as soon as the session exists. `POST /cloud/pairing/token` is answered by the Durable Object: it matches `sessionToken` against the stored value (401 `session token is invalid or expired` on mismatch or TTL) and holds concurrent waiters until the device approves. The device does not see token waiters.
4. After approval the device mints one 32-character `grantSecret`, stores `{name, grantSecret}` in NVS (`cloud.grants`), and sends a single unsolicited `pairingSessionTokenResponse`. The Durable Object stores that secret, broadcasts it to waiters, and serves later `/pairing/token` calls with the same secret until TTL.
5. The relay seals `{devicePublicKeyDigest, grantSecret}` into a routing token and redirects to `redirect_uri?code=<routingToken>&state=…`.

Grant list/revoke on the device: `GET /cloud/grant-secrets`, `DELETE /cloud/grant-secrets/{grantSecret}` (LAN or cloud). The human control UI has a Cloud tab.

`grantSecret` is `base64url` of 24 random bytes (32 `[A-Za-z0-9_-]` characters).

## Landing page (reference worker)

Public HTML for humans, no bearer token. Firmware logs this URL after a successful `authResult` (`http` + `CONFIG_CLOUD_URL` without the leading `ws`).

- `GET /cloud/landing/<digest>/page` — copyable MCP URL (`/device/<digest>/mcp`), REST API base (`/device/<digest>`), and `openapi.json`. The page polls `/online`.
- `GET /cloud/landing/<digest>/online` — `{ "online": true | false }`. Online means an authenticated WebSocket is currently attached to that digest (heartbeat still valid). Unauthenticated or disconnected sockets are offline.

These routes do not grant API access. MCP and REST still require a routing token.

## Device-side implementation notes (`main/src/cloud.c`)

- One FreeRTOS task (`cloud-relay`, 6 KB stack) owns receive parsing and dispatch; sends from HTTP handlers go through `Cloud_Write` under a mutex.
- Receive path: fragments (opcodes 0/1) are reassembled; control frames may interleave. Oversize or malformed buffers trigger a relay restart. Each queued message carries the connection generation; stale messages are dropped.
- Message handling: `pong` is ignored; `authRequest` is answered only while unauthenticated; `authResult` sets the authenticated flag; `pairingSessionRequest` runs in `cloud.c` (no `requestId`); holding BOOT sends one unsolicited `pairingSessionTokenResponse`; `request` is dispatched synchronously on the relay task (cloud-side timeouts bound the work) after `grantSecret` is checked.
- Requests arriving during Wi-Fi pairing or with the API server down get `503 device API unavailable` from the device itself.

## Constants

| Constant | Value | Where |
| --- | --- | --- |
| Challenge size | 32 bytes | `CHALLENGE_BYTES` |
| Public key / signature | 65 / 64 bytes | `PUBLIC_KEY_BYTES`, `SIGNATURE_BYTES` |
| Digest | 20-byte payload + 4-byte checksum, Base58Check | `DIGEST_PAYLOAD_BYTES`, `DIGEST_CHECKSUM_BYTES` |
| Max device WS message (firmware) | 72 KB | `CONFIG_CLOUD_MAX_MESSAGE_BYTES` |
| Max proxied request body | 64 KB | `MAX_BODY_BYTES` |
| Max device response body | 1 MB (serialized) | reference `device.ts` |
| Max path / headers / name / value | 1024 / 16 / 64 / 512 | `MAX_PATH_LEN`, `MAX_HEADERS`, … |
| In-flight requests per device | 8 | `MAX_PENDING_REQUESTS` |
| Heartbeat interval / timeout | 10 s / 33 s | firmware timer / `HEARTBEAT_TIMEOUT_MS` |
| Auth timeout | 10 s default | `AUTH_TIMEOUT_MS` |
| Request timeout | 55 s default | `REQUEST_TIMEOUT_MS` |
| Device firmware queue | 8 messages | `xQueueCreate(8, …)` |

## Security considerations

- The private key never leaves the eFuse key block; all signing goes through the hardware ECDSA peripheral with deterministic nonces (no RNG reuse risk).
- Domain separation (`saihub/cloud-auth/v1\0`) prevents cross-protocol signature reuse.
- Possession of the digest alone grants nothing: the WebSocket requires a valid challenge signature, and client HTTP requires a sealed routing token bound to that digest.
- Routing tokens double as OAuth codes and long-lived bearer tokens; rotating the relay secret invalidates all of them at once.
- The relay is JSON-only and strips hop-by-hop headers, which keeps the proxy surface small but rules out SSE/binary streaming until revisited.
