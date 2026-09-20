# 自备开发板

固件并不绑定 Saihub-Mini 这块板子。任何 ESP32-C5 开发板都能跑——Espressif 官方 DevKit 或你自己画的载板都行——只要有 Wi-Fi、天线和足够的空闲 GPIO。智能体对接的一切（MCP、REST、网页控制台、Lua）都长在芯片里。

[English](bring-your-own-board.md) · [README](../README.zh.md) · [硬件文档](hardware.zh.md)

## 在任何 ESP32-C5 上都可用的功能

- MCP 服务器（80 端口 Streamable HTTP）、REST + OpenAPI、Wi-Fi 配网门户和网页控制台
- GPIO：数字输入 / 输出 / 开漏、脉冲、PWM（4 通道，最高 50 kHz）、边沿采集（最长 60 s / 1024 个事件）
- 产品 UART——UART1 经 GPIO 交换矩阵路由，TX / RX 可以落在你映射的任意引脚上
- 沙箱 Lua 5.4 脚本和资源锁
- 云中继：ECDSA 身份密钥在首次启动时烧写进每颗芯片的 eFuse，所以任何板子都能用中继——把 `CONFIG_CLOUD_URL` 指向你自行部署的 Worker 即可（托管中继仅随我们售出的板子提供）

## 离开 Saihub-Mini 硬件后缺失的功能

| 功能 | 原因 | 替代方案 |
| --- | --- | --- |
| 可通断的 3V3 / 5V 电源（`set_output_power_state`） | 依赖板上 TPS2553 负载开关电路 | 把使能脚重映射到你自己的负载开关，或忽略电源工具 |
| 约 1 A 输出限流、3 A 保险丝输入保护 | 属于载板保护电路 | 自行设计供电路径；别把大负载挂在 DevKit 的电源脚上 |
| BOOT 按键交互（按一下 = Wi-Fi 配对） | 按键接在 `CONFIG_BUTTON_PIN` 上 | 多数 C5 DevKit 的 BOOT 键同样接 GPIO28——请对照你的原理图确认。否则重映射该引脚，或把 Wi-Fi 凭据直接写进 `CONFIG_WIFI_SSID` / `CONFIG_WIFI_PASSWORD`：NVS 里没有保存凭据时就会用它们，配网成功后凭据会持久化到 NVS |
| 蜂鸣器提示 | GPIO12 上的 5020 蜂鸣器 | 重映射或忽略 |
| 托管云中继 | 随我们售出的板子提供的服务 | 自行部署 Worker（[使用手册第 5 节](cookbook.zh.md#5-自行部署-cloudflare-中继)） |

## 在哪里改 GPIO 映射

所有和板子相关的引脚都集中在 `main/include/config.h`：

```c
#define CONFIG_GPIO_LOGICAL_TO_HW {10, 1, 0, 23, 4, 5, 6, 24} /* IO0…IO7 → 芯片 GPIO */
#define CONFIG_BUTTON_PIN 28            /* BOOT 按键；按一下 = Wi-Fi 配对 */
#define CONFIG_GPIO_POWER_3V3_PIN 8     /* 3V3_SW 负载开关使能 */
#define CONFIG_GPIO_POWER_5V_PIN 9      /* 5V_SW 负载开关使能 */
#define CONFIG_BUZZER_PIN 12
#define CONFIG_CLOUD_URL "wss://…"      /* 中继源站；留空则禁用中继 */
#define CONFIG_WIFI_SSID "CHANGE_ME"    /* 可选的 Wi-Fi 凭据种子，见上 */
#define CONFIG_WIFI_PASSWORD "CHANGE_ME"
```

`CONFIG_GPIO_LOGICAL_TO_HW` 就是智能体看到的引脚表：第 *N* 项对应 `list_pins`、`set_pin_levels` 等工具里的逻辑引脚 `IO<N>`，值是实际接线的芯片 GPIO。数组长度决定暴露多少个引脚。

[Releases](https://github.com/xqe2011/saihub/releases) 里的预编译 `.bin` 按 Saihub-Mini 的映射和 4 MB Flash 布局构建。自己的板子请重新编译：

```bash
idf.py set-target esp32c5
idf.py build
idf.py merge-bin   # 生成网页烧录器用的合并镜像
```

### 选脚规则

- 逻辑电平只有 3.3 V。
- 避开 strapping 引脚 **GPIO2 / GPIO7 / GPIO28**，不要在复位期间驱动这些脚的电平——只作输入没问题（BOOT 按键接 GPIO28 正是这个原因）。
- 使用原生 USB（Serial/JTAG）时，避开 **GPIO13 / GPIO14**（USB D- / D+）。
- 避开控制台 UART0 占用的引脚，以及模组内部用于 flash / PSRAM、未引出的引脚——请对照模组数据手册和你的 `sdkconfig`。
- 其余引脚都可以自由使用：UART1、PWM 和边沿采集全部经 GPIO 交换矩阵路由。
