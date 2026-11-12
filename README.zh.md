<h1 align="center">
  <img src="docs/assets/robot.svg" width="46" height="46" alt="SAIHub 机器人" valign="middle"> SAIHub ⚡️
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
  · <a href="docs/build.md">构建指南</a>
  · <a href="https://github.com/xqe2011/saihub/releases">Releases</a>
  · <a href="LICENSE">MIT</a>
</p>

<p align="center">
  观看视频：<a href="https://youtu.be/VpJexyDZVVk"><img src="docs/assets/youtube.svg" width="16" height="16" alt="" valign="middle"> YouTube</a>
  · <a href="https://www.bilibili.com/video/BV1MVn66EEgq"><img src="docs/assets/bilibili.svg" width="16" height="16" alt="" valign="middle"> 哔哩哔哩</a>
</p>

---

## SAIHub 是什么

SAIHub 是给编程智能体用的口袋 IO 与硬件调试伴侣。烧好固件、连上 Wi-Fi、把 MCP 地址填进客户端，智能体就有了一只直通真实硬件的手：

- **无线烧录与单步调试**——支持通过局域网或云中继穿透提供 CMSIS-DAP（SWD）与 RFC2217（网络串口），直接用 OpenOCD、GDB 和串口烧录工具远程刷写并单步调试 STM32、RP2040、nRF52、Arduino、ESP32 等各类主流开发板与单片机。
- **总线调试与仿真**——既能直接收发调试 UART、SPI、I2C 总线，也能将 SPI、I2C 切换为从机模式，抓取目标板发出的总线报文，并通过可编程响应映射直接仿真传感器与外设。
- **GPIO / PWM / 边沿采集 / 可编程电源**——翻转引脚、发送微秒级脉冲、输出最高 50 kHz 的 PWM、像逻辑分析仪一样多脚同步抓边沿、通断带故障锁存保护的 3.3 V / 5 V 电源轨，全部通过 MCP 完成。
- **板载脚本**——智能体可以把 Lua 脚本上传到板子上运行，时序敏感或多步骤逻辑在设备本地执行，不必每一步都来回一次网络请求。

硬件本体是 **SAIHub-Mini**：48 × 28 mm 的 ESP32-C5 小板，带 USB-C、12 针排针、带故障锁存保护的可编程 3.3 V / 5 V 电源输出和板载蜂鸣器。引脚定义和电气规格见[硬件文档](docs/hardware.zh.md)。固件、KiCad 工程和中继服务端全部开源在本仓库。

不一定非要用我们的板子——固件兼容任意 ESP32-C5 开发板。哪些功能会受影响、GPIO 映射在哪里改，见[自备开发板](docs/bring-your-own-board.zh.md)。

## 能用来做什么

**零电脑参与的云端智能体闭环开发。** 工位上甚至不需要放电脑——只要用充电头给 SAIHub 供电并连好目标板，在网页或手机上启动 **Cursor Cloud Agents**、**Claude Code 云端会话（Cloud Sessions）** 或 **OpenAI Codex Cloud**，通过 MCP 与云中继接入 SAIHub，云端智能体就能全自动走完整个嵌入式开发闭环：

1. **在云端自动配好工具链**——在云容器内自行安装交叉编译器、OpenOCD、GDB 或串口烧录工具，编写并编译固件。
2. **远程烧录与在线调试**——经由云中继穿透的 CMSIS-DAP（SWD）或 RFC2217 网络串口，直接从云端把固件刷进目标板并挂上 GDB 单步调试。
3. **硬件自动化测试**——控制 3.3 V / 5 V 电源轨上下电复位，用 `trace_pins` 观测引脚波形与脉冲时序，读取 UART 日志，抓取 SPI/I2C 总线流量，或者把 SAIHub 设为 **SPI / I2C 从机模拟传感器**注入测试数据，全面验证固件行为。
4. **直接交付实测可用的固件**——在真实硬件上自动完成验证迭代，最终直接交付通过硬件实测的可运行固件与代码提交，全程无需人工插拔、抓波形或配置本地环境。

**让本地智能体在工位上亲手调硬件。** 把 SAIHub 接在待测板旁边，让本地的 Cursor、Claude Code 或 Codex CLI 自己驱动 GPIO、输出 PWM、抓波形、读写 I2C/SPI、通过 SWD/UART 烧录调试，不用你拿着表笔和跳线逐根去探。

**最省事的智能体 IO 扩展。** 把各种真实外设直接挂到智能体上：

- 用板载 Lua 脚本扫一遍**舵机**角度，让智能体盯着机构动作随时调整。
- 用 PWM 给**灯**调光、闪烁状态 LED，或者在构建与测试完成时让板载**蜂鸣器**发出提示音。
- 吸合**电磁继电器**，通断水泵、电锁、加热器。
- 用带过流保护的可控电源给 3.3 V / 5 V 传感器供电，再经 I2C、SPI、UART 或 GPIO 把数据读回来。

