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

    // 飞书网关配对选择弹窗 (极简单横线下划线选中指示)
    lv_obj_t* gateway_modal_ = nullptr;
    lv_obj_t* gateway_title_label_ = nullptr;
    lv_obj_t* gateway_sub_label_ = nullptr;
    lv_obj_t* gateway_sep_ = nullptr;
    lv_obj_t* gateway_list_box_ = nullptr;
    static constexpr size_t kMaxGatewayItems = 5;
    lv_obj_t* gateway_item_containers_[kMaxGatewayItems] = {nullptr};
    lv_obj_t* gateway_item_labels_[kMaxGatewayItems] = {nullptr};
    lv_obj_t* gateway_item_lines_[kMaxGatewayItems] = {nullptr};
    std::vector<FeishuGateway> cached_gateways_;
    int selected_gateway_index_ = 0;
    esp_timer_handle_t gateway_hide_timer_ = nullptr;

    // 项目切换弹窗 (极简单横线下划线选中指示)
    lv_obj_t* project_modal_ = nullptr;
    lv_obj_t* project_title_label_ = nullptr;
    lv_obj_t* project_sub_label_ = nullptr;
    lv_obj_t* project_sep_ = nullptr;
    lv_obj_t* project_list_box_ = nullptr;
    static constexpr size_t kMaxProjectItems = 6;
    lv_obj_t* project_item_containers_[kMaxProjectItems] = {nullptr};
    lv_obj_t* project_item_labels_[kMaxProjectItems] = {nullptr};
    lv_obj_t* project_item_lines_[kMaxProjectItems] = {nullptr};
    std::vector<std::string> cached_projects_;
    std::string current_project_name_;
    int selected_project_index_ = 0;
    esp_timer_handle_t project_hide_timer_ = nullptr;

    // 系统设置弹窗 (极简单横线下划线选中指示)
    lv_obj_t* settings_modal_ = nullptr;
    lv_obj_t* settings_title_label_ = nullptr;
    lv_obj_t* settings_sub_label_ = nullptr;
    lv_obj_t* settings_sep_ = nullptr;
    lv_obj_t* settings_list_box_ = nullptr;
    static constexpr size_t kMaxSettingsItems = 7;
    lv_obj_t* settings_item_containers_[kMaxSettingsItems] = {nullptr};
    lv_obj_t* settings_item_labels_[kMaxSettingsItems] = {nullptr};
    lv_obj_t* settings_item_lines_[kMaxSettingsItems] = {nullptr};
    int selected_settings_index_ = 0;
    esp_timer_handle_t settings_hide_timer_ = nullptr;

    // 主题反色状态 (false: 纯黑底白字; true: 纯白底黑字)
    bool is_theme_inverted_ = false;

    void RenderIdleLine() {
        if (wave_container_) {
            lv_obj_add_flag(wave_container_, LV_OBJ_FLAG_HIDDEN);
        }
        if (center_line_obj_) {
            lv_obj_remove_flag(center_line_obj_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    void RenderFftSpectrum(const uint8_t bands[kWaveBarsCount]) {
        if (center_line_obj_) {
            lv_obj_add_flag(center_line_obj_, LV_OBJ_FLAG_HIDDEN);
        }
        if (wave_container_) {
            lv_obj_remove_flag(wave_container_, LV_OBJ_FLAG_HIDDEN);
        }

        for (int i = 0; i < kWaveBarsCount; i++) {
            int h = bands[i];
            if (h < 2) h = 2;
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
        if (gateway_hide_timer_) {
            esp_timer_stop(gateway_hide_timer_);
            esp_timer_delete(gateway_hide_timer_);
            gateway_hide_timer_ = nullptr;
        }
        if (project_hide_timer_) {
            esp_timer_stop(project_hide_timer_);
            esp_timer_delete(project_hide_timer_);
            project_hide_timer_ = nullptr;
        }
        if (settings_hide_timer_) {
            esp_timer_stop(settings_hide_timer_);
            esp_timer_delete(settings_hide_timer_);
            settings_hide_timer_ = nullptr;
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

        // 4. 屏幕正中间动态波浪条容器 (录音与播放时动态显示真实 FFT 频谱)
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

        // 7. 状态栏与通知设置 (限制在 160px 内居中对齐，杜绝与左右两侧 WiFi 及电池图标重叠)
        if (status_bar_) {
            lv_obj_set_width(status_bar_, 160);
            lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 2);
        }
        if (status_label_) {
            if (text_font) lv_obj_set_style_text_font(status_label_, text_font, 0);
            lv_obj_set_width(status_label_, 160);
            lv_label_set_long_mode(status_label_, LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
        }
        if (notification_label_) {
            if (text_font) lv_obj_set_style_text_font(notification_label_, text_font, 0);
            lv_obj_set_width(notification_label_, 216);
            lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
        }

        // 8. 飞书网关配对选择弹窗 (宽228, 高260, 黑白高对比)
        gateway_modal_ = lv_obj_create(screen);
        lv_obj_set_size(gateway_modal_, 228, 260);
        lv_obj_align(gateway_modal_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(gateway_modal_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(gateway_modal_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(gateway_modal_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(gateway_modal_, 2, 0);
        lv_obj_set_style_radius(gateway_modal_, 8, 0);
        lv_obj_set_style_pad_all(gateway_modal_, 6, 0);
        lv_obj_set_flex_flow(gateway_modal_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(gateway_modal_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(gateway_modal_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_flag(gateway_modal_, LV_OBJ_FLAG_HIDDEN);

        gateway_title_label_ = lv_label_create(gateway_modal_);
        if (text_font) lv_obj_set_style_text_font(gateway_title_label_, text_font, 0);
        lv_obj_set_style_text_color(gateway_title_label_, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(gateway_title_label_, "[ Feishu Gateway ]");

        gateway_sub_label_ = lv_label_create(gateway_modal_);
        if (text_font) lv_obj_set_style_text_font(gateway_sub_label_, text_font, 0);
        lv_obj_set_style_text_color(gateway_sub_label_, lv_color_hex(0xAAAAAA), 0);
        lv_obj_set_width(gateway_sub_label_, 216);
        lv_label_set_long_mode(gateway_sub_label_, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(gateway_sub_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(gateway_sub_label_, "UP/DOWN: Move   OK: Connect");

        gateway_sep_ = lv_obj_create(gateway_modal_);
        lv_obj_set_size(gateway_sep_, 214, 1);
        lv_obj_set_style_bg_color(gateway_sep_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(gateway_sep_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(gateway_sep_, 0, 0);
        lv_obj_set_style_margin_top(gateway_sep_, 2, 0);
        lv_obj_set_style_margin_bottom(gateway_sep_, 4, 0);

        gateway_list_box_ = lv_obj_create(gateway_modal_);
        lv_obj_set_size(gateway_list_box_, 216, 176);
        lv_obj_set_style_bg_opa(gateway_list_box_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(gateway_list_box_, 0, 0);
        lv_obj_set_style_pad_all(gateway_list_box_, 0, 0);
        lv_obj_set_flex_flow(gateway_list_box_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(gateway_list_box_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(gateway_list_box_, LV_SCROLLBAR_MODE_OFF);

        for (size_t i = 0; i < kMaxGatewayItems; i++) {
            gateway_item_containers_[i] = lv_obj_create(gateway_list_box_);
            lv_obj_set_size(gateway_item_containers_[i], 212, 32);
            lv_obj_set_style_bg_opa(gateway_item_containers_[i], LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(gateway_item_containers_[i], 0, 0);
            lv_obj_set_style_pad_all(gateway_item_containers_[i], 0, 0);
            lv_obj_set_style_pad_row(gateway_item_containers_[i], 2, 0);
            lv_obj_set_scrollbar_mode(gateway_item_containers_[i], LV_SCROLLBAR_MODE_OFF);
            lv_obj_set_flex_flow(gateway_item_containers_[i], LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(gateway_item_containers_[i], LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

            gateway_item_labels_[i] = lv_label_create(gateway_item_containers_[i]);
            if (text_font) lv_obj_set_style_text_font(gateway_item_labels_[i], text_font, 0);
            lv_obj_set_style_text_color(gateway_item_labels_[i], lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_width(gateway_item_labels_[i], 208);
            lv_label_set_long_mode(gateway_item_labels_[i], LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(gateway_item_labels_[i], LV_TEXT_ALIGN_CENTER, 0);

            // 选中选项单横线下划线指示 (宽120, 高2px, 零边距)
            gateway_item_lines_[i] = lv_obj_create(gateway_item_containers_[i]);
            lv_obj_set_size(gateway_item_lines_[i], 120, 2);
            lv_obj_set_style_bg_color(gateway_item_lines_[i], lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(gateway_item_lines_[i], LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(gateway_item_lines_[i], 0, 0);
            lv_obj_set_style_pad_all(gateway_item_lines_[i], 0, 0);
            lv_obj_set_style_min_height(gateway_item_lines_[i], 0, 0);
            lv_obj_set_style_min_width(gateway_item_lines_[i], 0, 0);
            lv_obj_set_style_radius(gateway_item_lines_[i], 0, 0);
            lv_obj_set_style_margin_top(gateway_item_lines_[i], 2, 0);
            lv_obj_set_scrollbar_mode(gateway_item_lines_[i], LV_SCROLLBAR_MODE_OFF);
            lv_obj_clear_flag(gateway_item_lines_[i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(gateway_item_lines_[i], LV_OBJ_FLAG_HIDDEN);

            lv_obj_add_flag(gateway_item_containers_[i], LV_OBJ_FLAG_HIDDEN);
        }

        // 9. 项目切换选择弹窗 (宽228, 高260, 黑白高对比)
        project_modal_ = lv_obj_create(screen);
        lv_obj_set_size(project_modal_, 228, 260);
        lv_obj_align(project_modal_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(project_modal_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(project_modal_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(project_modal_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(project_modal_, 2, 0);
        lv_obj_set_style_radius(project_modal_, 8, 0);
        lv_obj_set_style_pad_all(project_modal_, 6, 0);
        lv_obj_set_flex_flow(project_modal_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(project_modal_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(project_modal_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_flag(project_modal_, LV_OBJ_FLAG_HIDDEN);

        project_title_label_ = lv_label_create(project_modal_);
        if (text_font) lv_obj_set_style_text_font(project_title_label_, text_font, 0);
        lv_obj_set_style_text_color(project_title_label_, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(project_title_label_, "[ Projects ]");

        project_sub_label_ = lv_label_create(project_modal_);
        if (text_font) lv_obj_set_style_text_font(project_sub_label_, text_font, 0);
        lv_obj_set_style_text_color(project_sub_label_, lv_color_hex(0xAAAAAA), 0);
        lv_obj_set_width(project_sub_label_, 216);
        lv_label_set_long_mode(project_sub_label_, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(project_sub_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(project_sub_label_, "UP/DOWN: Move   OK: Switch");

        project_sep_ = lv_obj_create(project_modal_);
        lv_obj_set_size(project_sep_, 214, 1);
        lv_obj_set_style_bg_color(project_sep_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(project_sep_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(project_sep_, 0, 0);
        lv_obj_set_style_margin_top(project_sep_, 2, 0);
        lv_obj_set_style_margin_bottom(project_sep_, 4, 0);

        project_list_box_ = lv_obj_create(project_modal_);
        lv_obj_set_size(project_list_box_, 216, 176);
        lv_obj_set_style_bg_opa(project_list_box_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(project_list_box_, 0, 0);
        lv_obj_set_style_pad_all(project_list_box_, 0, 0);
        lv_obj_set_flex_flow(project_list_box_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(project_list_box_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(project_list_box_, LV_SCROLLBAR_MODE_OFF);

        for (size_t i = 0; i < kMaxProjectItems; i++) {
            project_item_containers_[i] = lv_obj_create(project_list_box_);
            lv_obj_set_size(project_item_containers_[i], 212, 32);
            lv_obj_set_style_bg_opa(project_item_containers_[i], LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(project_item_containers_[i], 0, 0);
            lv_obj_set_style_pad_all(project_item_containers_[i], 0, 0);
            lv_obj_set_style_pad_row(project_item_containers_[i], 2, 0);
            lv_obj_set_scrollbar_mode(project_item_containers_[i], LV_SCROLLBAR_MODE_OFF);
            lv_obj_set_flex_flow(project_item_containers_[i], LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(project_item_containers_[i], LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

            project_item_labels_[i] = lv_label_create(project_item_containers_[i]);
            if (text_font) lv_obj_set_style_text_font(project_item_labels_[i], text_font, 0);
            lv_obj_set_style_text_color(project_item_labels_[i], lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_width(project_item_labels_[i], 208);
            lv_label_set_long_mode(project_item_labels_[i], LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(project_item_labels_[i], LV_TEXT_ALIGN_CENTER, 0);

            // 选中选项单横线下划线指示 (宽120, 高2px, 零边距)
            project_item_lines_[i] = lv_obj_create(project_item_containers_[i]);
            lv_obj_set_size(project_item_lines_[i], 120, 2);
            lv_obj_set_style_bg_color(project_item_lines_[i], lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(project_item_lines_[i], LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(project_item_lines_[i], 0, 0);
            lv_obj_set_style_pad_all(project_item_lines_[i], 0, 0);
            lv_obj_set_style_min_height(project_item_lines_[i], 0, 0);
            lv_obj_set_style_min_width(project_item_lines_[i], 0, 0);
            lv_obj_set_style_radius(project_item_lines_[i], 0, 0);
            lv_obj_set_style_margin_top(project_item_lines_[i], 2, 0);
            lv_obj_set_scrollbar_mode(project_item_lines_[i], LV_SCROLLBAR_MODE_OFF);
            lv_obj_clear_flag(project_item_lines_[i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(project_item_lines_[i], LV_OBJ_FLAG_HIDDEN);

            lv_obj_add_flag(project_item_containers_[i], LV_OBJ_FLAG_HIDDEN);
        }

        // 10. 系统设置选择弹窗 (宽228, 高260, 黑白高对比)
        settings_modal_ = lv_obj_create(screen);
        lv_obj_set_size(settings_modal_, 228, 260);
        lv_obj_align(settings_modal_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(settings_modal_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(settings_modal_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(settings_modal_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(settings_modal_, 2, 0);
        lv_obj_set_style_radius(settings_modal_, 8, 0);
        lv_obj_set_style_pad_all(settings_modal_, 6, 0);
        lv_obj_set_flex_flow(settings_modal_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(settings_modal_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(settings_modal_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_flag(settings_modal_, LV_OBJ_FLAG_HIDDEN);

        settings_title_label_ = lv_label_create(settings_modal_);
        if (text_font) lv_obj_set_style_text_font(settings_title_label_, text_font, 0);
        lv_obj_set_style_text_color(settings_title_label_, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(settings_title_label_, "[ Settings / 设定 ]");

        settings_sub_label_ = lv_label_create(settings_modal_);
        if (text_font) lv_obj_set_style_text_font(settings_sub_label_, text_font, 0);
        lv_obj_set_style_text_color(settings_sub_label_, lv_color_hex(0xAAAAAA), 0);
        lv_obj_set_width(settings_sub_label_, 216);
        lv_label_set_long_mode(settings_sub_label_, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(settings_sub_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(settings_sub_label_, "UP/DOWN: Move   OK: Select");

        settings_sep_ = lv_obj_create(settings_modal_);
        lv_obj_set_size(settings_sep_, 214, 1);
        lv_obj_set_style_bg_color(settings_sep_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(settings_sep_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(settings_sep_, 0, 0);
        lv_obj_set_style_margin_top(settings_sep_, 2, 0);
        lv_obj_set_style_margin_bottom(settings_sep_, 4, 0);

        settings_list_box_ = lv_obj_create(settings_modal_);
        lv_obj_set_size(settings_list_box_, 216, 176);
        lv_obj_set_style_bg_opa(settings_list_box_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(settings_list_box_, 0, 0);
        lv_obj_set_style_pad_all(settings_list_box_, 0, 0);
        lv_obj_set_flex_flow(settings_list_box_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(settings_list_box_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(settings_list_box_, LV_SCROLLBAR_MODE_OFF);

        for (size_t i = 0; i < kMaxSettingsItems; i++) {
            settings_item_containers_[i] = lv_obj_create(settings_list_box_);
            lv_obj_set_size(settings_item_containers_[i], 212, 32);
            lv_obj_set_style_bg_opa(settings_item_containers_[i], LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(settings_item_containers_[i], 0, 0);
            lv_obj_set_style_pad_all(settings_item_containers_[i], 0, 0);
            lv_obj_set_style_pad_row(settings_item_containers_[i], 2, 0);
            lv_obj_set_scrollbar_mode(settings_item_containers_[i], LV_SCROLLBAR_MODE_OFF);
            lv_obj_set_flex_flow(settings_item_containers_[i], LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(settings_item_containers_[i], LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

            settings_item_labels_[i] = lv_label_create(settings_item_containers_[i]);
            if (text_font) lv_obj_set_style_text_font(settings_item_labels_[i], text_font, 0);
            lv_obj_set_style_text_color(settings_item_labels_[i], lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_width(settings_item_labels_[i], 208);
            lv_label_set_long_mode(settings_item_labels_[i], LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(settings_item_labels_[i], LV_TEXT_ALIGN_CENTER, 0);

            // 选中选项单横线下划线指示 (宽120, 高2px, 零边距)
            settings_item_lines_[i] = lv_obj_create(settings_item_containers_[i]);
            lv_obj_set_size(settings_item_lines_[i], 120, 2);
            lv_obj_set_style_bg_color(settings_item_lines_[i], lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(settings_item_lines_[i], LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(settings_item_lines_[i], 0, 0);
            lv_obj_set_style_pad_all(settings_item_lines_[i], 0, 0);
            lv_obj_set_style_min_height(settings_item_lines_[i], 0, 0);
            lv_obj_set_style_min_width(settings_item_lines_[i], 0, 0);
            lv_obj_set_style_radius(settings_item_lines_[i], 0, 0);
            lv_obj_set_style_margin_top(settings_item_lines_[i], 2, 0);
            lv_obj_set_scrollbar_mode(settings_item_lines_[i], LV_SCROLLBAR_MODE_OFF);
            lv_obj_clear_flag(settings_item_lines_[i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(settings_item_lines_[i], LV_OBJ_FLAG_HIDDEN);

            lv_obj_add_flag(settings_item_containers_[i], LV_OBJ_FLAG_HIDDEN);
        }

        // 11. 真实 FFT 频谱 20Hz 刷新定时器 (运行在 LVGL 线程中，完全无锁安全)
        if (!wave_timer_) {
            wave_timer_ = lv_timer_create([](lv_timer_t* timer) {
                auto self = static_cast<AiPassportDisplay*>(lv_timer_get_user_data(timer));
                self->OnWaveTimerTick();
            }, 50, this);
        }

        // 12. 确保底部栏默认隐藏，保持纯粹极简
        if (bottom_bar_) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }

        // 13. 初始化并应用当前主题纯黑白样式
        ApplyThemeStyles();
    }

    void OnWaveTimerTick() {
        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();
        auto& audio_service = app.GetAudioService();

        bool is_recording = is_recording_ptt_.load(std::memory_order_relaxed) ||
                            (state == kDeviceStateListening);
        bool is_playing = (state == kDeviceStateSpeaking) || (!audio_service.IsPlaybackIdle());

        if (is_recording || is_playing) {
            uint8_t bands[kWaveBarsCount];
            audio_service.GetSpectrumBands(bands);
            RenderFftSpectrum(bands);
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

    void SetWaveTimerPaused(bool paused) {
        DisplayLockGuard lock(this);
        if (wave_timer_) {
            if (paused) {
                lv_timer_pause(wave_timer_);
                RenderIdleLine();
            } else {
                lv_timer_resume(wave_timer_);
            }
        }
    }

    void ShowIdleStraightLine() {
        DisplayLockGuard lock(this);
        was_active_wave_ = false;
        RenderIdleLine();
    }

    // ================== 飞书网关配对弹窗 ==================
    void ShowGatewayList(const std::vector<FeishuGateway>& gateways) {
        DisplayLockGuard lock(this);
        HideProjectList();
        HideSettingsList();
        SetStatus("");
        cached_gateways_ = gateways;
        selected_gateway_index_ = 0;
        for (size_t i = 0; i < cached_gateways_.size(); i++) {
            if (cached_gateways_[i].is_paired) {
                selected_gateway_index_ = (int)i;
                break;
            }
        }
        if (gateway_modal_) {
            lv_obj_remove_flag(gateway_modal_, LV_OBJ_FLAG_HIDDEN);
        }
        ResetGatewayHideTimer();
        UpdateGatewayListUI();
    }

    void ResetGatewayHideTimer() {
        if (!gateway_hide_timer_) {
            esp_timer_create_args_t gw_timer_args = {
                .callback = [](void* arg) {
                    auto self = static_cast<AiPassportDisplay*>(arg);
                    self->HideGatewayList();
                },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "gw_hide_timer",
                .skip_unhandled_events = true
            };
            esp_timer_create(&gw_timer_args, &gateway_hide_timer_);
        }
        esp_timer_stop(gateway_hide_timer_);
        esp_timer_start_once(gateway_hide_timer_, 15000000);
    }

    void HideGatewayList() {
        DisplayLockGuard lock(this);
        if (gateway_hide_timer_) {
            esp_timer_stop(gateway_hide_timer_);
        }
        if (gateway_modal_) {
            lv_obj_add_flag(gateway_modal_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    bool IsGatewayListVisible() const {
        if (!gateway_modal_) return false;
        return !lv_obj_has_flag(gateway_modal_, LV_OBJ_FLAG_HIDDEN);
    }

    int GetSelectedGatewayIndex() const {
        return selected_gateway_index_;
    }

    void MoveGatewaySelectionUp() {
        DisplayLockGuard lock(this);
        if (cached_gateways_.empty()) return;
        ResetGatewayHideTimer();
        if (selected_gateway_index_ > 0) {
            selected_gateway_index_--;
        } else {
            selected_gateway_index_ = (int)cached_gateways_.size() - 1; // 循环跳到最后一项
        }
        UpdateGatewayListUI();
    }

    void MoveGatewaySelectionDown() {
        DisplayLockGuard lock(this);
        if (cached_gateways_.empty()) return;
        ResetGatewayHideTimer();
        if (selected_gateway_index_ < (int)cached_gateways_.size() - 1) {
            selected_gateway_index_++;
        } else {
            selected_gateway_index_ = 0; // 循环回到第一项
        }
        UpdateGatewayListUI();
    }

    void SelectAndConnectGateway(int index) {
        selected_gateway_index_ = index;
        if (gateway_hide_timer_) {
            esp_timer_stop(gateway_hide_timer_);
        }
        if (index >= 0 && index < (int)cached_gateways_.size()) {
            ShowGatewayConnecting(cached_gateways_[index].name);
        }
        Application::GetInstance().Schedule([this, index]() {
            auto& app = Application::GetInstance();
            app.ConnectFeishuGatewayByIndex(index);
        });
    }

    void ShowGatewayConnecting(const std::string& name) {
        DisplayLockGuard lock(this);
        if (gateway_sub_label_) {
            std::string text = "Connecting: " + name + "\nPlease confirm on Feishu";
            lv_label_set_text(gateway_sub_label_, text.c_str());
        }
    }

    void UpdateGatewayListUI() {
        if (!gateway_modal_) return;

        if (cached_gateways_.empty()) {
            lv_label_set_text(gateway_sub_label_, "Searching... Please wait");
            for (size_t i = 0; i < kMaxGatewayItems; i++) {
                if (gateway_item_containers_[i]) {
                    lv_obj_add_flag(gateway_item_containers_[i], LV_OBJ_FLAG_HIDDEN);
                }
            }
            return;
        }

        lv_label_set_text(gateway_sub_label_, "UP/DOWN: Move   OK: Connect (Hold: Exit)");

        if (selected_gateway_index_ >= (int)cached_gateways_.size()) {
            selected_gateway_index_ = (int)cached_gateways_.size() - 1;
        }
        if (selected_gateway_index_ < 0) {
            selected_gateway_index_ = 0;
        }

        for (size_t i = 0; i < kMaxGatewayItems; i++) {
            if (!gateway_item_containers_[i]) continue;
            if (i < cached_gateways_.size()) {
                lv_obj_remove_flag(gateway_item_containers_[i], LV_OBJ_FLAG_HIDDEN);
                const auto& gw = cached_gateways_[i];

                char buf[64];
                const char* star = gw.is_paired ? "★ " : "";
                snprintf(buf, sizeof(buf), "%s%s (%s)", star, gw.name.c_str(), gw.ip.c_str());
                lv_label_set_text(gateway_item_labels_[i], buf);

                if ((int)i == selected_gateway_index_) {
                    lv_color_t line_col = is_theme_inverted_ ? lv_color_hex(0x000000) : lv_color_hex(0xFFFFFF);
                    lv_obj_set_style_bg_color(gateway_item_lines_[i], line_col, 0);
                    lv_obj_remove_flag(gateway_item_lines_[i], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_style_border_side(gateway_item_containers_[i], LV_BORDER_SIDE_BOTTOM, 0);
                    lv_obj_set_style_border_width(gateway_item_containers_[i], 2, 0);
                    lv_obj_set_style_border_color(gateway_item_containers_[i], line_col, 0);
                    lv_obj_scroll_to_view(gateway_item_containers_[i], LV_ANIM_ON);
                } else {
                    lv_obj_add_flag(gateway_item_lines_[i], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_style_border_width(gateway_item_containers_[i], 0, 0);
                }
            } else {
                lv_obj_add_flag(gateway_item_containers_[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    // ================== 项目切换弹窗 (单横线下划线指示) ==================
    void ShowProjectList(const std::vector<std::string>& projects, const std::string& current_project) {
        DisplayLockGuard lock(this);
        HideGatewayList();
        HideSettingsList();
        SetStatus("");
        cached_projects_ = projects;
        current_project_name_ = current_project;
        selected_project_index_ = 0;
        for (size_t i = 0; i < cached_projects_.size(); i++) {
            if (cached_projects_[i] == current_project_name_) {
                selected_project_index_ = (int)i;
                break;
            }
        }
        if (project_modal_) {
            lv_obj_remove_flag(project_modal_, LV_OBJ_FLAG_HIDDEN);
        }
        ResetProjectHideTimer();
        UpdateProjectListUI();
    }

    void ResetProjectHideTimer() {
        if (!project_hide_timer_) {
            esp_timer_create_args_t p_timer_args = {
                .callback = [](void* arg) {
                    auto self = static_cast<AiPassportDisplay*>(arg);
                    self->HideProjectList();
                },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "proj_hide_timer",
                .skip_unhandled_events = true
            };
            esp_timer_create(&p_timer_args, &project_hide_timer_);
        }
        esp_timer_stop(project_hide_timer_);
        esp_timer_start_once(project_hide_timer_, 15000000);
    }

    void HideProjectList() {
        DisplayLockGuard lock(this);
        if (project_hide_timer_) {
            esp_timer_stop(project_hide_timer_);
        }
        if (project_modal_) {
            lv_obj_add_flag(project_modal_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    bool IsProjectListVisible() const {
        if (!project_modal_) return false;
        return !lv_obj_has_flag(project_modal_, LV_OBJ_FLAG_HIDDEN);
    }

    void MoveProjectSelectionUp() {
        DisplayLockGuard lock(this);
        if (cached_projects_.empty()) return;
        ResetProjectHideTimer();
        if (selected_project_index_ > 0) {
            selected_project_index_--;
        } else {
            selected_project_index_ = (int)cached_projects_.size() - 1; // 循环跳到末项
        }
        UpdateProjectListUI();
    }

    void MoveProjectSelectionDown() {
        DisplayLockGuard lock(this);
        if (cached_projects_.empty()) return;
        ResetProjectHideTimer();
        if (selected_project_index_ < (int)cached_projects_.size() - 1) {
            selected_project_index_++;
        } else {
            selected_project_index_ = 0; // 循环回到首项
        }
        UpdateProjectListUI();
    }

    std::string GetSelectedProjectName() const {
        if (selected_project_index_ >= 0 && selected_project_index_ < (int)cached_projects_.size()) {
            return cached_projects_[selected_project_index_];
        }
        return "";
    }

    void UpdateProjectListUI() {
        if (!project_modal_) return;

        if (cached_projects_.empty()) {
            lv_label_set_text(project_sub_label_, "No projects found");
            for (size_t i = 0; i < kMaxProjectItems; i++) {
                if (project_item_containers_[i]) {
                    lv_obj_add_flag(project_item_containers_[i], LV_OBJ_FLAG_HIDDEN);
                }
            }
            return;
        }

        lv_label_set_text(project_sub_label_, "UP/DOWN: Move   OK: Switch (Hold: Exit)");

        if (selected_project_index_ >= (int)cached_projects_.size()) {
            selected_project_index_ = (int)cached_projects_.size() - 1;
        }
        if (selected_project_index_ < 0) {
            selected_project_index_ = 0;
        }

        for (size_t i = 0; i < kMaxProjectItems; i++) {
            if (!project_item_containers_[i]) continue;
            if (i < cached_projects_.size()) {
                lv_obj_remove_flag(project_item_containers_[i], LV_OBJ_FLAG_HIDDEN);
                const auto& proj = cached_projects_[i];

                char buf[64];
                if (proj == current_project_name_) {
                    snprintf(buf, sizeof(buf), "%s (当前)", proj.c_str());
                } else {
                    snprintf(buf, sizeof(buf), "%s", proj.c_str());
                }
                lv_label_set_text(project_item_labels_[i], buf);

                if ((int)i == selected_project_index_) {
                    lv_color_t line_col = is_theme_inverted_ ? lv_color_hex(0x000000) : lv_color_hex(0xFFFFFF);
                    lv_obj_set_style_bg_color(project_item_lines_[i], line_col, 0);
                    lv_obj_remove_flag(project_item_lines_[i], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_style_border_side(project_item_containers_[i], LV_BORDER_SIDE_BOTTOM, 0);
                    lv_obj_set_style_border_width(project_item_containers_[i], 2, 0);
                    lv_obj_set_style_border_color(project_item_containers_[i], line_col, 0);
                    lv_obj_scroll_to_view(project_item_containers_[i], LV_ANIM_ON);
                } else {
                    lv_obj_add_flag(project_item_lines_[i], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_style_border_width(project_item_containers_[i], 0, 0);
                }
            } else {
                lv_obj_add_flag(project_item_containers_[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    // ================== 系统设置弹窗 (单横线下划线指示) ==================
    void ShowSettingsList() {
        DisplayLockGuard lock(this);
        HideGatewayList();
        HideProjectList();
        SetStatus("");
        selected_settings_index_ = 0;
        if (settings_modal_) {
            lv_obj_remove_flag(settings_modal_, LV_OBJ_FLAG_HIDDEN);
        }
        ResetSettingsHideTimer();
        UpdateSettingsListUI();
    }

    void ResetSettingsHideTimer() {
        if (!settings_hide_timer_) {
            esp_timer_create_args_t s_timer_args = {
                .callback = [](void* arg) {
                    auto self = static_cast<AiPassportDisplay*>(arg);
                    self->HideSettingsList();
                },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "sett_hide_timer",
                .skip_unhandled_events = true
            };
            esp_timer_create(&s_timer_args, &settings_hide_timer_);
        }
        esp_timer_stop(settings_hide_timer_);
        esp_timer_start_once(settings_hide_timer_, 15000000);
    }

    void HideSettingsList() {
        DisplayLockGuard lock(this);
        if (settings_hide_timer_) {
            esp_timer_stop(settings_hide_timer_);
        }
        if (settings_modal_) {
            lv_obj_add_flag(settings_modal_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    bool IsSettingsListVisible() const {
        if (!settings_modal_) return false;
        return !lv_obj_has_flag(settings_modal_, LV_OBJ_FLAG_HIDDEN);
    }

    void MoveSettingsSelectionUp() {
        DisplayLockGuard lock(this);
        ResetSettingsHideTimer();
        if (selected_settings_index_ > 0) {
            selected_settings_index_--;
        } else {
            selected_settings_index_ = (int)kMaxSettingsItems - 1; // 循环跳到末项
        }
        UpdateSettingsListUI();
    }

    void MoveSettingsSelectionDown() {
        DisplayLockGuard lock(this);
        ResetSettingsHideTimer();
        if (selected_settings_index_ < (int)kMaxSettingsItems - 1) {
            selected_settings_index_++;
        } else {
            selected_settings_index_ = 0; // 循环回到首项
        }
        UpdateSettingsListUI();
    }

    int GetSelectedSettingsIndex() const {
        return selected_settings_index_;
    }

    void UpdateSettingsListUI() {
        if (!settings_modal_) return;

        static const char* kSettingsTitles[kMaxSettingsItems] = {
            "Brightness",
            "Volume (音量)",
            "Theme (模式切换)",
            "Gateway (服务连接)",
            "WiFi (重新配网)",
            "Device Info (设备信息)",
            "Reboot (重启设备)"
        };

        lv_label_set_text(settings_sub_label_, "UP/DOWN: Move   OK: Select (Hold: Exit)");

        for (size_t i = 0; i < kMaxSettingsItems; i++) {
            if (!settings_item_containers_[i]) continue;
            lv_obj_remove_flag(settings_item_containers_[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(settings_item_labels_[i], kSettingsTitles[i]);

            if ((int)i == selected_settings_index_) {
                lv_color_t line_col = is_theme_inverted_ ? lv_color_hex(0x000000) : lv_color_hex(0xFFFFFF);
                lv_obj_set_style_bg_color(settings_item_lines_[i], line_col, 0);
                lv_obj_remove_flag(settings_item_lines_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_border_side(settings_item_containers_[i], LV_BORDER_SIDE_BOTTOM, 0);
                lv_obj_set_style_border_width(settings_item_containers_[i], 2, 0);
                lv_obj_set_style_border_color(settings_item_containers_[i], line_col, 0);
                lv_obj_scroll_to_view(settings_item_containers_[i], LV_ANIM_ON);
            } else {
                lv_obj_add_flag(settings_item_lines_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_border_width(settings_item_containers_[i], 0, 0);
            }
        }
    }

    bool IsAnyMenuVisible() const {
        return IsGatewayListVisible() || IsProjectListVisible() || IsSettingsListVisible();
    }

    void HideAllMenus() {
        HideGatewayList();
        HideProjectList();
        HideSettingsList();
    }

    // 纯黑白双色主题自适应渲染
    // 在白色主题下（is_theme_inverted_ == true）：白底、黑字、黑色边框、选中下划线为纯黑
    // 在黑色主题下（is_theme_inverted_ == false）：黑底、白字、白色边框、选中下划线为纯白
    void ApplyThemeStyles() {
        DisplayLockGuard lock(this);
        auto screen = lv_screen_active();

        lv_color_t bg_col = is_theme_inverted_ ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x000000);
        lv_color_t fg_col = is_theme_inverted_ ? lv_color_hex(0x000000) : lv_color_hex(0xFFFFFF);
        lv_color_t sub_col = is_theme_inverted_ ? lv_color_hex(0x666666) : lv_color_hex(0xAAAAAA);

        if (screen) {
            lv_obj_set_style_bg_color(screen, bg_col, 0);
        }
        if (center_line_obj_) {
            lv_obj_set_style_bg_color(center_line_obj_, fg_col, 0);
        }
        for (int i = 0; i < kWaveBarsCount; i++) {
            if (wave_bars_[i]) {
                lv_obj_set_style_bg_color(wave_bars_[i], fg_col, 0);
            }
        }
        if (bottom_volume_box_) {
            lv_obj_set_style_bg_color(bottom_volume_box_, bg_col, 0);
            lv_obj_set_style_border_color(bottom_volume_box_, fg_col, 0);
            if (volume_icon_label_) lv_obj_set_style_text_color(volume_icon_label_, fg_col, 0);
            if (volume_text_label_) lv_obj_set_style_text_color(volume_text_label_, fg_col, 0);
        }
        if (status_label_) {
            lv_obj_set_style_text_color(status_label_, fg_col, 0);
        }
        if (notification_label_) {
            lv_obj_set_style_text_color(notification_label_, fg_col, 0);
        }

        // 飞书网关配对选择弹窗
        if (gateway_modal_) {
            lv_obj_set_style_bg_color(gateway_modal_, bg_col, 0);
            lv_obj_set_style_border_color(gateway_modal_, fg_col, 0);
            if (gateway_title_label_) lv_obj_set_style_text_color(gateway_title_label_, fg_col, 0);
            if (gateway_sub_label_) lv_obj_set_style_text_color(gateway_sub_label_, sub_col, 0);
            if (gateway_sep_) lv_obj_set_style_bg_color(gateway_sep_, fg_col, 0);
            for (size_t i = 0; i < kMaxGatewayItems; i++) {
                if (gateway_item_labels_[i]) lv_obj_set_style_text_color(gateway_item_labels_[i], fg_col, 0);
                if (gateway_item_lines_[i]) lv_obj_set_style_bg_color(gateway_item_lines_[i], fg_col, 0);
            }
        }

        // 项目切换选择弹窗
        if (project_modal_) {
            lv_obj_set_style_bg_color(project_modal_, bg_col, 0);
            lv_obj_set_style_border_color(project_modal_, fg_col, 0);
            if (project_title_label_) lv_obj_set_style_text_color(project_title_label_, fg_col, 0);
            if (project_sub_label_) lv_obj_set_style_text_color(project_sub_label_, sub_col, 0);
            if (project_sep_) lv_obj_set_style_bg_color(project_sep_, fg_col, 0);
            for (size_t i = 0; i < kMaxProjectItems; i++) {
                if (project_item_labels_[i]) lv_obj_set_style_text_color(project_item_labels_[i], fg_col, 0);
                if (project_item_lines_[i]) lv_obj_set_style_bg_color(project_item_lines_[i], fg_col, 0);
            }
        }

        // 系统设置选择弹窗
        if (settings_modal_) {
            lv_obj_set_style_bg_color(settings_modal_, bg_col, 0);
            lv_obj_set_style_border_color(settings_modal_, fg_col, 0);
            if (settings_title_label_) lv_obj_set_style_text_color(settings_title_label_, fg_col, 0);
            if (settings_sub_label_) lv_obj_set_style_text_color(settings_sub_label_, sub_col, 0);
            if (settings_sep_) lv_obj_set_style_bg_color(settings_sep_, fg_col, 0);
            for (size_t i = 0; i < kMaxSettingsItems; i++) {
                if (settings_item_labels_[i]) lv_obj_set_style_text_color(settings_item_labels_[i], fg_col, 0);
                if (settings_item_lines_[i]) lv_obj_set_style_bg_color(settings_item_lines_[i], fg_col, 0);
            }
        }
    }

    // 切换黑白主题
    void ToggleThemeInvert() {
        DisplayLockGuard lock(this);
        is_theme_inverted_ = !is_theme_inverted_;
        ApplyThemeStyles();
        Settings settings("display", true);
        settings.SetInt("invert_color", is_theme_inverted_ ? 1 : 0);
        ShowNotification(is_theme_inverted_ ? "Theme: White" : "Theme: Black", 1500);
    }

    void ApplySavedThemeInvert() {
        Settings settings("display", false);
        int inv = settings.GetInt("invert_color", 0);
        is_theme_inverted_ = (inv == 1);
        ApplyThemeStyles();
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
            lv_color_t bg_col = is_theme_inverted_ ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x000000);
            lv_color_t fg_col = is_theme_inverted_ ? lv_color_hex(0x000000) : lv_color_hex(0xFFFFFF);
            lvgl_theme->set_background_color(bg_col);
            lvgl_theme->set_text_color(fg_col);
            lvgl_theme->set_chat_background_color(bg_col);
            lvgl_theme->set_user_bubble_color(bg_col);
            lvgl_theme->set_assistant_bubble_color(bg_col);
            lvgl_theme->set_system_bubble_color(bg_col);
            lvgl_theme->set_system_text_color(fg_col);
            lvgl_theme->set_border_color(fg_col);
            lvgl_theme->set_low_battery_color(fg_col);
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
    std::vector<std::string> last_projects_;
    std::string current_project_;
    int64_t ok_press_down_time_ = 0;
    bool ok_speech_aborted_ = false;
    bool ok_stop_listening_on_down_ = false;
    int64_t last_ok_tap_time_ = 0;
    esp_timer_handle_t menu_long_press_timer_ = nullptr;
    bool ok_menu_closed_by_long_press_ = false;

    void StartMenuLongPressTimer() {
        if (!menu_long_press_timer_) {
            esp_timer_create_args_t timer_args = {
                .callback = [](void* arg) {
                    auto self = static_cast<AiPassportBoard*>(arg);
                    Application::GetInstance().Schedule([self]() {
                        if (self->display_ && self->display_->IsAnyMenuVisible()) {
                            self->ok_menu_closed_by_long_press_ = true;
                            self->display_->HideAllMenus();
                            self->display_->ShowNotification("Main screen", 1200);
                            auto& app = Application::GetInstance();
                            self->display_->SetStatus(app.IsFeishuConnected() ? "[已连接]" : "[待命]");
                            self->display_->ShowIdleStraightLine();
                            ESP_LOGI(TAG, "Menu closed by OK long press (500ms timer)");
                        }
                    });
                },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "menu_lp_timer"
            };
            esp_timer_create(&timer_args, &menu_long_press_timer_);
        }
        esp_timer_stop(menu_long_press_timer_);
        esp_timer_start_once(menu_long_press_timer_, 500000); // 500ms
    }

    void StopMenuLongPressTimer() {
        if (menu_long_press_timer_) {
            esp_timer_stop(menu_long_press_timer_);
        }
    }

    bool CheckAndAbortSpeaking() {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateSpeaking) {
            ESP_LOGI(TAG, "Barge-in triggered by physical key! Aborting speech...");
            app.AbortSpeaking(kAbortReasonNone);
            app.GetAudioService().ResetDecoder();
            if (display_) {
                display_->ShowNotification("Barge-in", 1200);
                display_->SetStatus("[待命]");
                display_->ShowIdleStraightLine();
            }
            return true;
        }
        return false;
    }

    void EnterOrRefreshStandbyClock() {
        if (!display_) return;
        if (display_->IsAnyMenuVisible()) return;
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

        auto& app = Application::GetInstance();
        bool feishu_online = app.IsFeishuConnected();

        char status_buf[64];
        snprintf(status_buf, sizeof(status_buf), "%s  %s",
                 time_buf, feishu_online ? "[已连接]" : "[待命]");

        display_->SetStatus(status_buf);
        display_->ShowIdleStraightLine();
        display_->SetWaveTimerPaused(true);
    }

    void ExitStandbyClock() {
        if (!is_in_standby_clock_) return;
        is_in_standby_clock_ = false;
        if (display_) {
            display_->SetWaveTimerPaused(false);
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
                    auto cur_state = Application::GetInstance().GetDeviceState();
                    if (!self->is_push_to_talk_active_ && cur_state != kDeviceStateListening) {
                        return;
                    }
                    int64_t elapsed_us = esp_timer_get_time() - self->record_start_time_;
                    int elapsed_sec = (int)(elapsed_us / 1000000);
                    Application::GetInstance().Schedule([self, elapsed_sec, elapsed_us]() {
                        auto state = Application::GetInstance().GetDeviceState();
                        if (!self->is_push_to_talk_active_ && state != kDeviceStateListening) {
                            return;
                        }
                        if (elapsed_sec >= 60) {
                            self->StopPushToTalk(elapsed_us);
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

    void StopPushToTalk(int64_t elapsed_us) {
        auto& app = Application::GetInstance();
        if (!is_push_to_talk_active_ && app.GetDeviceState() != kDeviceStateListening) {
            return;
        }
        is_push_to_talk_active_ = false;
        StopRecordTimer();

        // 录音结束，立即恢复屏幕正中心水平直线
        if (display_) {
            display_->SetRecording(false);
            display_->ShowIdleStraightLine();
        }

        // 仅当录音极短（小于 250ms 即 0.25 秒误按）才视为误触取消
        if (elapsed_us < 250000) {
            if (app.GetDeviceState() == kDeviceStateListening) {
                app.StopListening();
            }
            if (display_) {
                display_->ShowNotification("Audio too short", 1500);
                display_->SetStatus(app.IsFeishuConnected() ? "[已连接]" : "[待命]");
            }
            return;
        }

        app.SetFeishuAwaitingReply(true);
        if (app.GetDeviceState() == kDeviceStateListening) {
            app.StopListening();
        }

        int duration_sec = (int)((elapsed_us + 500000) / 1000000);
        if (duration_sec < 1) duration_sec = 1;

        if (display_) {
            char tip[64];
            snprintf(tip, sizeof(tip), "Audio sent (%ds)\nProcessing...", duration_sec);
            display_->ShowNotification(tip, 1500);
            display_->SetStatus("[Thinking...]");
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

    void CycleBrightness() {
        if (current_brightness_ >= 100) {
            current_brightness_ = 20;
        } else {
            current_brightness_ += 20;
            if (current_brightness_ > 100) current_brightness_ = 100;
        }
        GetBacklight()->SetBrightness(current_brightness_);
        if (display_) {
            char buf[32];
            snprintf(buf, sizeof(buf), "[屏幕亮度: %d%%]", current_brightness_);
            display_->ShowNotification(buf, 1500);
        }
    }

    void CycleVolume() {
        auto codec = GetAudioCodec();
        int vol = codec->output_volume();
        if (vol >= 100) {
            vol = 20;
        } else {
            vol += 20;
            if (vol > 100) vol = 100;
        }
        codec->SetOutputVolume(vol);
        if (display_) {
            display_->ShowVolumeBottom(vol);
        }
    }

    void ExecuteSettingItem(int index) {
        switch (index) {
            case 0: // 亮度调节
                CycleBrightness();
                break;
            case 1: // 音量调节
                CycleVolume();
                break;
            case 2: // 主题配置 (反色)
                if (display_) {
                    display_->ToggleThemeInvert();
                }
                break;
            case 3: // 服务端配置
                if (display_) {
                    display_->HideSettingsList();
                    auto& app = Application::GetInstance();
                    auto gateways = app.GetDiscoveredFeishuGateways();
                    display_->ShowGatewayList(gateways);
                }
                break;
            case 4: // 网络配网
                if (display_) {
                    display_->HideSettingsList();
                    display_->ShowNotification("Entering WiFi Config...", 3000);
                }
                EnterWifiConfigMode();
                break;
            case 5: { // 设备信息
                if (display_) {
                    display_->HideSettingsList();
                }
                auto& app = Application::GetInstance();
                auto& wifi = WifiManager::GetInstance();
                std::string ip = wifi.GetIpAddress();
                if (ip.empty()) ip = "None";
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
                    info += "\nGateway: Connected (" + gw + ")";
                } else {
                    info += "\nGateway: Disconnected";
                }
                if (battery_level >= 0) {
                    info += "\nBattery: " + std::to_string(battery_level) + "% (" + std::to_string(battery_mv) + "mV)";
                }
                if (display_) {
                    display_->ShowNotification(info.c_str(), 4500);
                }
                break;
            }
            case 6: // 重启设备
                if (display_) {
                    display_->HideSettingsList();
                    display_->ShowNotification("Rebooting...", 2000);
                }
                Application::GetInstance().Schedule([]() {
                    Application::GetInstance().Reboot();
                });
                break;
            default:
                break;
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
        adc_button_[kAdcButtonOk] = new AdcButton(adc_cfg);

        // 按钮事件绑定与交互优化
        auto up = adc_button_[kAdcButtonUp];
        up->OnClick([this]() {
            TouchActivity("UP");
            if (display_) {
                if (display_->IsGatewayListVisible()) {
                    display_->MoveGatewaySelectionUp();
                    return;
                }
                if (display_->IsProjectListVisible()) {
                    display_->MoveProjectSelectionUp();
                    return;
                }
                if (display_->IsSettingsListVisible()) {
                    display_->MoveSettingsSelectionUp();
                    return;
                }
            }

            // 主界面短按 ▲ 键：立即呼出项目切换列表弹窗
            Application::GetInstance().Schedule([this]() {
                if (last_projects_.empty()) {
                    last_projects_ = {"Letgo", "passport_game", "feishu-bot-plugin", "antigravity-feishu-bot"};
                    if (current_project_.empty()) current_project_ = "Letgo";
                }
                if (display_) {
                    display_->ShowProjectList(last_projects_, current_project_);
                }
                auto& app = Application::GetInstance();
                if (app.IsFeishuConnected()) {
                    app.RequestFeishuProjectList();
                }
            });
        });
        up->OnDoubleClick([this]() {
            TouchActivity("UP_DOUBLE");
            Application::GetInstance().Schedule([this]() {
                auto& app = Application::GetInstance();
                if (!app.IsFeishuConnected()) {
                    if (display_) {
                        display_->ShowNotification("Searching gateway...", 3000);
                        display_->SetStatus("[待命]");
                    }
                    app.TriggerFeishuDiscovery();
                } else {
                    if (display_) {
                        display_->ShowNotification("Refreshing...", 2000);
                    }
                    app.SendFeishuButtonEvent("up", "double_click");
                }
            });
        });
        up->OnLongPress([this]() {
            TouchActivity("UP_LONG");
            Application::GetInstance().Schedule([this]() {
                if (display_) {
                    display_->ShowNotification("Entering WiFi Config...", 3000);
                }
                EnterWifiConfigMode();
            });
        });

        auto down = adc_button_[kAdcButtonDown];
        down->OnClick([this]() {
            TouchActivity("DOWN");
            if (display_) {
                if (display_->IsGatewayListVisible()) {
                    display_->MoveGatewaySelectionDown();
                    return;
                }
                if (display_->IsProjectListVisible()) {
                    display_->MoveProjectSelectionDown();
                    return;
                }
                if (display_->IsSettingsListVisible()) {
                    display_->MoveSettingsSelectionDown();
                    return;
                }
            }

            // 主界面短按 ▼ 键：呼出系统设置菜单
            Application::GetInstance().Schedule([this]() {
                if (display_) {
                    display_->ShowSettingsList();
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
                    ip = "None";
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
                    info += "\nGateway: Connected (" + gw + ")";
                } else {
                    info += "\nGateway: Disconnected";
                }
                if (battery_level >= 0) {
                    info += "\nBattery: " + std::to_string(battery_level) + "% (" + std::to_string(battery_mv) + "mV)";
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
        ok->OnPressDown([this]() {
            TouchActivity("OK_DOWN");
            ok_press_down_time_ = esp_timer_get_time();
            ok_speech_aborted_ = false;
            ok_stop_listening_on_down_ = false;

            // 1. 如果菜单开启，按下时立即启动 500ms 长按退出检测定时器，不触发对讲
            if (display_ && display_->IsAnyMenuVisible()) {
                ok_menu_closed_by_long_press_ = false;
                StartMenuLongPressTimer();
                return;
            }

            auto& app = Application::GetInstance();
            auto state = app.GetDeviceState();

            // 2. 如果正在播放 TTS 语音，按下立即打断 (Barge-in)
            if (state == kDeviceStateSpeaking) {
                ESP_LOGI(TAG, "Barge-in triggered by OK press down!");
                app.AbortSpeaking(kAbortReasonNone);
                app.GetAudioService().ResetDecoder();
                if (display_) {
                    display_->ShowNotification("Barge-in", 1200);
                    display_->SetStatus("[待命]");
                    display_->ShowIdleStraightLine();
                }
                ok_speech_aborted_ = true;
                return;
            }

            // 3. 如果当前已经在录音中，用户再次按下 OK 键意味着【停止录音并发送】（单击切换对话模式下的第二次点击）
            if (state == kDeviceStateListening) {
                ESP_LOGI(TAG, "OK pressed while listening: Stopping recording...");
                ok_stop_listening_on_down_ = true;
                int64_t elapsed_us = esp_timer_get_time() - record_start_time_;
                StopPushToTalk(elapsed_us);
                return;
            }

            // 4. 如果当前设备处于待命状态，按下瞬间立即启动录音！
            if (state == kDeviceStateIdle || state == kDeviceStateStarting) {
                is_push_to_talk_active_ = true;
                wave_frame_ = 0;
                record_start_time_ = esp_timer_get_time();
                if (display_) {
                    display_->SetRecording(true);
                    display_->SetStatus("[录音中 00:00]");
                }
                StartRecordTimer();
                Application::GetInstance().Schedule([]() {
                    Application::GetInstance().StartListening();
                });
            }
        });

        ok->OnLongPress([this]() {
            TouchActivity("OK_LONG");
            StopMenuLongPressTimer();
            if (display_ && display_->IsAnyMenuVisible()) {
                ok_menu_closed_by_long_press_ = true;
                display_->HideAllMenus();
                display_->ShowNotification("Main screen", 1200);
                auto& app = Application::GetInstance();
                display_->SetStatus(app.IsFeishuConnected() ? "[已连接]" : "[待命]");
                display_->ShowIdleStraightLine();
                ESP_LOGI(TAG, "Menu closed by OK OnLongPress");
            }
        });

        ok->OnPressUp([this]() {
            TouchActivity("OK_UP");
            StopMenuLongPressTimer();
            int64_t now = esp_timer_get_time();
            int64_t duration_ms = (now - ok_press_down_time_) / 1000;

            // 如果刚才长按已经退出了菜单，抬手仅消耗事件，直接返回
            if (ok_menu_closed_by_long_press_) {
                ok_menu_closed_by_long_press_ = false;
                return;
            }

            // 如果刚才打断了语音播报，抬手不做任何额外处理
            if (ok_speech_aborted_) {
                ok_speech_aborted_ = false;
                return;
            }

            // 如果刚才在按下时触发了停止录音，抬手不做任何处理
            if (ok_stop_listening_on_down_) {
                ok_stop_listening_on_down_ = false;
                return;
            }

            // 菜单可见时的选择确认逻辑
            if (display_ && display_->IsAnyMenuVisible()) {
                // 如果按住达到 500ms（兜底长按逻辑），关闭菜单返回主界面
                if (duration_ms >= 500) {
                    display_->HideAllMenus();
                    display_->ShowNotification("Main screen", 1200);
                    auto& app = Application::GetInstance();
                    display_->SetStatus(app.IsFeishuConnected() ? "[已连接]" : "[待命]");
                    display_->ShowIdleStraightLine();
                    return;
                }
                if (display_->IsGatewayListVisible()) {
                    int idx = display_->GetSelectedGatewayIndex();
                    display_->SelectAndConnectGateway(idx);
                    return;
                }
                if (display_->IsProjectListVisible()) {
                    std::string selected_proj = display_->GetSelectedProjectName();
                    display_->HideProjectList();
                    if (!selected_proj.empty()) {
                        current_project_ = selected_proj;
                        char buf[64];
                        snprintf(buf, sizeof(buf), "Switched to:\n%s", selected_proj.c_str());
                        display_->ShowNotification(buf, 1500);
                        Application::GetInstance().Schedule([selected_proj]() {
                            Application::GetInstance().SwitchFeishuProject(selected_proj);
                        });
                    }
                    return;
                }
                if (display_->IsSettingsListVisible()) {
                    int idx = display_->GetSelectedSettingsIndex();
                    ExecuteSettingItem(idx);
                    return;
                }
            }

            auto& app = Application::GetInstance();

            // 若有未连接的飞书网关，优先连接
            if (app.HasPendingFeishuGateway()) {
                if (is_push_to_talk_active_ || app.GetDeviceState() == kDeviceStateListening) {
                    is_push_to_talk_active_ = false;
                    StopRecordTimer();
                    if (app.GetDeviceState() == kDeviceStateListening) {
                        app.StopListening();
                    }
                    if (display_) {
                        display_->SetRecording(false);
                        display_->ShowIdleStraightLine();
                    }
                }
                app.ConnectSelectedFeishuGateway();
                return;
            }

            // 对讲录音判定：
            if (is_push_to_talk_active_) {
                // 检查是否为快速双击（双击请求重播上条）
                if (duration_ms < 350) {
                    int64_t interval_since_last_tap = (now - last_ok_tap_time_) / 1000;
                    last_ok_tap_time_ = now;
                    if (interval_since_last_tap > 50 && interval_since_last_tap < 400) {
                        // 判定为双击：取消录音并触发重播
                        is_push_to_talk_active_ = false;
                        StopRecordTimer();
                        if (app.GetDeviceState() == kDeviceStateListening) {
                            app.StopListening();
                        }
                        if (display_) {
                            display_->SetRecording(false);
                            display_->ShowIdleStraightLine();
                            display_->ShowNotification("Requesting replay...", 1500);
                        }
                        if (app.IsFeishuConnected()) {
                            app.SendFeishuButtonEvent("ok", "double_click");
                        }
                        last_ok_tap_time_ = 0;
                        return;
                    }
                }

                if (duration_ms < 350) {
                    // 【短按点击 / Quick Tap】（完美兼容原项目 ToggleChat 体验！）
                    // 用户轻点了一下 OK 键：保持录音状态，用户可从容讲话，说完再次点击 OK 键结束
                    is_push_to_talk_active_ = false; // 脱离长按按住模式，进入持续点击对话模式
                    if (display_) {
                        display_->SetStatus("[录音中 (点击OK结束)]");
                    }
                    ESP_LOGI(TAG, "OK quick tap: Keep recording in toggle mode (duration %lld ms)", duration_ms);
                } else {
                    // 【长按对讲 / PTT: Push-to-Talk】（纯正对讲机体验！）
                    // 用户按住说话，松手立即结束对讲并发送给飞书
                    int64_t elapsed_us = now - record_start_time_;
                    ESP_LOGI(TAG, "OK long press released: Stop PTT recording (duration %lld ms)", duration_ms);
                    StopPushToTalk(elapsed_us);
                }
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
    virtual ~AiPassportBoard() {
        if (dim_timer_) {
            esp_timer_stop(dim_timer_);
            esp_timer_delete(dim_timer_);
            dim_timer_ = nullptr;
        }
        if (record_timer_) {
            esp_timer_stop(record_timer_);
            esp_timer_delete(record_timer_);
            record_timer_ = nullptr;
        }
        if (menu_long_press_timer_) {
            esp_timer_stop(menu_long_press_timer_);
            esp_timer_delete(menu_long_press_timer_);
            menu_long_press_timer_ = nullptr;
        }
    }

    AiPassportBoard() : display_(nullptr), battery_(nullptr) {
        InitializeCodecI2c();
        InitializeSpi();
        InitializeDisplay();
        InitializeButtons();
        GetBacklight()->RestoreBrightness();

        Settings settings("display", true);
        settings.SetString("theme", "dark");
        if (display_) {
            display_->ApplySavedThemeInvert();
        }

        last_projects_ = {"Letgo", "passport_game", "feishu-bot-plugin", "antigravity-feishu-bot"};
        current_project_ = "Letgo";

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

                // 若菜单正在显示，不触发时钟看板和休眠变暗
                if (self->display_ && self->display_->IsAnyMenuVisible()) {
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

        // 监听局域网飞书网关列表变动，若未连接且发现候选网关，自动呼出选择弹窗
        Application::GetInstance().SetOnFeishuGatewaysChanged([this](const std::vector<FeishuGateway>& gateways) {
            if (!display_) return;
            auto& app = Application::GetInstance();
            if (app.IsFeishuConnected()) {
                display_->HideGatewayList();
                return;
            }
            if (!gateways.empty()) {
                display_->ShowGatewayList(gateways);
                TouchActivity();
            } else {
                display_->HideGatewayList();
            }
        });

        // 监听飞书服务端返回的项目列表
        Application::GetInstance().SetOnFeishuProjectsReceived([this](const std::vector<std::string>& projects, const std::string& current) {
            if (!projects.empty()) {
                last_projects_ = projects;
            }
            if (!current.empty()) {
                current_project_ = current;
            }
            if (display_ && display_->IsProjectListVisible()) {
                display_->ShowProjectList(last_projects_, current_project_);
            }
        });
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
