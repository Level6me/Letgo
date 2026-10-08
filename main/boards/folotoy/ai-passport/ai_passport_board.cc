#include "wifi_board.h"
#include "wifi_manager.h"
#include "display/lcd_display.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "codecs/es8311_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "assets/lang_config.h"
#include "cw2017_battery_monitor.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <button_adc.h>
#include <esp_adc/adc_oneshot.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <material_symbols.h>
#include <time.h>
#include <cmath>
#include <atomic>

#define TAG "AiPassport"

// Physical keys share one ADC pin through a resistor ladder (see config.h).
enum {
    kAdcButtonUp = 0,
    kAdcButtonDown,
    kAdcButtonOk,
    kAdcButtonNum,
};

/**
 * @brief 专属极简纯黑白高对比度显示驱动
 *
 * 核心规范：
 * 1. 屏幕正中间平时只有一根纯白水平直线 (140px宽, 2px高)；
 * 2. 录音和播放器播放时变成随真实声音能量流动的纯白动态频谱波浪 (LVGL原生绘制，零字体依赖)；
 * 3. 彻底移除一切彩色 Emoji、彩色状态点、彩色气泡，全界面纯黑白单色设计；
 * 4. 音量调节在屏幕底部显示居中胶囊 HUD (喇叭图标 + 百分比)。
 */
class AiPassportDisplay : public SpiLcdDisplay {
private:
    lv_obj_t* center_line_obj_ = nullptr;
    lv_obj_t* wave_container_ = nullptr;
    static constexpr int kWaveBarsCount = 15;
    lv_obj_t* wave_bars_[kWaveBarsCount] = {nullptr};

    // 屏幕底部音量胶囊 HUD
    lv_obj_t* bottom_volume_box_ = nullptr;
    lv_obj_t* volume_icon_label_ = nullptr;
    lv_obj_t* volume_text_label_ = nullptr;
    esp_timer_handle_t volume_hide_timer_ = nullptr;

    // 律动动画与状态
    lv_timer_t* wave_timer_ = nullptr;
    uint32_t wave_frame_ = 0;
    bool was_active_wave_ = false;
    std::atomic<bool> is_recording_ptt_{false};

