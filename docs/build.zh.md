# 构建指南

面向开发者的固件与 Cloudflare Worker 中继构建参考。普通用户的配置流程见[使用手册](cookbook.zh.md)；中继协议细节见[设计笔记](design-notes.md)（英文）。

[English](build.md) · [README](../README.zh.md)

## 固件

需要 ESP-IDF ≥ 5.5，目标芯片 ESP32-C5：

```bash
idf.py set-target esp32c5
idf.py build
idf.py merge-bin   # 生成网页烧录器使用的合并镜像（烧到 0x0）
```

如果不使用合并镜像，各分区单独烧录的偏移为：bootloader `0x2000`、分区表 `0x8000`、应用 `0x10000`。

与板子相关的配置全部集中在 `main/include/config.h`：

| 宏定义 | 作用 |
| --- | --- |
| `CONFIG_GPIO_LOGICAL_TO_HW` | 逻辑引脚 IO0–IO7 → 芯片 GPIO 的映射；数组长度即暴露的引脚数 |
| `CONFIG_BUTTON_PIN` | BOOT 按键（按一下 = Wi-Fi 配对） |
| `CONFIG_GPIO_POWER_3V3_PIN` / `CONFIG_GPIO_POWER_5V_PIN` | 3V3 / 5V 两路负载开关的使能脚 |
| `CONFIG_BUZZER_PIN` | 蜂鸣器 |
| `CONFIG_CLOUD_URL` | 中继源站，`ws://` 或 `wss://` 开头、不带路径；留空则禁用中继。自行部署时必须修改并重新编译 |
| `CONFIG_WIFI_SSID` / `CONFIG_WIFI_PASSWORD` | Wi-Fi 凭据种子，仅在 NVS 中没有已保存凭据时使用 |

为 SAIHub-Mini 以外的板子编译：见[自备开发板](bring-your-own-board.zh.md)。

推送 git tag 会跑 [`.github/workflows/firmware-release.yml`](../.github/workflows/firmware-release.yml)。tag 名写入 `version.txt`（固件 version 字符串），合并镜像发布为 GitHub Release。若 GitHub Environment `cloud` 配有变量 `ADMIN_URL`（含 `/admin` 的管理地址，例如 `https://example.com/admin`）和密钥 `ADMIN_TOKEN`，会在 `cloud/cloudflare` 下执行 `bun run upload-file-cache -- <adminUrl> <token> <version>`（把 `mcp.json` 和 `openapi.json` POST 到 `{ADMIN_URL}/file-cache`）。缺任一配置则跳过上传。

## Cloudflare Worker

需要 [Bun](https://bun.sh)（或 Node）和 [Wrangler](https://developers.cloudflare.com/workers/wrangler/)。生产环境部署步骤见[使用手册第 5 节](cookbook.zh.md#5-自行部署-cloudflare-中继)。

```bash
cd cloud/cloudflare
bun install
bun run dev            # 先应用本地 D1 迁移，再 wrangler dev → http://127.0.0.1:8787
```

本地联调时，把固件的 `CONFIG_CLOUD_URL` 指向局域网里你的电脑，例如 `ws://<你的局域网IP>:8787`。把 `.dev.vars.example` 复制为 `.dev.vars`，供 `wrangler dev` 使用本地的 `ROUTING_TOKEN_SECRET` 和 `ADMIN_TOKEN`。生产密钥在 Deploy to Cloudflare 配置页填写，切勿提交真实密钥。

`wrangler.jsonc` 绑定 `DEVICE` Durable Object（SQLite 类）、D1 数据库（`device_whitelist` 与 `file_cache`），并设置 `AUTH_TIMEOUT_MS` / `REQUEST_TIMEOUT_MS`（10 s / 55 s）；不在白名单中的设备会收到 403。`file_cache` 以固件 `version` 加文件名（`mcp.json`、`openapi.json`）为键；命中时 MCP `tools/list` 和 `GET /openapi.json` 不必转发到设备。`wrangler.test.jsonc` 会部署独立的 `saihub-cloud-test` Worker（超时 2 s），专供集成测试。

### 检查与测试

```bash
bun run typecheck       # tsc --noEmit
bun run test            # tests/cloud/cloudflare/*.test.ts（客户端流程、headroom 队列）
bun run test:hardware   # tests/hardware/smoke.ts
```

### 本地模拟设备

```bash
bun run fake-device     # 默认连 http://127.0.0.1:8787，也可传入其他源站作为参数
```

`tests/cloud/cloudflare/fake-device.ts` 无需实体板子即可实现完整的中继协议（ECDSA 鉴权、代理、MCP）；身份持久化在 `.fake-device-key.json`，因此 digest 在多次运行间保持稳定。`fragment-device.mjs` 是测试用的 Node 辅助脚本，用来验证分片（流式）设备响应。

### Worker 提供的路由

| 路由 | 作用 |
| --- | --- |
| `GET /.well-known/oauth-authorization-server` | OAuth 元数据 |
| `GET /.well-known/oauth-protected-resource[/device/<digest>/mcp]` | RFC 9728 资源元数据 |
| `POST /register`、`POST /token` | MCP OAuth——静态客户端、routing-token 换 code |
| `GET /cloud/oauth/redirect` | 浏览器配对页 / OAuth 跳转 |
| `GET /cloud/oauth/echo` | OAuth 回调页，展示 routing token |
| `POST /cloud/pairing/session`、`POST /cloud/pairing/token` | 按键配对流程，转发到设备 |
| `GET /cloud/landing/{digest}/page` | 公开落地页，含 MCP、REST、OpenAPI 地址 |
| `GET /cloud/landing/{digest}/online` | `{ "online": true or false }`——当前是否有已鉴权的 WebSocket |
| `GET /cloud/device/{digest}` | 设备 WebSocket——无需 bearer，靠挑战握手鉴权 |
| `/device/{digest}/…` | 代理 REST 和 `/mcp`——需要 bearer routing token，仅限 JSON |
| `GET /admin` | 管理界面——输入 `ADMIN_TOKEN` 管理设备白名单和文件缓存 |
| `GET /admin/devices/whitelist?page=` | 白名单（`Authorization: Bearer <ADMIN_TOKEN>`） |
| `POST /admin/devices/whitelist` | 将 `{ "digest" }` 加入白名单 |
| `DELETE /admin/devices/whitelist/{digest}` | 从白名单删除 digest |
| `GET /admin/file-cache?page=` | 已缓存的 `mcp.json` / `openapi.json`（`version`、`filename`、`bytes`） |
| `POST /admin/file-cache` | 上传 `{ "version", "filename", "content" }`（`mcp.json` 或 `openapi.json`；覆盖写入） |
| `DELETE /admin/file-cache/{version}/{filename}` | 删除一条缓存 |
