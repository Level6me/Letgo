#include "wifi_board.h"
#include "wifi_manager.h"
#include "display/lcd_display.h"
#include "codecs/es8311_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "assets/lang_config.h"
#include "cw2017_battery_monitor.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <button_adc.h>
#include <esp_adc/adc_oneshot.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>

#define TAG "AiPassport"

// Physical keys share one ADC pin through a resistor ladder (see config.h).
enum {
    kAdcButtonUp = 0,
    kAdcButtonDown,
    kAdcButtonOk,
    kAdcButtonNum,
};

class AiPassportBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    Button* adc_button_[kAdcButtonNum];
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    LcdDisplay* display_;
    Cw2017BatteryMonitor* battery_;
    int64_t last_activity_time_ = 0;
    uint8_t current_brightness_ = 100;
    esp_timer_handle_t dim_timer_ = nullptr;
    bool is_push_to_talk_active_ = false;
    int64_t record_start_time_ = 0;
    esp_timer_handle_t record_timer_ = nullptr;
    bool is_in_standby_clock_ = false;
    uint32_t wave_frame_ = 0;

    bool CheckAndAbortSpeaking() {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateSpeaking) {
            ESP_LOGI(TAG, "Barge-in triggered by physical key! Aborting speech...");
            app.AbortSpeaking(kAbortReasonNone);
            app.GetAudioService().ResetDecoder();
            if (GetDisplay()) {
                GetDisplay()->ShowNotification("⏹️ 语音播报已打断", 1200);
                GetDisplay()->SetStatus("⏹️ 已打断");
                GetDisplay()->SetEmotion("neutral");
            }
            return true;
        }
        return false;
    }

    void EnterOrRefreshStandbyClock() {
        auto display = GetDisplay();
        if (!display) return;
        is_in_standby_clock_ = true;

        time_t now = time(NULL);
        struct tm* tm_now = localtime(&now);

        char time_buf[32];
        char date_buf[64];
        if (tm_now && tm_now->tm_year >= 2025 - 1900) {
            strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm_now);
            static const char* kWeekDays[] = {"日", "一", "二", "三", "四", "五", "六"};
            snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d 星期%s",
                     tm_now->tm_year + 1900, tm_now->tm_mon + 1, tm_now->tm_mday,
                     kWeekDays[tm_now->tm_wday % 7]);
        } else {
            int uptime_sec = (int)(esp_timer_get_time() / 1000000);
            snprintf(time_buf, sizeof(time_buf), "运行 %02d:%02d:%02d",
                     uptime_sec / 3600, (uptime_sec % 3600) / 60, uptime_sec % 60);
            snprintf(date_buf, sizeof(date_buf), "设备待命");
        }

        int battery_level = -1;
        int battery_mv = -1;
        if (battery_ && battery_->IsPresent()) {
            battery_level = battery_->GetBatteryLevel();
            battery_mv = battery_->GetBatteryVoltageMv();
        }

        auto& app = Application::GetInstance();
        bool feishu_online = app.IsFeishuConnected();
        std::string gw = app.GetFeishuGatewayIp();

        char dashboard_buf[192];
        if (battery_level >= 0) {
            snprintf(dashboard_buf, sizeof(dashboard_buf),
                     "🕒 %s\n📅 %s\n🔋 电量: %d%% (%d mV)\n%s",
                     time_buf, date_buf, battery_level, battery_mv,
                     feishu_online ? ("🟢 控制台: " + gw).c_str() : "⚪ 控制台: 离线 [双击上键搜索]");
        } else {
            snprintf(dashboard_buf, sizeof(dashboard_buf),
                     "🕒 %s\n📅 %s\n%s",
                     time_buf, date_buf,
                     feishu_online ? ("🟢 控制台: " + gw).c_str() : "⚪ 控制台: 离线 [双击上键搜索]");
        }

        display->SetChatMessage("system", dashboard_buf);
        display->SetStatus("🕒 随身时钟看板");
        display->SetEmotion("neutral");
    }

    void ExitStandbyClock() {
        if (!is_in_standby_clock_) return;
        is_in_standby_clock_ = false;
        auto display = GetDisplay();
        if (display) {
            auto& app = Application::GetInstance();
            display->SetChatMessage("system", app.IsFeishuConnected() ? Lang::Strings::FEISHU_HOLD_OK_TALK : Lang::Strings::STANDBY);
            display->SetStatus(app.IsFeishuConnected() ? Lang::Strings::FEISHU_CONSOLE_READY : Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
        }
    }

    void StartRecordTimer() {
        if (!record_timer_) {
            esp_timer_create_args_t timer_args = {
                .callback = [](void* arg) {
                    auto self = static_cast<AiPassportBoard*>(arg);
                    if (!self->is_push_to_talk_active_) {
                        return;
                    }
                    self->wave_frame_++;

                    // 状态栏每 5 帧(~250ms)刷新一次跳跃式波形字符，保证极致顺滑
                    if (self->wave_frame_ % 5 == 0) {
                        int elapsed_sec = (int)((esp_timer_get_time() - self->record_start_time_) / 1000000);
                        Application::GetInstance().Schedule([self, elapsed_sec]() {
                            if (!self->is_push_to_talk_active_) {
                                return;
                            }
                            if (elapsed_sec >= 60) {
                                self->StopPushToTalk(elapsed_sec);
                                return;
                            }
                            auto display = self->GetDisplay();
                            if (display) {
                                static const char* kWaveBars[] = {
                                    " ▂▃▅▆▇▆▅▃▂ ",
                                    "▂▃▅▆▇█▇▆▅▃ ",
                                    "▃▅▆▇█▇▆▅▃▂ ",
                                    "▅▆▇█▇▆▅▃▂  ",
                                    "▆▇█▇▆▅▃▂ ▂▃",
                                    "▇█▇▆▅▃▂ ▂▃▅",
                                    "█▇▆▅▃▂ ▂▃▅▆",
                                    "▇▆▅▃▂ ▂▃▅▆▇",
                                    "▆▅▃▂ ▂▃▅▆▇█",
                                    "▅▃▂ ▂▃▅▆▇█▇",
                                    "▃▂ ▂▃▅▆▇█▇▆",
                                    "▂ ▂▃▅▆▇█▇▆▅",
                                };
                                const char* wave = kWaveBars[self->wave_frame_ % 12];

                                char status_buf[64];
                                snprintf(status_buf, sizeof(status_buf), "🎙️ %02d:%02d [%s]",
                                         elapsed_sec / 60, elapsed_sec % 60, wave);
                                display->SetStatus(status_buf);
                                display->SetEmotion("listening");
                            }
                        });
                    }
                },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "record_timer"
            };
            esp_timer_create(&timer_args, &record_timer_);
        }
        // 50ms 周期（20Hz 刷新率），在 ESP32-S3 SPI 传输与 CPU 负荷之间取得最佳平衡
        esp_timer_start_periodic(record_timer_, 50000);
    }

    void StopRecordTimer() {
        if (record_timer_) {
            esp_timer_stop(record_timer_);
        }
    }

    void StopPushToTalk(int duration_sec) {
        if (!is_push_to_talk_active_) {
            return;
        }
        is_push_to_talk_active_ = false;
        StopRecordTimer();

        auto& app = Application::GetInstance();
        auto display = GetDisplay();

        if (duration_sec < 1) {
            if (app.GetDeviceState() == kDeviceStateListening) {
                app.StopListening();
            }
            if (display) {
                display->ShowNotification("⚠️ 录音时间太短(<1秒)\n已取消发送", 2000);
                display->SetStatus(app.IsFeishuConnected() ? Lang::Strings::FEISHU_CONSOLE_READY : Lang::Strings::STANDBY);
                display->SetEmotion("neutral");
                if (app.IsFeishuConnected()) {
                    display->SetChatMessage("system", Lang::Strings::FEISHU_HOLD_OK_TALK);
                }
            }
            return;
        }

        app.SetFeishuAwaitingReply(true);
        if (app.GetDeviceState() == kDeviceStateListening) {
            app.StopListening();
        }

        if (display) {
            char tip[80];
            snprintf(tip, sizeof(tip), "📤 录音完成 (%d秒)\n%s", duration_sec, Lang::Strings::FEISHU_SENDING);
            display->ShowNotification(tip, 3500);
            display->SetStatus(Lang::Strings::FEISHU_SENDING);
            display->SetEmotion("thinking");
        }
    }

    void InitializeCodecI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));

        // CW2017 fuel gauge is optional; a missing chip just disables battery UI.
        battery_ = new Cw2017BatteryMonitor(codec_i2c_bus_, BATTERY_CW2017_ADDR);
        battery_->Initialize();
    }

    void ChangeVolume(int delta) {
        auto codec = GetAudioCodec();
        auto volume = codec->output_volume() + delta;
        if (volume > 100) {
            volume = 100;
        }
        if (volume < 0) {
            volume = 0;
        }
        codec->SetOutputVolume(volume);

        // 可视化音量胶囊 HUD 进度槽
        int filled = (volume + 5) / 10;
        if (filled > 10) filled = 10;
        std::string hud = "🔊 音量: " + std::to_string(volume) + "%\n[";
        for (int i = 0; i < 10; i++) {
            hud += (i < filled) ? "■" : "·";
        }
        hud += "]";
        if (GetDisplay()) {
            GetDisplay()->ShowNotification(hud.c_str(), 1600);
        }
    }

    void ToggleChat() {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateStarting) {
            EnterWifiConfigMode();
            return;
        }
        app.ToggleChatState();
    }

    void TouchActivity(const char* btn_name = nullptr) {
        last_activity_time_ = esp_timer_get_time();
        if (current_brightness_ < 100) {
            current_brightness_ = 100;
            GetBacklight()->SetBrightness(100);
        }
        if (is_in_standby_clock_) {
            ExitStandbyClock();
        }
        if (btn_name) {
            ESP_LOGD(TAG, "TouchActivity triggered by button: %s", btn_name);
        }
    }

    void InitializeButtons() {
        for (int i = 0; i < kAdcButtonNum; i++) {
            adc_button_[i] = nullptr;
        }

        // One ADC1 unit shared by all three ladder keys. AdcButton reuses the
        // handle when adc_config.adc_handle is non-null, so the same physical
        // pin can decode several keys without "adc1 is already in use".
        adc_oneshot_unit_init_cfg_t init_cfg = {
            .unit_id = ADC_UNIT_1,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &adc_handle_));

        button_adc_config_t adc_cfg = {};
        adc_cfg.adc_handle = &adc_handle_;
        adc_cfg.unit_id = ADC_UNIT_1;
        adc_cfg.adc_channel = ADC_CHANNEL_0;  // GPIO0

        adc_cfg.button_index = kAdcButtonUp;      // UP:   ~0 mV
        adc_cfg.min = BSP_ADC_BUTTON_UP_MIN;
        adc_cfg.max = BSP_ADC_BUTTON_UP_MAX;
        adc_button_[kAdcButtonUp] = new AdcButton(adc_cfg, 1500);

        adc_cfg.button_index = kAdcButtonDown;    // DOWN: ~300 mV
        adc_cfg.min = BSP_ADC_BUTTON_DOWN_MIN;
        adc_cfg.max = BSP_ADC_BUTTON_DOWN_MAX;
        adc_button_[kAdcButtonDown] = new AdcButton(adc_cfg, 1500);

        adc_cfg.button_index = kAdcButtonOk;      // OK:   ~595 mV
        adc_cfg.min = BSP_ADC_BUTTON_OK_MIN;
        adc_cfg.max = BSP_ADC_BUTTON_OK_MAX;
        adc_button_[kAdcButtonOk] = new AdcButton(adc_cfg, 600);

        // 按钮事件绑定与交互优化
        auto up = adc_button_[kAdcButtonUp];
        up->OnClick([this]() {
            TouchActivity("UP");
            Application::GetInstance().Schedule([this]() {
                if (CheckAndAbortSpeaking()) {
                    return;
                }
                ChangeVolume(+10);
                auto& app = Application::GetInstance();
                if (app.IsFeishuConnected()) {
                    app.SendFeishuButtonEvent("up", "short_press");
                }
            });
        });
        up->OnDoubleClick([this]() {
            TouchActivity("UP_DOUBLE");
            Application::GetInstance().Schedule([this]() {
                auto& app = Application::GetInstance();
                if (!app.IsFeishuConnected()) {
                    GetDisplay()->ShowNotification(Lang::Strings::FEISHU_SEARCH_NOTICE, 3000);
                    GetDisplay()->SetStatus(Lang::Strings::FEISHU_SEARCHING);
                    app.TriggerFeishuDiscovery();
                } else {
                    GetDisplay()->ShowNotification("🔄 刷新看板与控制台数据...", 2000);
                    app.SendFeishuButtonEvent("up", "double_click");
                }
            });
        });
        up->OnLongPress([this]() {
            TouchActivity("UP_LONG");
            Application::GetInstance().Schedule([this]() {
                GetDisplay()->ShowNotification("📶 进入热点配网模式...", 3000);
                EnterWifiConfigMode();
            });
        });

        auto down = adc_button_[kAdcButtonDown];
        down->OnClick([this]() {
            TouchActivity("DOWN");
            Application::GetInstance().Schedule([this]() {
                if (CheckAndAbortSpeaking()) {
                    return;
                }
                ChangeVolume(-10);
                auto& app = Application::GetInstance();
                if (app.IsFeishuConnected()) {
                    app.SendFeishuButtonEvent("down", "short_press");
                }
            });
        });
        down->OnDoubleClick([this]() {
            TouchActivity("DOWN_DOUBLE");
            Application::GetInstance().Schedule([this]() {
                auto& app = Application::GetInstance();
                auto& wifi = WifiManager::GetInstance();
                std::string ip = wifi.GetIpAddress();
                if (ip.empty()) {
                    ip = "未分配";
                }
                bool connected = app.IsFeishuConnected();
                std::string gw = app.GetFeishuGatewayIp();

                int battery_level = -1;
                int battery_mv = -1;
                if (battery_ && battery_->IsPresent()) {
                    battery_level = battery_->GetBatteryLevel();
                    battery_mv = battery_->GetBatteryVoltageMv();
                }

                std::string info = "📶 本机IP: " + ip;
                if (connected) {
                    info += "\n🟢 控制台: 已连接 (" + gw + ")";
                } else {
                    info += "\n⚪ 控制台: 未连接 [双击上键搜索]";
                }
                if (battery_level >= 0) {
                    info += "\n🔋 电量: " + std::to_string(battery_level) + "% (" + std::to_string(battery_mv) + "mV)";
                }
                GetDisplay()->ShowNotification(info.c_str(), 4500);
                app.SendFeishuButtonEvent("down", "double_click");
            });
        });
        down->OnLongPress([this]() {
            TouchActivity("DOWN_LONG");
            Application::GetInstance().Schedule([]() {
                ESP_LOGW(TAG, "Physical Emergency Stop Triggered!");
                Application::GetInstance().SendFeishuButtonEvent("down", "long_press");
                Application::GetInstance().Alert("EMERGENCY", "Physical Stop Sent!\nAborted all tasks.", "danger", Lang::Sounds::OGG_EXCLAMATION);
            });
        });

        auto ok = adc_button_[kAdcButtonOk];
        ok->OnClick([this]() {
            TouchActivity("OK");
            if (is_push_to_talk_active_) {
                return;
            }
            Application::GetInstance().Schedule([this]() {
                if (CheckAndAbortSpeaking()) {
                    return;
                }
                auto& app = Application::GetInstance();
                if (app.HasPendingFeishuGateway()) {
                    app.ConnectSelectedFeishuGateway();
                } else {
                    ToggleChat();
                }
            });
        });
        ok->OnLongPress([this]() {
            TouchActivity("OK_LONG");
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateSpeaking) {
                app.AbortSpeaking(kAbortReasonNone);
                app.GetAudioService().ResetDecoder();
            }
            is_push_to_talk_active_ = true;
            wave_frame_ = 0;
            record_start_time_ = esp_timer_get_time();

            Application::GetInstance().Schedule([this]() {
                auto& app = Application::GetInstance();
                app.StartListening();
                if (GetDisplay()) {
                    char initial_buf[64];
                    snprintf(initial_buf, sizeof(initial_buf), "🎙️ 00:00 [ ▂▃▅▆▇▆▅▃▂ ]");
                    GetDisplay()->SetStatus(initial_buf);
                    GetDisplay()->SetEmotion("listening");

                    GetDisplay()->ShowNotification("🎙️ 按住说话...\n[ ▂▃▅▆▇▆▅▃▂ ]\n松开按键发送", 1200);
                }
                StartRecordTimer();
            });
        });
        ok->OnPressUp([this]() {
            TouchActivity("OK_UP");
            if (is_push_to_talk_active_) {
                int duration_sec = (int)((esp_timer_get_time() - record_start_time_) / 1000000);
                Application::GetInstance().Schedule([this, duration_sec]() {
                    StopPushToTalk(duration_sec);
                });
            }
        });
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SPI_SCK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_SPI_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;  // -1 -> software reset
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);

        // Panel-specific power/porch/gamma sequence for the ST7789P3 module
        // used on the Passport (copied from the original badge firmware).
        static const struct {
            uint8_t command;
            uint8_t data[16];
            uint8_t data_length;
            uint16_t delay_ms;
        } kSt7789P3InitCommands[] = {
            {0xB2, {0x05, 0x05, 0x00, 0x33, 0x33}, 5, 0},
            {0xB7, {0x35}, 1, 0},
            {0xBB, {0x21}, 1, 0},
            {0xC0, {0x2C}, 1, 0},
            {0xC2, {0x01}, 1, 0},
            {0xC3, {0x0B}, 1, 0},
            {0xC4, {0x20}, 1, 0},
            {0xC6, {0x0F}, 1, 0},
            {0xD0, {0xA7, 0xA1}, 2, 0},
            {0xD0, {0xA4, 0xA1}, 2, 0},
            {0xD6, {0xA1}, 1, 0},
            {0xE0, {0xD0, 0x04, 0x08, 0x0A, 0x09, 0x05, 0x2D, 0x43,
                    0x49, 0x09, 0x16, 0x15, 0x26, 0x2B}, 14, 0},
            {0xE1, {0xD0, 0x03, 0x09, 0x0A, 0x0A, 0x06, 0x2E, 0x44,
                    0x40, 0x3A, 0x15, 0x15, 0x26, 0x2A}, 14, 10},
        };
        for (const auto& cmd : kSt7789P3InitCommands) {
            esp_lcd_panel_io_tx_param(panel_io, cmd.command, cmd.data, cmd.data_length);
            if (cmd.delay_ms > 0) {
                vTaskDelay(pdMS_TO_TICKS(cmd.delay_ms));
            }
        }

        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_set_gap(panel, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);

        display_ = new SpiLcdDisplay(panel_io, panel,
                                     DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

public:
    AiPassportBoard() : display_(nullptr), battery_(nullptr) {
        InitializeCodecI2c();
        InitializeSpi();
        InitializeDisplay();
        InitializeButtons();
        GetBacklight()->RestoreBrightness();

        last_activity_time_ = esp_timer_get_time();
        esp_timer_create_args_t dim_timer_args = {
            .callback = [](void* arg) {
                auto self = static_cast<AiPassportBoard*>(arg);
                int64_t now = esp_timer_get_time();
                auto state = Application::GetInstance().GetDeviceState();

                // 超过 15 秒无操作且处于待命状态，自动进入/刷新随身时钟看板
                if (now - self->last_activity_time_ >= 15 * 1000000LL &&
                    (state == kDeviceStateIdle || state == kDeviceStateStarting) &&
                    !self->is_push_to_talk_active_) {
                    Application::GetInstance().Schedule([self]() {
                        self->EnterOrRefreshStandbyClock();
                    });

                    // 超过 30 秒无操作，降低屏幕亮度至 20% 省电休眠
                    if (now - self->last_activity_time_ >= 30 * 1000000LL) {
                        if (self->current_brightness_ > 20) {
                            self->current_brightness_ = 20;
                            self->GetBacklight()->SetBrightness(20);
                            ESP_LOGI(TAG, "Screen dimmed to 20%% due to 30s inactivity");
                        }
                    }
                }
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "dim_timer",
            .skip_unhandled_events = true
        };
        esp_timer_create(&dim_timer_args, &dim_timer_);
        esp_timer_start_periodic(dim_timer_, 2000000);
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            codec_i2c_bus_,
            I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (!battery_ || !battery_->IsPresent()) {
            return false;
        }
        int soc = battery_->GetBatteryLevel();
        if (soc < 0) {
            return false;
        }
        level = soc;
        // CW2017 reports no charge state and the Passport has no charge-detect
        // GPIO, so report a plain (discharging) reading.
        charging = false;
        discharging = true;
        return true;
    }
};

DECLARE_BOARD(AiPassportBoard);
