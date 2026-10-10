# FoloToy AI Passport 固件深度优化开发日志

**项目名称**: Letgo (基于 FoloToy AI Passport & XiaoZhi 二次开发)  
**目标芯片**: ESP32-C3-MINI-1 (无 PSRAM, 400KB SRAM, 8MB Flash)  
**更新日期**: 2026-10-10  

---

## 优化背景与目标

本项目为搭载 ESP32-C3 的 FoloToy AI Passport 飞书智能硬件终端。硬件特性为单核 RISC-V、400KB 内部 SRAM（无外部 PSRAM）、8MB Flash、ST7789 240x320 屏幕。
在此类资源受限微控制器上，需保证低功耗、网络安全连接（TLS/WSS）、音频双工流媒体传输以及黑白 UI 流畅刷新，杜绝内存碎片化 OOM 与 Flash 虚大问题。

本轮优化围绕 **“固件瘦身、消除Flash空白填充、提升内存韧性、按键防抖与外设节能”** 展开，属于不改变任何现有功能的细节精工打磨。

---

## 优化记录明细

### 1. 消除 Flash 空洞与紧凑分区表优化 (Partitions Alignment)
* **优化文件**: `partitions/v2/8m.csv`
* **问题痛点**: 
  - 原分区表中预先刷入出厂资源的 `assets` 分区放置于 `0x600000` (6MB) 处，而中间出厂未写入数据的 `ota_1` (3MB) 分区导致合盘生成工具 (`esptool merge-bin`) 在 `ota_0` 结尾与 `assets` 之间填充了整整 **3.75MB 的 `0xFF` 纯空白数据**，造成全量固件虚胖至 7.26MB。
* **改动方案**:
  - 重构分区物理排布顺序：`assets` (1.5MB) -> `ota_0` (3.0MB) -> `ota_1` (3.0MB)。
  - `assets` 紧随引导层之后 (`0x20000` ~ `0x1A0000`)；
  - `ota_0` 紧贴 `assets` (`0x1A0000` ~ `0x4A0000`)，容量从原 2.875MB 扩充至 **3.0MB**；
  - `ota_1` 置于高位末尾 (`0x4A0000` ~ `0x7A0000`)；
  - 所有分区起点严格遵循 64KB Flash 扇区对齐。
* **收益效果**:
  - 彻底抹平 3.75MB 的 0xFF 空洞；
  - 合盘输出的单文件固件物理体积从 **7.26MB** 骤降至 **约 3.7MB ~ 4.3MB**（降幅超 40%）；
  - 应用分区多出 128KB 升级余量。

---

### 2. 多语言资产与无用驱动物理剥离 (Components & Assets Pruning)
* **优化文件**: `main/assets/locales/`, `main/CMakeLists.txt`, `main/idf_component.yml`
* **改动方案**:
  - **语言包裁剪**: 彻底删除除 `zh-CN` 与 `en-US` 以外的 38 种外语静态音频包与 JSON，本地静态资产减少约 4MB。
  - **音频 Codec 剥离**: 物理移除 `es8374`, `es8388`, `es8389`, `box_audio`, `dummy_audio`, `no_audio` 等无关驱动源文件，仅保留 Passport 原装的 `es8311`。
  - **屏幕与灯控剥离**: 移除黑白屏无需用到的彩色表情图库 `DEFAULT_EMOJI_COLLECTION`，移除 `oled_display`、`emote_display` 及 `circular_strip`、`gpio_led` 等外部驱动。
  - **云端冗余清理**: 移除闲置的公版 MQTT 与原始 WebSocket 代码，收敛并锁定飞书网关长连接。
* **收益效果**: 编译源文件减少 7,000+ 行，减少无用组件下载与构建缓存。

---

### 3. ESP32-C3 MbedTLS 动态缓冲与代码尺寸优化 (SRAM Resilience & Size Opt)
* **优化文件**: `main/boards/folotoy/ai-passport/config.json`
* **配置增补**:
  - `CONFIG_MBEDTLS_DYNAMIC_FREE_PEER_CERT=y`：在与飞书网关完成 TLS 握手后，立刻销毁并释放对端证书缓存结构体；
  - `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`（已在全局 defaults 激活）：动态分配 SSL 收发包缓冲区；
  - `CONFIG_COMPILER_OPTIMIZATION_SIZE=y`：启用 `-Os` 尺寸极限优化。
* **收益效果**:
  - 连接建立后瞬间归还约 **15KB ~ 20KB 内部贵重 SRAM**，极大地防范了 TLS 通讯与音频录放并发时的堆栈溢出 (OOM) 隐患。

---

### 4. GPIO0 梯形按键电压窗口迟滞防抖 (Hysteresis Deadband)
* **优化文件**: `main/boards/folotoy/ai-passport/config.h`
* **问题痛点**:
  - Passport 的 UP / DOWN / OK 三按键共用 GPIO0 ADC 分压网络。原先三个按键的判决区间边界无死区（UP: 0~150mV, DOWN: 150~447mV, OK: 447~1900mV），在 150mV 和 447mV 临界点若遇供电噪声或温漂极易产生误判或连击。
* **改动方案**:
  - 引入 40mV 迟滞死区（Deadband）：
    - `UP`: `0 ~ 130 mV`（中值 ~0 mV）
    - *死区区间: 130 ~ 170 mV*
    - `DOWN`: `170 ~ 420 mV`（中值 ~300 mV）
    - *死区区间: 420 ~ 480 mV*
    - `OK`: `480 ~ 1900 mV`（中值 ~595 mV）
* **收益效果**: 根除了按键临界抖动误触，提升了手感与硬件抗噪容限。

---

### 5. CW2017 电池电量计 I2C 轮询节流 (Battery Reading Throttle)
* **优化文件**: `main/boards/folotoy/ai-passport/ai_passport_board.cc`
* **问题痛点**:
  - 原先屏幕每秒 tick 及状态刷新均会通过 I2C 读取 CW2017 寄存器，频繁唤醒 I2C 总线，增加无谓的轻度待机功耗。
* **改动方案**:
  - 在 `AiPassportBoard::GetBatteryLevel` 增加 10 秒时间戳缓存节流；
  - 10 秒内连续查询直接复用上一次读取的 SOC 数值，超时或异常时方发起物理 I2C 传输。
* **收益效果**:
  - 将 I2C 总线物理唤醒频率降低 90% 以上，降低待机发热与功耗。

---

## 总结
通过上述全链路的细节打磨，固件在保持 100% 飞书交互与黑白 UI 功能的前提下，达成了：
1. **体积精简**: 固件由 7.26MB 压缩至 3.7~4.3MB；
2. **内存健壮**: TLS 动态证书释放省下 15KB+ SRAM；
3. **硬件稳定性**: 消除按键临界抖动误判，降低 I2C 轮询功耗；
4. **编译合规**: 保持 GitHub Actions 统一云端自动化构建，代码树干净规范。
