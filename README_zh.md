# Letgo - FoloToy AI Passport 飞书随身智能终端

（中文 | [English](README.md)）

本项目是专为 **FoloToy AI Passport** 硬件量身定制的飞书智能随身语音终端固件，作为飞书办公与智能机器人生态的实体硬件入口。

> [!IMPORTANT]
> **云端编译规范**：本项目的所有固件构建**只能且必须在 GitHub Actions 云端流水线进行编译**。本地环境因 ESP-IDF 6.1 工具链与复杂组件缓存差异，不提供也不支持本地构建。每次提交代码推送到 `main` 分支后，GitHub Actions 会自动触发构建并发布全量烧录包。

---

## 📌 项目渊源与二次开发说明

本项目基于以下开源项目深度定制与二次开发而来：

* **直接上游**: [FoloToy / folo-ai-passport-xiaozhi](https://github.com/FoloToy/folo-ai-passport-xiaozhi)（专为 AI Passport 适配的小智分支）
* **基础原型**: [XiaoZhi ESP32](https://github.com/78/xiaozhi-esp32)（小智开源 AI 聊天机器人固件）

### 为什么二次开发？
原版固件面向公版开发板与通用大模型，包含了大量通用驱动、38 种外语资源、彩色 Emoji、闲置协议栈以及产生 3.75MB 空洞的分区表，导致合盘固件高达 7.26MB 且在仅有 400KB 内存的 ESP32-C3 上极易因 TLS 握手产生内存碎片（OOM）。

**本项目对其进行了彻底的重构与深度优化**：
1. **纯粹面向飞书生态**：剔除无关的公版协议栈与通用逻辑，打通与飞书智能网关的私有长连接交互。
2. **硬件物理级瘦身**：剥离 38 种外语、彩色表情和无关屏幕/音频驱动，将全量固件合盘体积从 **7.26MB 压缩至 3.87MB（减少 46.6%）**，彻底抹平 Flash 0xFF 空洞。
3. **极简纯黑白高对比度 UI**：针对 ST7789 240x320 竖屏量身打造纯黑白线框交互、随声音能量波动的原生频谱波浪、音量居中胶囊 HUD 以及 90 秒自适应息屏待机时钟。
4. **C3 芯片级稳定性保障**：启用 MbedTLS 动态证书释放（归还 15KB+ 内部 SRAM）、ES8311 停播软静音消爆音、梯形电阻按键 40mV 迟滞防抖与 I2C 电池轮询节流。

---

## 🛠️ 本项目做什么用的？

`Letgo` 将 FoloToy AI Passport 变成一个高响应、长续航的**飞书实体语音助手**：

* **飞书即时语音对话**：通过 Push-to-Talk（单击 OK 键或长按）与飞书机器人流式语音对话，支持实时流式打断（短按 OK 键即停）。
* **极简声音能量波浪**：屏幕中央常驻纯白直线，在说话录音和回复播报时实时转换为 15 频段音频能量频谱动效（纯 LVGL 矢量绘制，零图片依赖）。
* **飞书网关自动发现与项目切换**：
  * **网关选择**：局域网 mDNS/UDP 自动发现运行中的多个飞书网关服务，未连接时一键弹出选择列表；
  * **多项目切换**：短按 ▲ 键即可弹出项目切换菜单，在不同的飞书智能体/工作空间之间无缝切换。
* **随身时钟与智能节能**：
  * 待命 15 秒无操作自动展示全屏极简大字体数字时钟；
  * 待命 30 秒自动微光节能（20% 亮度）；
  * 待命 90 秒深度息屏休眠（0% 亮度），按键或消息到达时 200ms 内平滑淡入唤醒。

---

## 🔗 项目依赖体系

### 1. 服务端 / 调度依赖（核心大脑：Antigravity CLI 与飞书机器人网关）

`Letgo` 并不是一个普通的独立联网音箱，而是 **Google Antigravity CLI** 驱动的飞书智能机器人系统在物理世界的随身硬件投影。

为了让 Passport 硬件正常工作并与飞书进行语音对话、调度大模型和操作项目，**必须在您的宿主机/服务器上部署以下两项核心服务**：

#### 依赖 A：Google Antigravity CLI (`agy`)
* **定位**：核心推理与工程执行引擎（由 Google DeepMind 研发的高级 Agentic Coding 体系）。
* **作用**：负责接收飞书网关转交的用户语音指令，在服务器工作区自动执行多步任务规划、读取与修改代码、执行 Shell 终端命令、管理上下文以及调用 MCP 工具。
* **安装要求**：
  * 宿主机系统需预先安装 Antigravity CLI 并配置系统环境变量，使终端支持 `agy` 或 `antigravity` 命令；
  * 执行 `agy login` 完成账号授权登录，或在环境变量中注入对应的认证凭据。

#### 依赖 B：飞书网关插件服务 (`antigravity-feishu-bot`)
* **定位**：连接飞书平台、宿主机 Antigravity CLI 以及 Passport 硬件的核心中间件网关。
* **作用**：
  1. 通过飞书原生 WebSocket 长连接与开放平台通信（**无需公网 IP，无需配置 Webhook 回调域名**）；
  2. 在局域网内通过 UDP / mDNS 广播服务，让 AI Passport 开机秒级自动发现并建立私有长连接；
  3. 双向流式转发：将硬件上传的 Opus 语音流实时转写并调度 `agy`，同时将推理答复通过 Edge-TTS 实时合成回传给硬件喇叭播报，并在飞书 App 客户端渲染富媒体动态流转卡片。
* **环境要求**：
  * **操作系统**：Linux (Ubuntu 20.04+ / Debian 11+ / CentOS 等) 或 macOS；
  * **Python 环境**：Python 3.10 及以上版本；
  * **进程守护**：Node.js & PM2（推荐用于后台 24x7 高可用守护）。
* **安装与部署步骤**：
  ```bash
  # 1. 克隆飞书机器人网关仓库
  git clone https://github.com/Level6me/antigravity-feishu-bot.git
  cd antigravity-feishu-bot

  # 2. 创建并激活 Python 虚拟环境
  python3 -m venv venv
  source venv/bin/activate

  # 3. 安装依赖包 (包括 lark-oapi, websockets, edge-tts, requests 等)
  pip install -r requirements.txt

  # 4. 配置环境变量
  cp .env.example .env
  nano .env  # 填写飞书应用的 APP_ID 与 APP_SECRET
  ```
* **一键交互式安装（备选推荐）**：
  在服务器终端直接执行：
  ```bash
  bash <(curl -sL https://raw.githubusercontent.com/Level6me/antigravity-feishu-bot/main/install.sh)
  ```
* **启动服务**：
  ```bash
  pm2 start venv/bin/python3 --name "feishu-bot" -- main.py
  pm2 save
  ```

#### 依赖 C：飞书开放平台应用权限配置
需在 [飞书开放平台 (open.feishu.cn)](https://open.feishu.cn/) 创建企业“企业自建应用”，获取凭证并开启以下权限：
1. **启用机器人能力**：在“添加应用能力”中开启“机器人”；
2. **事件订阅方式**：选择 **“使用 WebSocket 长连接接收事件”**；
3. **开通必要权限**：
   * `im:message`（接收消息与事件）；
   * `im:message:send_as_bot`（以应用身份发送消息）；
   * `im:resource`（获取与上传图片、音频、富媒体文件）。

### 2. 硬件规格依赖
* **主控芯片**: ESP32-C3-MINI-1 (单核 RISC-V 32 位，最高 160MHz，400KB 内部 SRAM，8MB SPI Flash，**无外部 PSRAM**)
* **音频编解码**: ES8311 (I2S 全双工，标准 16kHz 采样率)
* **显示屏**: ST7789 240x320 4-line SPI 竖屏
* **按键电路**: UP / DOWN / OK 三键共用 GPIO0 ADC1_CH0 梯形电阻分压网络
* **电量计量**: CellWise CW2017 (I2C 接口，520mAh 专用放电曲线)

### 3. ESP-IDF 组件与第三方库依赖
固件构建基于 **ESP-IDF v6.1**（最低版本要求 `>= v6.0.1`）：
* **网络与连接**:
  * `78/esp-wifi-connect` (~3.3.1): Wi-Fi 连接管理、Web AP 动态配网门户。
  * `78/esp-ml307` (~3.7.0): 提供网络与套接字统一抽象层接口。
* **图形与界面**:
  * `lvgl/lvgl` (~9.5.0) & `esp_lvgl_port` (~2.9.0): 轻量级嵌入式 GUI 图形栈。
  * `78/xiaozhi-fonts` (~2.0.0): 内置点阵中英文字体与 Material Symbols 图标字库。
  * `espressif/esp_image_effects` (^1.1.0): 屏幕色彩格式转换与图像裁剪。
  * `espressif2022/esp_emote_expression` (^1.0.2): 嵌入式二维码轻量生成引擎。
* **音频与外设**:
  * `espressif/esp_codec_dev` (~1.6.2) & `espressif/esp_audio_codec` (~2.5.0): ES8311 驱动抽象与流控制。
  * `espressif/esp_audio_effects` (~1.3.0): 音频重采样与数字增益。
  * `espressif/button` (~4.2.1): 按键驱动与长按/双击事件分发。
  * `espressif/esp_mmap_assets` (^1.4.0): Flash 只读静态资源映射。

---

## ⚡ 深度细节优化亮点

| 优化维度 | 优化措施 | 实际效果 |
| :--- | :--- | :--- |
| **固件合盘体积** | 紧凑型分区表重排：`assets`(1.5M) -> `ota_0`(3.0M) -> `ota_1`(3.0M) | 彻底消除 3.75MB 的 0xFF 空洞，合盘从 **7.26MB 锐减至 3.87MB (-46.6%)** |
| **SRAM 极度抗崩** | 开启 `CONFIG_MBEDTLS_DYNAMIC_FREE_PEER_CERT=y` 与动态缓冲 | TLS 握手后即刻销毁证书结构体，归还 **15KB~20KB** 珍贵内部 SRAM |
| **音频停播消爆** | 在关闭音频通道前先将 DAC 输出软静音归零 | 杜绝播放完毕和暂停时功放产生的瞬态“咔嗒”Pop 爆音 |
| **按键迟滞抗噪** | 为 GPIO0 电阻梯按键引入 40mV 迟滞死区 (Deadband) | 彻底消除温漂与电池低压纹波引起的按键临界跳变与误触 |
| **待机功耗与发热** | 电池读取 10s 缓存节流 + 200ms 自适应呼吸背光 + 90s 息屏休眠 | 物理 I2C 唤醒减少 90%，空闲时配合 Modem-Sleep 显著延长续航 |

*更多底层优化细节详见开发日志：[docs/OPTIMIZATION_LOG.md](docs/OPTIMIZATION_LOG.md)*。

---

## 🚀 固件获取与刷机

### 1. 下载固件
请前往本仓库的 [Releases 页面](https://github.com/Level6me/Letgo/releases) 下载最新发布的单文件全量合盘包：
* 固件文件名：`FoloToy-AI-Passport-full.bin`
* 适用起始烧录地址：`0x00000000`（单文件合盘，包含 Bootloader、分区表、静态 Assets 及主程序）

### 2. 刷机方式

#### 方式 A：Web 网页一键烧录（推荐）
1. 使用 Chrome / Edge 浏览器打开 [ESP Web Flasher](https://espressif.github.io/esptool-js/) 或任意 Web 串口工具；
2. 用 Type-C 数据线将 Passport 连接至电脑（按住 OK 键插入 USB 可强制进入下载模式）；
3. 选择固件文件 `FoloToy-AI-Passport-full.bin`，偏移地址填写 `0x0`，点击烧录即可。

#### 方式 B：esptool 命令行烧录
```bash
esptool.py -p /dev/ttyACM0 -b 921600 --chip esp32c3 write_flash 0x0 FoloToy-AI-Passport-full.bin
```

---

## 📶 使用与配网指南

1. **进入配网模式**：
   * 首次开机或双击 ▼ 键查看设备信息；长按 ▼ 键可强制进入 Wi-Fi 配网热点模式。
   * 手机连接名为 `Passport-XXXX` 的 Wi-Fi 热点，浏览器自动弹出配置页面（或访问 `192.168.4.1`），配置您路由器的 Wi-Fi 账密。
2. **连接飞书网关**：
   * 确保您的电脑或服务器已启动 `antigravity-feishu-bot` 网关服务，且与 Passport 处于同一局域网；
   * 设备联网后会自动搜索局域网网关；若存在多个网关，屏幕将弹出选择菜单，短按 ▲/▼ 键选中后短按 OK 确认；
   * 屏幕左上角显示 `[已连接]` 状态时，即可长按或单击 OK 键与飞书机器人开始对话！

---

## 📄 开源许可证

本项目核心业务代码遵循 MIT 许可证开源。所包含的组件与第三方库版权归各自所有者所有。
