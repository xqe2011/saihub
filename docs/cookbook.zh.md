# SAIHub 使用手册

烧录固件、连上 Wi-Fi，把 MCP 地址填进 Cursor、Claude Code 或 Codex CLI，智能体就能驱动板子了。

[English cookbook](cookbook.md) · [README](../README.zh.md)

## 1. 烧录固件

1. 从 [GitHub Releases](https://github.com/xqe2011/saihub/releases) 下载固件 `.bin`。这是 ESP32-C5（4 MB Flash）的**合并镜像**，一个文件包含全部固件。
2. 用 **Chrome** 或 **Edge** 打开官方 [ESP 网页烧录器](https://espressif.github.io/esptool-js/)（依赖 Web Serial）。
3. 先让板子进入**下载模式**（见下文），再点击 Connect，选择对应的 USB 串口。
4. 把 `.bin` 烧到地址 **`0x0`**，芯片选 **ESP32-C5**。如果工具询问其他参数，Flash 模式 DIO、80 MHz、4 MB 即可。
5. 烧录完成后拔掉 USB，重新插上（这次**不要**按住 BOOT），新固件即会启动。

务必使用 **USB 数据线**；纯充电线没有数据芯，无法烧录。

### 进入下载模式

SAIHub-Mini 采用原生 USB，BOOT 键（GPIO28）位于 USB-C 旁边，板上没有单独的 EN / RESET 键。只要在复位瞬间拉低 GPIO28，芯片就会进入 ROM 串口下载模式。

操作步骤：

1. 拔掉 USB。
2. 按住板上的 **BOOT** 键（位于 12 针排针与 USB-C 之间）。
3. 保持按住，插入 USB-C。
4. 等网页烧录器连接成功后，再松开 BOOT。

如果板子已处于上电状态、烧录器一直连不上：

- 继续按住 **BOOT**。
- 用镊子短接板顶的 **TP1** 和 **TP2**（EN / GND 复位焊盘）触发复位，等烧录器连上后再松开 BOOT。

没烧录过的空片往往本来就处于下载模式，可以先直接点 Connect 试试；超时了再走上面的流程。

> 注意：固件正常运行后，按一下 BOOT 是开启 **Wi-Fi 配对**，不是进下载模式。下载模式只在复位（或上电）瞬间按住 BOOT 才会触发。

## 2. 连接 Wi-Fi

1. 正常开机后，**按一下 BOOT**。板子会开出一个无密码热点，名称为 `SAIHUB-` 加 MAC 地址后三字节，例如 `SAIHUB-a1b2c3`。
2. 用手机或电脑连接这个热点。
3. 连接后通常会自动弹出配网页面；如果没有，请在浏览器打开 [http://192.168.4.1/wifi/page](http://192.168.4.1/wifi/page)。
4. 选择你的 Wi-Fi，输入密码，点 **Connect**，等页面显示出 **Device IP**。
5. 记下这个 IP。配对成功几秒后，热点会自动关闭。想取消配对，再按一下 BOOT 即可。

<img src="assets/wifi-pairing.webp" alt="SAIHub Wi-Fi 配对页" style="display:block;max-height:720px;width:auto;margin:0 auto">

之后每次开机都会自动重连已保存的网络。需要更换 Wi-Fi 时，随时按一下 BOOT。

此时局域网内可访问：

| 地址 | 用途 |
| --- | --- |
| `http://<设备IP>/` | 网页控制台（供人工操作） |
| `http://<设备IP>/mcp` | MCP（Streamable HTTP） |
| `http://<设备IP>/openapi.json` | REST 接口描述 |

## 3. 接入 MCP 客户端

将 `DEVICE_IP` 替换为配对页面显示的地址；电脑需与板子处于同一局域网。

添加完成后，不妨给智能体一个具体任务验证连接，例如：「列出所有引脚，把 pin 0 配置为数字输出，拉高一秒钟。」

### Cursor

打开设置 → **Tools & MCP**，或直接编辑 `~/.cursor/mcp.json` / `.cursor/mcp.json`：

```json
{
  "mcpServers": {
    "saihub": {
      "url": "http://DEVICE_IP/mcp"
    }
  }
}
```

### Claude Code

```bash
claude mcp add --transport http saihub http://DEVICE_IP/mcp
claude mcp list
```

添加到项目级配置（写入 `.mcp.json`）：

```bash
claude mcp add --scope project --transport http saihub http://DEVICE_IP/mcp
```

### Codex CLI

```bash
codex mcp add saihub --url http://DEVICE_IP/mcp
```

或编辑 `~/.codex/config.toml`：

```toml
[mcp_servers.saihub]
url = "http://DEVICE_IP/mcp"
```

在 TUI 中输入 `/mcp` 可查看服务器是否在线。

### 智能体可用的工具

GPIO：`list_pins`、`configure_pins`、`get_pin_levels`、`set_pin_levels`、`pulse_pins`、`get_pin_pwms`、`set_pin_pwms`、`trace_pins`。

电源：`get_output_power_state`、`set_output_power_state`（`3v3` / `5v`）。

UART：`list_uarts`、`configure_uart`、`uart_transmit`、`uart_receive`、`uart_flush`。

脚本：`run_script` —— 在设备端沙箱中运行 Lua 5.4，可调用同一套引脚 / 电源工具，另提供 `sleep(us)`。

完整 schema 见 [`mcp.json`](../mcp.json)；REST 对应描述见 [`openapi.json`](../openapi.json)。

## 4. 托管云中继（可选）

**从我们这里购买**的板子自带一个由我们托管的云中继。板子连上 Wi-Fi 后，会与云端保持一条 WebSocket 长连接；之后你在咖啡馆、另一间办公室甚至 CI 里都能用 SAIHub，不受局域网限制。

1. 云端鉴权成功后，串口日志会打印落地页地址：

   `https://saihub.xqe2011.com/cloud/landing/<digest>/page`

2. 打开该地址。页面会显示板子是否在线。接着像填局域网地址一样，把 MCP 地址填进 Cursor、Claude Code 或 Codex CLI。客户端会在浏览器完成 OAuth：给这个客户端起名，然后在板子上**长按 BOOT 3 秒**。无需手动粘贴长期 token。

   使用 REST 需要先拿到 routing token。在落地页点击 **Get routing token**，会走同一套 OAuth 配对流程，然后将会展示 token 并提供复制按钮。之后可在设备控制页的 **Cloud** 标签页撤销授权（`http://<设备IP>/`）。

<img src="assets/cloud-landing.webp" alt="SAIHub 云落地页" style="display:block;max-height:720px;width:auto;margin:0 auto">

自行部署的 Worker 使用相同的 URL 格式；串口日志会打印指向你自己源站的落地页。见下一节。

## 5. 自行部署 Cloudflare 中继

`cloud/cloudflare/` 中的 Worker 借助 Durable Object 把 MCP 和 REST 请求转发到板子，并为 MCP 客户端提供 OAuth 鉴权。中继协议本身不绑定 Cloudflare——消息格式、鉴权握手和各类限制详见[协议设计笔记](design-notes.md)（英文）。

### 部署

准备工作：一个 Cloudflare 账号。D1、Durable Object、环境变量和密钥会自动创建。

[![Deploy to Cloudflare](https://deploy.workers.cloudflare.com/button)](https://deploy.workers.cloudflare.com/?url=https://github.com/xqe2011/saihub/tree/main/cloud/cloudflare)

在配置页填入 `ROUTING_TOKEN_SECRET` 和 `ADMIN_TOKEN`（各用 `openssl rand -hex 32` 生成）。

完成后控制台会给出 `*.workers.dev` 源站地址，例如 `https://saihub-cloud.<account>.workers.dev`。也可以绑定自定义域名。打开该源站的 `/admin`，输入 `ADMIN_TOKEN`，把设备 digest 加入白名单后才能连接；未列入的 digest 会返回 403。

### 配置固件指向你的 Worker

编辑 `main/include/config.h`，填入 WebSocket 源站地址（以 **`wss://`** 开头，不带路径）：

```c
#define CONFIG_CLOUD_URL "wss://saihub-cloud.<account>.workers.dev"
```

然后用 ESP-IDF ≥ 5.5 重新编译并烧录：

```bash
idf.py set-target esp32c5
idf.py build
idf.py merge-bin   # 生成网页烧录器使用的合并镜像，烧到 0x0
```

为 SAIHub-Mini 以外的板子编译？请先重映射引脚：[自备开发板](bring-your-own-board.zh.md)。

板子重启后，设备将连接：

`wss://<源站>/cloud/device/<digest>`

MCP 客户端则使用：

`https://<源站>/device/<digest>/mcp`

`<digest>` 是板子的 Base58Check 设备 ID（由硬件 ECDSA 公钥派生）。板子鉴权成功后，串口日志会打印落地页 `https://<源站>/cloud/landing/<digest>/page`，打开即可复制 MCP、REST 和 OpenAPI 地址。

`ROUTING_TOKEN_SECRET` 和 `ADMIN_TOKEN` 作为 Worker secrets 保存，切勿提交进仓库。

Worker 的路由表、本地模拟设备联调和测试方法见[构建指南](build.zh.md)。

## 6. 引脚、电源和示例

![SAIHub-Mini J2 排针](assets/pinout.webp)

速查——俯视板子、排针朝上时，J2 从左到右：`1:3V3_SW  2:GND  3:5V_SW  4:GND  5:IO0 … 12:IO7`；IO0–IO7 对应 ESP32-C5 GPIO **10、1、0、23、4、5、6、24**；两路电源默认**关闭**。完整引脚表、电源规格和测试点见[硬件文档](hardware.zh.md)。

### 让智能体上手调试真实硬件

把 SAIHub 的 GPIO 和 UART 连到待测板，然后对智能体说：

> 把 pin 0–2 配置为上拉输入，抓取两秒边沿信号，期间我会按一下待测板的按键。然后总结边沿情况，并在 pin 3 上打一个复位脉冲。

这条指令背后是 GPIO + 脉冲 + `trace_pins`（相当于小型逻辑分析仪），还可以顺带用 UART 连接调试串口；PWM 也能为待测板提供它需要的时钟信号。

### 外接设备示例

**舵机。** 打开 5 V 电源，在数据脚输出 PWM（常见 50 Hz，占空比约 2.5–12.5% 对应 0–180°）。可以让智能体连续扫动舵机，并在光电开关对应的 GPIO 上停住。

**灯光。** 用 PWM 驱动 LED 或 MOSFET 调光电路。需要缓慢渐变时，把逻辑写进 `run_script`，配合 `sleep` 实现。

**电磁继电器。** 线圈需要 5 V 就先打开 5 V 电源，把引脚配置为数字输出，拉高即可吸合。可以当作智能开关使用：水泵、门锁、加热器，或工装上的市电继电器（市电只能走继电器触点，绝不能接到 SAIHub 引脚）。

**安全提示。** 带实际负载时请使用 5 V / 3 A 的适配器和线缆；接入不确定的 USB 口时，先关闭电源输出。电源通道的保护阈值约为 1 A，不保证能持续输出 1 A。