## 快速开始

完整步骤见[使用手册](docs/cookbook.zh.md)：

1. 用 Chrome 或 Edge 打开 [SAIHub 网页烧录器](https://xqe2011.github.io/saihub)，选择版本并烧录。
2. 按一下 **BOOT** 键进入 Wi-Fi 配对，连接 `SAIHUB-xxxxxx` 热点，选好你的 Wi-Fi。
3. 在 Cursor / Claude Code / Codex CLI 里填入 `http://<设备IP>/mcp`。至此，智能体就有了一只放在你工位上的"手"。

## 三种接入方式

工位调试用局域网地址就够了。想在任何地方访问板子，就在前面加一层中继——我们托管，或者你自己部署：

| | 局域网直连 | 自部署中继 | 托管云 |
| --- | --- | --- | --- |
| 适用场景 | 工位调试，零配置 | 远程访问，账号自己掌控 | 远程访问，零运维 |
| MCP 地址 | `http://<设备IP>/mcp` | `https://<你的Worker域名>/device/<digest>/mcp` | `https://saihub.xqe2011.com/device/<digest>/mcp` |
| 访问范围 | 仅同一局域网 | 任意地点 | 任意地点 |
| 准备工作 | 连上 Wi-Fi 即可 | 部署 Worker 然后用你的域名重编固件（[步骤](docs/cookbook.zh.md#5-自行部署-cloudflare-中继)） | 无需配置——从我们这里购买的板子自动接入托管云 |
| 鉴权 | 无（默认信任局域网） | OAuth（经你的中继） | OAuth（浏览器授权） |
| 数据通路 | 直达设备 | 经你的 Cloudflare 账号 | 经我们的基础设施 |
| 费用 | 免费 | Cloudflare 免费额度 | 购板即含 |

板子与中继鉴权成功后，串口日志会打印落地页 `https://<源站>/cloud/landing/<digest>/page`，显示设备是否在线，可复制 MCP 或 REST/OpenAPI 地址，并可获取用于鉴权的Token。

中继协议本身不绑定 Cloudflare——仓库里的 Worker 只是参考实现，详见[协议设计笔记](docs/design-notes.md)（英文）。

## 无厂商锁定

这个项目不把你绑定在我们身上：

- **任意 ESP32-C5 开发板都能用。** SAIHub-Mini 只是一种载板；现成的 DevKit 或你自己画的板子跑的是同一套固件，见[自备开发板](docs/bring-your-own-board.zh.md)。
- **不依赖我们的托管云。** 中继就是一个 Cloudflare Worker，你可以部署到自己的账号下；协议有完整文档且不绑定服务端，也可以完全自己实现。
- **软硬件全部 MIT 开源。** 固件、KiCad 硬件工程、云端中继代码都在本仓库里。
- **局域网模式不需要任何服务。** 连上 Wi-Fi、填入本地 MCP 地址即可完整使用；云端只是可选的便利，不是前提。

## 能力一览

| 模块 | 智能体能做什么 |
| --- | --- |
| GPIO 0–7 | 数字输入 / 输出 / 开漏、微秒脉冲、最高 50 kHz 的 PWM |
| 边沿采集 | 单脚或多脚同步抓取（最长 60 s），相当于一台口袋逻辑分析仪 |
| SWD 调试 | 基于 TCP 的 CMSIS-DAP，支持局域网或云中继下接入 OpenOCD / GDB |
| UART | 一路产品级 UART，带 RTS / DTR 支持自动烧录电路，支持 RFC2217 网络串口 |
| SPI | 主机（最高 10 MHz）与从机，可抓取、产生、模拟总线通信 |
| I2C | 主机（最高 1 MHz）与从机，支持 7 / 10 位地址，可抓取、产生、模拟总线通信 |
| 电源 | 可通断的 **3V3** / **5V** 输出，带过流/故障保护 |
| 蜂鸣器 | 板载摩斯电码无源蜂鸣器 |

逻辑引脚仅支持 **3.3 V**，完整电气规格见[硬件文档](docs/hardware.zh.md)。

## 仓库结构

| 路径 | 内容 |
| --- | --- |
| `main/` | ESP-IDF 固件（ESP32-C5，IDF ≥ 5.5） |
| `mcp.json` | 设备提供的 MCP 工具定义 |
| `openapi.json` | 同一套能力的 REST API 描述 |
| `cloud/cloudflare/` | Cloudflare Worker 中继与 OAuth（参考服务端） |
| `hardware/` | SAIHub-Mini KiCad 工程 |
| `docs/` | 使用手册、硬件文档、构建指南、协议设计笔记、自备开发板指南 |

## 社区交流

- [加入 Discord](https://discord.gg/aZerprjxsc)
- QQ 群：**499504696**

## 许可证

[MIT](LICENSE)。