    void RenderIdleLine() {
        if (wave_container_) {
            lv_obj_add_flag(wave_container_, LV_OBJ_FLAG_HIDDEN);
        }
        if (center_line_obj_) {
            lv_obj_remove_flag(center_line_obj_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    void RenderWaveBars(uint16_t energy_rms, uint32_t frame) {
        if (center_line_obj_) {
            lv_obj_add_flag(center_line_obj_, LV_OBJ_FLAG_HIDDEN);
        }
        if (wave_container_) {
            lv_obj_remove_flag(wave_container_, LV_OBJ_FLAG_HIDDEN);
        }

        // 计算声音能量活跃度因子 (0.0 ~ 1.0)
        float factor = 0.0f;
        if (energy_rms > 100) {
            factor = (float)(energy_rms - 100) / 2500.0f;
            if (factor > 1.0f) factor = 1.0f;
        }
        // 录音或播放中若暂无明显声响，保持基础 15% 呼吸微波，提示处于工作收听/播放状态
        float eff_factor = (factor < 0.15f) ? 0.15f : factor;
        float phase = (float)frame * 0.35f;

        for (int i = 0; i < kWaveBarsCount; i++) {
            float d = fabsf((float)i - 7.0f) / 7.0f;
            float window = cosf(d * 1.25f);
            if (window < 0.2f) window = 0.2f;

            float wave1 = sinf(phase + (float)i * 0.75f);
            float wave2 = cosf(phase * 1.4f - (float)i * 0.5f);
            float m = 0.5f + 0.35f * wave1 + 0.15f * wave2;

            int h = 3 + (int)(eff_factor * window * m * 28.0f);
            if (h < 3) h = 3;
            if (h > 32) h = 32;

            if (wave_bars_[i]) {
                lv_obj_set_height(wave_bars_[i], h);
            }
        }
    }

public:
    AiPassportDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                      int width, int height, int offset_x, int offset_y,
                      bool mirror_x, bool mirror_y, bool swap_xy)
        : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {}

    virtual ~AiPassportDisplay() {
        if (volume_hide_timer_) {
            esp_timer_stop(volume_hide_timer_);
            esp_timer_delete(volume_hide_timer_);
            volume_hide_timer_ = nullptr;
        }
        if (wave_timer_) {
            lv_timer_delete(wave_timer_);
            wave_timer_ = nullptr;
        }
    }

    virtual void SetupUI() override {
        SpiLcdDisplay::SetupUI();

        DisplayLockGuard lock(this);
        auto screen = lv_screen_active();

        // 1. 强制应用纯黑白两色高对比度极简主题 (黑底白字，去除一切彩色气泡)
        auto dark_theme = LvglThemeManager::GetInstance().GetTheme("dark");
        if (dark_theme) {
            dark_theme->set_background_color(lv_color_hex(0x000000));
            dark_theme->set_text_color(lv_color_hex(0xFFFFFF));
            dark_theme->set_chat_background_color(lv_color_hex(0x000000));
            dark_theme->set_user_bubble_color(lv_color_hex(0x000000));
            dark_theme->set_assistant_bubble_color(lv_color_hex(0x000000));
            dark_theme->set_system_bubble_color(lv_color_hex(0x000000));
            dark_theme->set_system_text_color(lv_color_hex(0xFFFFFF));
            dark_theme->set_border_color(lv_color_hex(0xFFFFFF));
            dark_theme->set_low_battery_color(lv_color_hex(0xFFFFFF));
            SetTheme(dark_theme);
        }

        // 2. 彻底隐藏默认表情包与机器人头像
        if (emoji_label_) {
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emoji_image_) {
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        }

        // 3. 屏幕正中间平时展示的纯白水平直线 (宽140px, 高2px, 居中对齐)
        center_line_obj_ = lv_obj_create(screen);
        lv_obj_set_size(center_line_obj_, 140, 2);
        lv_obj_set_style_bg_color(center_line_obj_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(center_line_obj_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(center_line_obj_, 0, 0);
        lv_obj_set_style_radius(center_line_obj_, 1, 0);
        lv_obj_set_scrollbar_mode(center_line_obj_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_align(center_line_obj_, LV_ALIGN_CENTER, 0, 0);

        // 4. 屏幕正中间动态波浪条容器 (录音与播放时动态显示，摆脱字体依赖，100% 纯白像素呈现)
        wave_container_ = lv_obj_create(screen);
        lv_obj_set_size(wave_container_, 140, 40);
        lv_obj_set_style_bg_opa(wave_container_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(wave_container_, 0, 0);
        lv_obj_set_style_pad_all(wave_container_, 0, 0);
        lv_obj_set_scrollbar_mode(wave_container_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(wave_container_, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(wave_container_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_align(wave_container_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(wave_container_, LV_OBJ_FLAG_HIDDEN);

        for (int i = 0; i < kWaveBarsCount; i++) {
            wave_bars_[i] = lv_obj_create(wave_container_);
            lv_obj_set_size(wave_bars_[i], 6, 2);
            lv_obj_set_style_bg_color(wave_bars_[i], lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(wave_bars_[i], LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(wave_bars_[i], 0, 0);
            lv_obj_set_style_radius(wave_bars_[i], 2, 0);
            lv_obj_set_style_pad_all(wave_bars_[i], 0, 0);
            if (i > 0) {
                lv_obj_set_style_margin_left(wave_bars_[i], 3, 0);
            }
        }

        // 5. 屏幕底部音量胶囊 HUD (黑底白边，内含喇叭图标与音量百分比)
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        const lv_font_t* text_font = lvgl_theme && lvgl_theme->text_font() ? lvgl_theme->text_font()->font() : nullptr;
        const lv_font_t* icon_font = lvgl_theme && lvgl_theme->icon_font() ? lvgl_theme->icon_font()->font() : nullptr;

        bottom_volume_box_ = lv_obj_create(screen);
        lv_obj_set_size(bottom_volume_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(bottom_volume_box_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(bottom_volume_box_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(bottom_volume_box_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(bottom_volume_box_, 1, 0);
        lv_obj_set_style_radius(bottom_volume_box_, 14, 0);
        lv_obj_set_style_pad_top(bottom_volume_box_, 4, 0);
        lv_obj_set_style_pad_bottom(bottom_volume_box_, 4, 0);
        lv_obj_set_style_pad_left(bottom_volume_box_, 12, 0);
        lv_obj_set_style_pad_right(bottom_volume_box_, 12, 0);
        lv_obj_set_flex_flow(bottom_volume_box_, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(bottom_volume_box_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(bottom_volume_box_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_align(bottom_volume_box_, LV_ALIGN_BOTTOM_MID, 0, -16);
        lv_obj_add_flag(bottom_volume_box_, LV_OBJ_FLAG_HIDDEN);

        volume_icon_label_ = lv_label_create(bottom_volume_box_);
        if (icon_font) {
            lv_obj_set_style_text_font(volume_icon_label_, icon_font, 0);
        }
        lv_obj_set_style_text_color(volume_icon_label_, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(volume_icon_label_, MATERIAL_SYMBOLS_VOLUME_UP);

        volume_text_label_ = lv_label_create(bottom_volume_box_);
        if (text_font) {
            lv_obj_set_style_text_font(volume_text_label_, text_font, 0);
        }
        lv_obj_set_style_text_color(volume_text_label_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_margin_left(volume_text_label_, 6, 0);
        lv_label_set_text(volume_text_label_, "100%");

        // 6. 音量 HUD 自动隐藏定时器 (1.5秒后自动隐退)
        if (!volume_hide_timer_) {
            esp_timer_create_args_t vol_timer_args = {
                .callback = [](void* arg) {
                    auto self = static_cast<AiPassportDisplay*>(arg);
                    DisplayLockGuard lock(self);
                    if (self->bottom_volume_box_) {
                        lv_obj_add_flag(self->bottom_volume_box_, LV_OBJ_FLAG_HIDDEN);
                    }
                },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "vol_hide_timer",
                .skip_unhandled_events = true
            };
            esp_timer_create(&vol_timer_args, &volume_hide_timer_);
        }

        // 7. 动态波浪线 20Hz 刷新定时器 (运行在 LVGL 线程中，完全无锁安全)
        if (!wave_timer_) {
            wave_timer_ = lv_timer_create([](lv_timer_t* timer) {
                auto self = static_cast<AiPassportDisplay*>(lv_timer_get_user_data(timer));
                self->OnWaveTimerTick();
            }, 50, this);
        }

        // 8. 确保底部栏默认隐藏，保持纯粹极简
        if (bottom_bar_) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    void OnWaveTimerTick() {
        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();
        auto& audio_service = app.GetAudioService();

        bool is_recording = is_recording_ptt_.load(std::memory_order_relaxed) ||
                            (state == kDeviceStateListening);
        bool is_playing = (state == kDeviceStateSpeaking) || (!audio_service.IsPlaybackIdle());

        if (is_recording) {
            wave_frame_++;
            uint16_t rms = audio_service.GetInputEnergyRms();
            RenderWaveBars(rms, wave_frame_);
            was_active_wave_ = true;
        } else if (is_playing) {
            wave_frame_++;
            uint16_t rms = audio_service.GetOutputEnergyRms();
            RenderWaveBars(rms, wave_frame_);
            was_active_wave_ = true;
        } else {
            if (was_active_wave_) {
                was_active_wave_ = false;
                RenderIdleLine();
            }
        }
    }

    void ShowVolumeBottom(int volume) {
        DisplayLockGuard lock(this);
        if (!bottom_volume_box_) return;

        if (volume <= 0) {
            lv_label_set_text(volume_icon_label_, MATERIAL_SYMBOLS_VOLUME_MUTE);
        } else if (volume < 50) {
            lv_label_set_text(volume_icon_label_, MATERIAL_SYMBOLS_VOLUME_DOWN);
        } else {
            lv_label_set_text(volume_icon_label_, MATERIAL_SYMBOLS_VOLUME_UP);
        }

        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", volume);
        lv_label_set_text(volume_text_label_, buf);

        lv_obj_remove_flag(bottom_volume_box_, LV_OBJ_FLAG_HIDDEN);

        if (volume_hide_timer_) {
            esp_timer_stop(volume_hide_timer_);
            esp_timer_start_once(volume_hide_timer_, 1500000);
        }
    }

    void SetRecording(bool recording) {
        is_recording_ptt_.store(recording, std::memory_order_relaxed);
    }

    void ShowIdleStraightLine() {
        DisplayLockGuard lock(this);
        was_active_wave_ = false;
        RenderIdleLine();
    }

    virtual void SetEmotion(const char* emotion) override {
        // 彻底屏蔽 Emoji 图标，屏幕正中只保持直线与流动波浪线
    }

    virtual void SetChatMessage(const char* role, const char* content) override {
        if (strcmp(role, "system") == 0) {
            ClearChatMessages();
            return;
        }
        SpiLcdDisplay::SetChatMessage(role, content);
    }

    virtual void SetTheme(Theme* theme) override {
        if (theme) {
            auto lvgl_theme = static_cast<LvglTheme*>(theme);
            lvgl_theme->set_background_color(lv_color_hex(0x000000));
            lvgl_theme->set_text_color(lv_color_hex(0xFFFFFF));
            lvgl_theme->set_chat_background_color(lv_color_hex(0x000000));
            lvgl_theme->set_user_bubble_color(lv_color_hex(0x000000));
            lvgl_theme->set_assistant_bubble_color(lv_color_hex(0x000000));
            lvgl_theme->set_system_bubble_color(lv_color_hex(0x000000));
            lvgl_theme->set_system_text_color(lv_color_hex(0xFFFFFF));
            lvgl_theme->set_border_color(lv_color_hex(0xFFFFFF));
            lvgl_theme->set_low_battery_color(lv_color_hex(0xFFFFFF));
        }
        SpiLcdDisplay::SetTheme(theme);
    }
};

class AiPassportBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    Button* adc_button_[kAdcButtonNum];
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    AiPassportDisplay* display_;
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
            if (display_) {
                display_->ShowNotification("[语音播报已打断]", 1200);
                display_->SetStatus("[已打断]");
                display_->ShowIdleStraightLine();
            }
            return true;
        }
        return false;
    }

    void EnterOrRefreshStandbyClock() {
        if (!display_) return;
        is_in_standby_clock_ = true;

        time_t now = time(NULL);
        struct tm* tm_now = localtime(&now);

        char time_buf[32];
        if (tm_now && tm_now->tm_year >= 2025 - 1900) {
            strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm_now);
        } else {
            int uptime_sec = (int)(esp_timer_get_time() / 1000000);
            snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d",
                     uptime_sec / 3600, (uptime_sec % 3600) / 60, uptime_sec % 60);
        }

        int battery_level = -1;
        if (battery_ && battery_->IsPresent()) {
            battery_level = battery_->GetBatteryLevel();
        }

        auto& app = Application::GetInstance();
        bool feishu_online = app.IsFeishuConnected();

        char status_buf[64];
        if (battery_level >= 0) {
            snprintf(status_buf, sizeof(status_buf), "%s | %s | %d%%",
                     time_buf, feishu_online ? "已连接" : "离线", battery_level);
        } else {
            snprintf(status_buf, sizeof(status_buf), "%s | %s",
                     time_buf, feishu_online ? "已连接" : "离线");
        }

        display_->SetStatus(status_buf);
        display_->ShowIdleStraightLine();
    }

    void ExitStandbyClock() {
        if (!is_in_standby_clock_) return;
        is_in_standby_clock_ = false;
        if (display_) {
            auto& app = Application::GetInstance();
            display_->ShowIdleStraightLine();
            display_->SetStatus(app.IsFeishuConnected() ? "[已连接]" : "[待命]");
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
                    int elapsed_sec = (int)((esp_timer_get_time() - self->record_start_time_) / 1000000);
                    Application::GetInstance().Schedule([self, elapsed_sec]() {
                        if (!self->is_push_to_talk_active_) {
                            return;
                        }
                        if (elapsed_sec >= 60) {
                            self->StopPushToTalk(elapsed_sec);
                            return;
                        }
                        if (self->display_) {
                            char status_buf[48];
                            snprintf(status_buf, sizeof(status_buf), "[录音中 %02d:%02d]",
                                     elapsed_sec / 60, elapsed_sec % 60);
                            self->display_->SetStatus(status_buf);
                        }
                    });
                },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "record_timer"
            };
            esp_timer_create(&timer_args, &record_timer_);
        }
        // 500ms 周期更新状态栏时间
        esp_timer_start_periodic(record_timer_, 500000);
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

        // 录音结束，立即恢复屏幕正中心水平直线
        if (display_) {
            display_->SetRecording(false);
            display_->ShowIdleStraightLine();
        }

        if (duration_sec < 1) {
            if (app.GetDeviceState() == kDeviceStateListening) {
                app.StopListening();
            }
            if (display_) {
                display_->ShowNotification("[录音时间太短(<1秒)]\n[已取消发送]", 2000);
                display_->SetStatus(app.IsFeishuConnected() ? "[已连接]" : "[待命]");
            }
            return;
        }

        app.SetFeishuAwaitingReply(true);
        if (app.GetDeviceState() == kDeviceStateListening) {
            app.StopListening();
        }

        if (display_) {
            char tip[64];
            snprintf(tip, sizeof(tip), "[录音完成 (%d秒)]\n[发送中...]", duration_sec);
            display_->ShowNotification(tip, 1200);
            display_->SetStatus("[发送中...]");
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

        if (display_) {
            display_->ShowVolumeBottom(volume);
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
                // 音量键专心调节音量，不中断正在播报的语音
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
                    if (display_) {
                        display_->ShowNotification("[正在搜索控制台网关...]", 3000);
                        display_->SetStatus("[搜索中...]");
                    }
                    app.TriggerFeishuDiscovery();
                } else {
                    if (display_) {
                        display_->ShowNotification("[正在刷新数据...]", 2000);
                    }
                    app.SendFeishuButtonEvent("up", "double_click");
                }
            });
        });
        up->OnLongPress([this]() {
            TouchActivity("UP_LONG");
            Application::GetInstance().Schedule([this]() {
                if (display_) {
                    display_->ShowNotification("[进入热点配网模式...]", 3000);
                }
                EnterWifiConfigMode();
            });
        });

        auto down = adc_button_[kAdcButtonDown];
        down->OnClick([this]() {
            TouchActivity("DOWN");
            Application::GetInstance().Schedule([this]() {
                // 音量键专心调节音量，不中断正在播报的语音
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

                std::string info = "IP: " + ip;
                if (connected) {
                    info += "\n控制台: 已连接 (" + gw + ")";
                } else {
                    info += "\n控制台: 未连接 [双击上键搜索]";
                }
                if (battery_level >= 0) {
                    info += "\n电量: " + std::to_string(battery_level) + "% (" + std::to_string(battery_mv) + "mV)";
                }
                if (display_) {
                    display_->ShowNotification(info.c_str(), 4500);
                }
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
                // OK 键作为主功能键，专职执行打断播报
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
        ok->OnDoubleClick([this]() {
            TouchActivity("OK_DOUBLE");
            Application::GetInstance().Schedule([this]() {
                auto& app = Application::GetInstance();
                if (CheckAndAbortSpeaking()) {
                    return;
                }
                if (display_) {
                    display_->ShowNotification("[正在请求重播上一句...]", 1500);
                }
                if (app.IsFeishuConnected()) {
                    app.SendFeishuButtonEvent("ok", "double_click");
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
            if (display_) {
                display_->SetRecording(true);
            }

            Application::GetInstance().Schedule([this]() {
                auto& app = Application::GetInstance();
                app.StartListening();
                if (display_) {
                    display_->SetStatus("[录音中 00:00]");
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

        display_ = new AiPassportDisplay(panel_io, panel,
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

        Settings settings("display", true);
        settings.SetString("theme", "dark");

        last_activity_time_ = esp_timer_get_time();
        esp_timer_create_args_t dim_timer_args = {
            .callback = [](void* arg) {
                auto self = static_cast<AiPassportBoard*>(arg);
                int64_t now = esp_timer_get_time();
                auto state = Application::GetInstance().GetDeviceState();

                // 若设备处于非空闲态（正在聆听/思考/播报），自动退出时钟看板恢复会话显示
                if (state != kDeviceStateIdle && state != kDeviceStateStarting) {
                    if (self->is_in_standby_clock_) {
                        self->TouchActivity();
                    }
                    return;
                }

                // 超过 15 秒无操作且处于待命状态，自动进入/刷新随身时钟看板
                if (now - self->last_activity_time_ >= 15 * 1000000LL &&
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
