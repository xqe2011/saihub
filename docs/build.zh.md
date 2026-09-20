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

为 Saihub-Mini 以外的板子编译：见[自备开发板](bring-your-own-board.zh.md)。

## Cloudflare Worker（参考中继实现）

需要 [Bun](https://bun.sh)（或 Node）和 [Wrangler](https://developers.cloudflare.com/workers/wrangler/)。生产环境部署步骤见[使用手册第 5 节](cookbook.zh.md#5-自行部署-cloudflare-中继)。

```bash
cd cloud/cloudflare
bun install
bun run dev            # wrangler dev → http://127.0.0.1:8787
```

本地联调时，把固件的 `CONFIG_CLOUD_URL` 指向局域网里你的电脑，例如 `ws://<你的局域网IP>:8787`。`.dev.vars` 只为 `wrangler dev` 提供本地的 `ROUTING_TOKEN_SECRET`；生产环境的密钥用 `wrangler secret put ROUTING_TOKEN_SECRET` 管理，切勿提交真实密钥。

`wrangler.jsonc` 绑定 `DEVICE` Durable Object（SQLite 类），并设置 `AUTH_TIMEOUT_MS` / `REQUEST_TIMEOUT_MS` 默认值（10 s / 55 s）。`wrangler.test.jsonc` 会部署一个独立的 `saihub-cloud-test` Worker，超时缩短为 2 s，专供集成测试。

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
| `POST /cloud/pairing/session`、`POST /cloud/pairing/token` | 按键配对流程，转发到设备 |
| `GET /cloud/device/{digest}` | 设备 WebSocket——无需 bearer，靠挑战握手鉴权 |
| `/device/{digest}/…` | 代理 REST 和 `/mcp`——需要 bearer routing token，仅限 JSON |
