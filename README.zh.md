<h1 align="center">
  <img src="docs/assets/robot.svg" width="46" height="46" alt="Saihub 机器人" valign="middle"> Saihub ⚡️
</h1>

<p align="center"><strong>给智能体搭把手。</strong></p>

<p align="center">
  MIT 开源、为智能体打造的 IO 板。<br>
  ESP32-C5 固件原生支持 <a href="https://modelcontextprotocol.io/">MCP</a>，
  Cursor、Claude Code、Codex 都能直接驱动真实硬件。
</p>

<p align="center">
  <a href="README.md">English</a>
  · <a href="docs/cookbook.zh.md">使用手册</a>
  · <a href="docs/hardware.zh.md">硬件文档</a>
  · <a href="docs/design-notes.md">协议设计</a>
  · <a href="https://github.com/xqe2011/saihub/releases">Releases</a>
  · <a href="LICENSE">MIT</a>
</p>

---

## Saihub 是什么

Saihub 是给编程智能体用的口袋 IO 板。烧好固件、连上 Wi-Fi、把 MCP 地址填进编辑器，智能体就能翻转引脚、输出 PWM、收发 UART、通断 3.3 V / 5 V 电源，还能像逻辑分析仪一样抓取边沿信号——在工位上用，或者经云端中继远程用，都行。

硬件本体是 **Saihub-Mini**：48 × 28 mm 的 ESP32-C5 小板，带 USB-C、BOOT 按键、12 针排针和外接 U.FL 天线。引脚定义和电气规格见[硬件文档](docs/hardware.zh.md)。固件、KiCad 工程和中继服务端全部开源在本仓库。

不一定非要用我们的板子——固件兼容任意 ESP32-C5 开发板。哪些功能会受影响、GPIO 映射在哪里改，见[自备开发板](docs/bring-your-own-board.zh.md)。

## 能用来做什么

**让智能体亲手调硬件。** 把 Saihub 接在待测板旁边，智能体就能自己驱动 GPIO、输出 PWM、抓波形、收发 UART，不用你拿着表笔逐根线去探。

**最省事的智能体 IO 扩展。** 把各种外设直接挂到智能体上：

- 用板载 Lua 脚本扫一遍**舵机**角度，让智能体盯着机构动作随时调整。
- 用 PWM 给**灯**调光，或者闪烁状态 LED。
- 吸合**电磁继电器**，通断水泵、电锁、加热器。
- 用可控电源给 3.3 V 传感器供电，再经 UART 或 GPIO 把数据读回来。

## 快速开始

完整步骤见[使用手册](docs/cookbook.zh.md)：

1. 从 [Releases](https://github.com/xqe2011/saihub/releases) 下载固件 `.bin`，用 [ESP 网页烧录器](https://espressif.github.io/esptool-js/) 烧录。
2. 按一下 **BOOT** 键进入 Wi-Fi 配对，连接 `SAIHUB-xxxxxx` 热点，选好你的 Wi-Fi。
3. 在 Cursor / Claude Code / Codex CLI 里填入 `http://<设备IP>/mcp`。至此，智能体就有了一只放在你工位上的"手"。

## 三种接入方式

工位调试用局域网地址就够了。想在任何地方访问板子，就在前面加一层中继——我们托管，或者你自己部署：

| | 局域网直连 | 自部署中继 | 托管中继 |
| --- | --- | --- | --- |
| 适用场景 | 工位调试，零配置 | 远程访问，账号自己掌控 | 远程访问，零运维 |
| MCP 地址 | `http://<设备IP>/mcp` | `https://<你的Worker域名>/device/<digest>/mcp` | `https://<托管域名>/device/<digest>/mcp` |
| 访问范围 | 仅同一局域网 | 任意地点 | 任意地点 |
| 准备工作 | 连上 Wi-Fi 即可 | [部署 Worker](docs/cookbook.zh.md#5-自行部署-cloudflare-中继)，并用你的域名重编固件 | 无需配置——从我们这里购买的板子自动接入托管中继 |
| 鉴权 | 无（默认信任局域网） | OAuth（经你的中继） | OAuth（浏览器授权） |
| 数据通路 | 直达设备 | 经你的 Cloudflare 账号 | 经我们的 Cloudflare 账号 |
| 费用 | 免费 | Cloudflare 免费额度 | 购板即含 |

板子与中继鉴权成功后，串口日志会打印落地页 `https://<源站>/cloud/landing/<digest>/page`，显示设备是否在线，可复制 MCP 或 REST/OpenAPI 地址，并可获取用于鉴权的Token。

中继协议本身不绑定 Cloudflare——仓库里的 Worker 只是参考实现，详见[协议设计笔记](docs/design-notes.md)（英文）。

## 能力一览

| 模块 | 智能体能做什么 |
| --- | --- |
| GPIO 0–7 | 数字输入 / 输出 / 开漏、脉冲、最高 50 kHz 的 PWM |
| 边沿采集 | 单脚或多脚同步抓取（最长 60 s），相当于一台小逻辑分析仪 |
| UART | 一路产品级 UART，支持 UTF-8 或原始字节 |
| 电源 | 可通断的 **3V3** / **5V** 输出（默认关闭） |
| 脚本 | 板载沙箱 Lua 5.4 |
| 接入 | 80 端口 Streamable HTTP MCP、REST + OpenAPI、可选云中继 |

逻辑引脚仅支持 **3.3 V**，完整电气规格见[硬件文档](docs/hardware.zh.md)。

## 仓库结构

| 路径 | 内容 |
| --- | --- |
| `main/` | ESP-IDF 固件（ESP32-C5，IDF ≥ 5.5） |
| `mcp.json` | 设备提供的 MCP 工具定义 |
| `openapi.json` | 同一套能力的 REST API 描述 |
| `cloud/cloudflare/` | Cloudflare Worker 中继与 OAuth（参考服务端） |
| `hardware/` | Saihub-Mini KiCad 工程 |
| `docs/` | 使用手册、硬件文档、构建指南、协议设计笔记、自备开发板指南 |

## 许可证

[MIT](LICENSE)。
