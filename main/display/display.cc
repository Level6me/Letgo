#include "display.h"
#include <esp_err.h>
#include <esp_log.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include "application.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "lvgl_display/lvgl_image.h"
#include "settings.h"

#define TAG "Display"

Display::Display() {}

Display::~Display() {}

void Display::SetPreviewImage(std::unique_ptr<LvglImage> image) {}

void Display::SetStatus(const char* status) { ESP_LOGW(TAG, "SetStatus: %s", status); }

void Display::ShowNotification(const std::string& notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void Display::ShowNotification(const char* notification, int duration_ms) {
    ESP_LOGW(TAG, "ShowNotification: %s", notification);
}

void Display::ShowProgress(int progress, size_t speed) {
    char buffer[32];
    if (speed > 0) {
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, (unsigned int)(speed / 1024));
    } else {
        snprintf(buffer, sizeof(buffer), "%d%%", progress);
    }
    char st_buf[32];
    snprintf(st_buf, sizeof(st_buf), "OTA %d%%", progress);
    SetStatus(st_buf);
    SetChatMessage("system", buffer);
}

void Display::HideProgress() {
    SetStatus("");
    SetChatMessage("system", "");
}

void Display::UpdateStatusBar(bool update_all) {}

void Display::SetEmotion(const char* emotion) { ESP_LOGW(TAG, "SetEmotion: %s", emotion); }

void Display::SetChatMessage(const char* role, const char* content) {
    ESP_LOGW(TAG, "Role:%s", role);
    ESP_LOGW(TAG, "     %s", content);
}

void Display::ClearChatMessages() {
    // Default empty implementation, override in subclasses if needed
}

void Display::SetTheme(Theme* theme) {
    current_theme_ = theme;
    Settings settings("display", true);
    settings.SetString("theme", theme->name());
}

void Display::SetPowerSaveMode(bool on) { ESP_LOGW(TAG, "SetPowerSaveMode: %d", on); }
