# SAIHub cloud relay (C++)

High-performance single-threaded C++20 relay implementing the `cloud/cloudflare` protocol with an in-memory hot-path cache backed by SQLite (`DB_PATH`), pre-compressed static HTML + forced `gzip` HTML responses, and streaming binary forwarding for `tcp-stream`.

## Container image (`ghcr.io`)

Prebuilt multi-arch (`linux/amd64`, `linux/arm64`) runtime images (~13 MB, built from `scratch`) are published on pushes to `main` and git tags via [`.github/workflows/cloud-cpp-ghcr.yml`](../../.github/workflows/cloud-cpp-ghcr.yml):

```bash
docker run -d -p 8787:8787 \
  -v saihub-data:/data \
  -e DB_PATH=/data/saihub.db \
  -e ROUTING_TOKEN_SECRET="$(openssl rand -hex 32)" \
  -e WEBSOCKET_TOKEN_SECRET="$(openssl rand -hex 32)" \
  -e WEBSOCKET_TOKEN_TTL_S=7200 \
  -e ADMIN_TOKEN="$(openssl rand -hex 32)" \
  ghcr.io/xqe2011/saihub-cloud:latest
```

## Environment variables

| Variable | Default | Description |
| --- | --- | --- |
| `HOST` | `0.0.0.0` | Bind address |
| `PORT` | `8787` | Listen port |
| `AUTH_TIMEOUT_MS` | `10000` | Device authentication challenge timeout (ms) |
| `REQUEST_TIMEOUT_MS` | `55000` | Proxy / pairing / stream-open timeout (ms) |
| `ROUTING_TOKEN_SECRET` | — | Secret used to seal/open bearer routing tokens |
| `WEBSOCKET_TOKEN_SECRET` | — | Secret used to seal/open `tcp-stream` WebSocket tokens |
| `WEBSOCKET_TOKEN_TTL_S` | `0` | TTL (seconds) for injected `tcp-stream` WebSocket tokens |
| `ADMIN_TOKEN` | — | Bearer token required for `/admin/*` APIs |
| `DB_PATH` | *(required)* | SQLite database path for persisting `device_whitelist` and `file_cache` |
| `LOG_LEVEL` | `info` | `debug`, `info`, `warn`, `error`, or `silent` |

## Endpoints

Implements the same routes as `cloud/cloudflare`, plus:

- `GET /health` — returns `200 OK` (`Cache-Control: no-store`) with `status`, `connectedDevices`, `authenticatedDevices`, `activeStreams`, `whitelistedDevices`, and `cachedFiles`.
- HTML pages (`/admin`, `/cloud/landing/:digest/page`, `/cloud/oauth/redirect`, `/cloud/oauth/echo`) always respond with `Content-Encoding: gzip` (`admin` and `oauth/echo` are pre-compressed at startup).

## Local build and tests (Linux Docker, `amd64` / `arm64`)

`cloud/cpp/Dockerfile` is split into `dev` (toolchain, `vcpkg`, tests, benchmarks, `iproute2`) and `run` (minimal `scratch` runtime image) and supports both `linux/amd64` and `linux/arm64`:

```bash
# Build dev image and run unit tests (substitute linux/amd64 or linux/arm64)
docker build --platform linux/arm64 --target dev -t saihub-cloud:dev -f cloud/cpp/Dockerfile cloud/cpp
docker run --rm --platform linux/arm64 saihub-cloud:dev ./build/saihub-tests

# Build minimal runtime image
docker build --platform linux/arm64 --target run -t saihub-cloud:run -f cloud/cpp/Dockerfile cloud/cpp

# Run 10k-connection + 50-stream benchmark pinned to 1 CPU core
docker run --rm --platform linux/arm64 --ulimit nofile=65536:65536 saihub-cloud:dev \
  python3 /src/cloud/cpp/bench/run_bench.py \
  --server-bin ./build/saihub-cloud \
  --bench-bin ./build/bench-load \
  --devices 10000 --streams 50 --duration 15 --stream-hz 20 --stream-bytes 512 --cpu 0
```
