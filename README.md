# Letgo - Feishu Assistant Firmware for FoloToy AI Passport

([中文](README_zh.md) | English)

`Letgo` is a highly-optimized, dedicated firmware designed specifically for the **FoloToy AI Passport** (ESP32-C3) hardware. It transforms the portable badge device into a responsive, desktop/wearable hardware voice assistant connected to the **Feishu (Lark) Intelligent Bot ecosystem**.

> [!IMPORTANT]
> **Cloud Build Requirement**: All firmware builds for this project **MUST be performed via GitHub Actions in the cloud**. Local compilation is neither maintained nor supported due to ESP-IDF v6.1 toolchain variances and component requirements. Pushing commits to the `main` branch automatically triggers cloud workflows that build and package the binary releases.

---

## 📌 Project Lineage & Derivative Context

This project is a deep customization and secondary development based on:

* **Direct Upstream**: [FoloToy / folo-ai-passport-xiaozhi](https://github.com/FoloToy/folo-ai-passport-xiaozhi) (XiaoZhi fork tailored for AI Passport hardware)
* **Foundational Architecture**: [XiaoZhi ESP32](https://github.com/78/xiaozhi-esp32) (Open-source ESP32 voice chatbot framework)

### Why Secondary Development?
The generic upstream firmware targeted multi-board public releases and general LLM platforms, containing hundreds of unused drivers, 38 foreign languages, color emojis, legacy MQTT/WebSocket protocols, and partition layouts that introduced a massive **3.75 MB 0xFF gap** (resulting in a bloated 7.26 MB firmware). On the resource-constrained ESP32-C3 (only 400 KB internal SRAM and no PSRAM), TLS handshakes frequently triggered heap fragmentation and Out-Of-Memory (OOM) crashes.

**Letgo completely refactors and hardens this codebase**:
1. **Dedicated to Feishu (Lark)**: Stripped generic protocol stacks in favor of a clean, persistent local/remote link to `antigravity-feishu-bot`.
2. **Physical-Level Pruning**: Removed 38 foreign language packs, emoji graphics, and unrelated codec/display drivers, trimming the binary from **7.26 MB down to 3.87 MB (-46.6%)** and eliminating 0xFF Flash holes.
3. **Minimalist Monochrome UI**: Replaced color emojis with crisp, high-contrast monochrome vector graphics on the ST7789 240x320 portrait screen (dynamic 15-band voice spectrum waves, pill-shaped volume HUD, full-screen standby clock).
4. **C3 Silicon Stability**: Implemented MbedTLS dynamic peer certificate deallocation (reclaiming **15KB+ SRAM** post-handshake), ES8311 soft-mute anti-pop routines, ADC button hysteresis deadbands (40mV gap), and battery polling throttling.

---

## 🛠️ What is Letgo Used For?

* **Feishu Real-time Voice Chat**: One-click Push-to-Talk via the OK button, full-duplex Opus streaming, and instant tap-to-interrupt capability.
* **Minimalist Native Energy Waves**: A crisp horizontal center line dynamically expands into a 15-band real-time audio FFT wave during recording and speech output (zero bitmap image overhead).
* **Multi-Gateway Discovery & Project Switching**:
  * **LAN Gateway Pairing**: Auto-discovers local `antigravity-feishu-bot` gateways via mDNS/UDP with an on-screen selection menu.
  * **Workspace / Project Switcher**: Tap the ▲ button to seamlessly switch between multiple workspace agents on the fly.
* **Standby Clock & Multi-Tier Power Saving**:
  * 15s inactivity: Automatically switches to full-screen digital clock dashboard.
  * 30s inactivity: Smoothly dims backlight to 20%.
  * 90s inactivity: Enters deep display sleep (0% backlight) while maintaining Wi-Fi Modem-Sleep. Instantly wakes on keypress or message arrival within 200ms.

---

## 🔗 Project Dependencies

### 1. Server / Ecosystem Dependencies
* **[antigravity-feishu-bot](https://github.com/Level6me/antigravity-feishu-bot)**: Companion Feishu gateway daemon. Handles Feishu OpenAPI WebSocket connections, LLM agent orchestration, Feishu Bitable synchronization, and local UDP service discovery.
* **Lark Open Platform**: Feishu internal enterprise bot application credentials.

### 2. Hardware Target
* **SoC**: ESP32-C3-MINI-1 (Single-core RISC-V 32-bit @ 160MHz, 400KB SRAM, 8MB SPI Flash, **No external PSRAM**)
* **Audio Codec**: ES8311 (I2S full-duplex, native 16kHz sampling)
* **Display**: ST7789 240×320 4-line SPI portrait LCD
* **Keys**: UP / DOWN / OK ladder keys sharing GPIO0 ADC1_CH0
* **Fuel Gauge**: CellWise CW2017 (I2C interface with dedicated 520mAh battery curve)

### 3. ESP-IDF & Component Dependencies
Built on **ESP-IDF v6.1** (`>= v6.0.1` required):
* **Networking**:
  * `78/esp-wifi-connect` (~3.3.1): Wi-Fi station management and captive portal AP.
  * `78/esp-ml307` (~3.7.0): Unified networking abstraction layer.
* **UI & Graphics**:
  * `lvgl/lvgl` (~9.5.0) & `esp_lvgl_port` (~2.9.0): Embedded graphics library.
  * `78/xiaozhi-fonts` (~2.0.0): Built-in fonts and Material Symbols glyphs.
  * `espressif/esp_image_effects` (^1.1.0): Display color formatting and transforms.
  * `espressif2022/esp_emote_expression` (^1.0.2): Embedded QR code generation.
* **Audio & Peripherals**:
  * `espressif/esp_codec_dev` (~1.6.2) & `espressif/esp_audio_codec` (~2.5.0): ES8311 driver abstraction.
  * `espressif/esp_audio_effects` (~1.3.0): Digital gain and audio processing.
  * `espressif/button` (~4.2.1): ADC ladder button event dispatcher.
  * `espressif/esp_mmap_assets` (^1.4.0): Memory-mapped flash assets.

---

## ⚡ Technical Optimization Highlights

| Domain | Optimization Detail | Concrete Benefit |
| :--- | :--- | :--- |
| **Binary Size** | Compact partition layout: `assets`(1.5M) -> `ota_0`(3.0M) -> `ota_1`(3.0M) | Wiped 3.75MB 0xFF gap; binary shrank from **7.26MB to 3.87MB (-46.6%)** |
| **SRAM Resilience** | Enabled `CONFIG_MBEDTLS_DYNAMIC_FREE_PEER_CERT=y` | Frees peer certificates post-handshake, saving **15KB~20KB** SRAM |
| **Audio Anti-Pop** | Software volume zeroing before ES8311 device closure | Eliminates transient DC offset click/pop noises on speaker stop |
| **Button Debounce** | 40mV hysteresis deadbands on GPIO0 ladder | Immune to thermal drift and supply ripple key bounce |
| **Standby Power** | 10s I2C battery throttle + 200ms adaptive PWM fade + 90s screen sleep | Reduces I2C wakeups by >90%; significantly extends battery lifespan |

*Detailed engineering logs: [docs/OPTIMIZATION_LOG.md](docs/OPTIMIZATION_LOG.md)*.

---

## 🚀 Firmware Downloads & Flashing

### 1. Download
Download the latest merged single-file binary from [GitHub Releases](https://github.com/Level6me/Letgo/releases):
* Binary name: `FoloToy-AI-Passport-full.bin`
* Flash offset: `0x00000000` (all-in-one merged binary containing Bootloader, Partition Table, Assets, and Application).

### 2. Flashing

#### Option A: Web Browser Flashing (Recommended)
1. Open [ESP Web Flasher](https://espressif.github.io/esptool-js/) in Chrome or Edge.
2. Connect Passport via USB Type-C (hold OK button while plugging in for download mode).
3. Select `FoloToy-AI-Passport-full.bin` at offset `0x0`, click **Program**.

#### Option B: esptool Command Line
```bash
esptool.py -p /dev/ttyACM0 -b 921600 --chip esp32c3 write_flash 0x0 FoloToy-AI-Passport-full.bin
```

---

## 📶 Quick Setup Guide

1. **Wi-Fi Provisioning**:
   * Long-press ▼ on Passport to start the configuration AP (`Passport-XXXX`).
   * Connect with your phone or PC, navigate to `192.168.4.1`, and submit your Wi-Fi credentials.
2. **Connecting to Feishu Gateway**:
   * Ensure `antigravity-feishu-bot` is running on your local network.
   * Passport will auto-discover the gateway. If multiple gateways exist, select with ▲/▼ and confirm with OK.
   * Once `[已连接]` is shown, tap or hold OK to start voice conversation!

---

## 📄 License

Core application logic is licensed under the MIT License. Embedded third-party components and drivers are copyright of their respective owners.
