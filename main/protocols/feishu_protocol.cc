#include "feishu_protocol.h"
#include "application.h"
#include "board.h"
#include "settings.h"
#include "system_info.h"
#include "display.h"

#include <esp_log.h>
#include <cJSON.h>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define TAG "FeishuProto"
#define DEFAULT_FEISHU_PORT 8765

FeishuProtocol::FeishuProtocol() {
    server_sample_rate_ = 16000;
    server_frame_duration_ = 60;

    // 创建 15s 心跳保活定时器
    esp_timer_create_args_t ping_timer_args = {
        .callback = [](void* arg) {
            auto self = static_cast<FeishuProtocol*>(arg);
            self->SendPing();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "feishu_ping",
        .skip_unhandled_events = true
    };
    esp_timer_create(&ping_timer_args, &ping_timer_);
}

FeishuProtocol::~FeishuProtocol() {
    if (ping_timer_ != nullptr) {
        esp_timer_stop(ping_timer_);
        esp_timer_delete(ping_timer_);
    }
    if (discovery_task_handle_ != nullptr) {
        vTaskDelete(discovery_task_handle_);
    }
}

bool FeishuProtocol::Start() {
    // 1. 读取 NVS 历史网关 IP
    Settings settings("feishu", false);
    gateway_ip_ = settings.GetString("gw_ip");
    gateway_port_ = settings.GetInt("gw_port", DEFAULT_FEISHU_PORT);

    if (!gateway_ip_.empty()) {
        ESP_LOGI(TAG, "Attempting fast direct connect to saved gateway: %s:%d", gateway_ip_.c_str(), gateway_port_);
        ConnectToGateway(gateway_ip_, gateway_port_);
    }

    // 2. 启动 UDP 服务自发现
    StartDiscovery();
    return true;
}

void FeishuProtocol::StartDiscovery() {
    if (discovery_task_handle_ != nullptr) {
        return;
    }

    xTaskCreate([](void* arg) {
        auto self = static_cast<FeishuProtocol*>(arg);
        char rx_buf[256];
        const char* probe = "DISCOVER_FEISHU_PASSPORT";

        while (true) {
            if (!self->connected_) {
                int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
                if (sock >= 0) {
                    int broadcast = 1;
                    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

                    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
                    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

                    struct sockaddr_in dest_addr;
                    dest_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
                    dest_addr.sin_family = AF_INET;
                    dest_addr.sin_port = htons(DEFAULT_FEISHU_PORT);

                    sendto(sock, probe, strlen(probe), 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));

                    struct sockaddr_storage src_addr;
                    socklen_t addr_len = sizeof(src_addr);
                    int len = recvfrom(sock, rx_buf, sizeof(rx_buf) - 1, 0, (struct sockaddr*)&src_addr, &addr_len);
                    if (len > 0) {
                        rx_buf[len] = '\0';
                        if (strstr(rx_buf, "feishu_passport")) {
                            struct sockaddr_in* sin = (struct sockaddr_in*)&src_addr;
                            char ip_str[32];
                            inet_ntop(AF_INET, &sin->sin_addr, ip_str, sizeof(ip_str));
                            int port = DEFAULT_FEISHU_PORT;

                            ESP_LOGI(TAG, "Discovered Feishu gateway at: %s:%d", ip_str, port);
                            close(sock);

                            // 保存到 NVS
                            Settings settings("feishu", true);
                            settings.SetString("gw_ip", ip_str);
                            settings.SetInt("gw_port", port);

                            self->ConnectToGateway(ip_str, port);
                            vTaskDelay(pdMS_TO_TICKS(5000));
                            continue;
                        }
                    }
                    close(sock);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(4000));
        }
    }, "feishu_discovery", 4096, this, 5, &discovery_task_handle_);
}

void FeishuProtocol::ConnectToGateway(const std::string& ip, int port) {
    if (websocket_ && websocket_->IsConnected()) {
        return;
    }

    gateway_ip_ = ip;
    gateway_port_ = port;

    std::string url = "ws://" + ip + ":" + std::to_string(port) + "/ws/passport";
    ESP_LOGI(TAG, "Connecting to Feishu Gateway: %s", url.c_str());

    auto network = Board::GetInstance().GetNetwork();
    if (!network) return;

    websocket_ = network->CreateWebSocket(1);
    if (!websocket_) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return;
    }

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        last_incoming_time_ = std::chrono::steady_clock::now();
        if (binary) {
            if (on_incoming_audio_ != nullptr && len > 0) {
                // 下行 TTS 二进制音频流
                on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                    .sample_rate = server_sample_rate_,
                    .frame_duration = server_frame_duration_,
                    .timestamp = 0,
                    .payload = std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + len)
                }));
            }
        } else {
            HandleServerJson(data, len);
        }
    });

    websocket_->OnDisconnected([this]() {
        ESP_LOGW(TAG, "WebSocket disconnected from Feishu gateway");
        connected_ = false;
        if (ping_timer_) {
            esp_timer_stop(ping_timer_);
        }
        if (on_disconnected_) {
            on_disconnected_();
        }
    });

    if (auto connected = websocket_->Connect(url.c_str()); connected) {
        ESP_LOGI(TAG, "Connected to Feishu Gateway successfully!");
        connected_ = true;
        SendHandshake();

        // 启动 15 秒保活 Ping
        if (ping_timer_) {
            esp_timer_start_periodic(ping_timer_, 15000000);
        }

        if (on_connected_) {
            on_connected_();
        }
    } else {
        ESP_LOGW(TAG, "Failed to connect to Feishu Gateway");
        connected_ = false;
    }
}

void FeishuProtocol::SendHandshake() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "handshake");
    cJSON_AddStringToObject(root, "device_id", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(root, "version", "2.0.0");
    cJSON_AddStringToObject(root, "board", "ai-passport");

    char* json_str = cJSON_PrintUnformatted(root);
    SendText(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
}

void FeishuProtocol::SendPing() {
    if (connected_) {
        SendText("{\"type\":\"ping\"}");
    }
}

bool FeishuProtocol::SendText(const std::string& text) {
    if (!websocket_ || !websocket_->IsConnected()) {
        return false;
    }
    return websocket_->Send(text);
}

bool FeishuProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!websocket_ || !websocket_->IsConnected() || !packet) {
        return false;
    }
    return websocket_->Send((const char*)packet->payload.data(), packet->payload.size(), true);
}

bool FeishuProtocol::OpenAudioChannel() {
    is_audio_channel_opened_ = true;
    if (on_audio_channel_opened_) {
        on_audio_channel_opened_();
    }
    return true;
}

void FeishuProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;
    is_audio_channel_opened_ = false;
    if (on_audio_channel_closed_) {
        on_audio_channel_closed_();
    }
}

bool FeishuProtocol::IsAudioChannelOpened() const {
    return is_audio_channel_opened_ && connected_;
}

void FeishuProtocol::SendStartListening(ListeningMode mode) {
    (void)mode;
    SendBargeIn();
    SendText("{\"type\":\"voice_start\"}");
    is_audio_channel_opened_ = true;
}

void FeishuProtocol::SendStopListening() {
    SendText("{\"type\":\"voice_end\"}");
    is_audio_channel_opened_ = false;
}

void FeishuProtocol::SendAbortSpeaking(AbortReason reason) {
    (void)reason;
    SendBargeIn();
}

bool FeishuProtocol::SendBargeIn() {
    return SendText("{\"type\":\"barge_in\"}");
}

bool FeishuProtocol::SendButtonEvent(const std::string& button, const std::string& action) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "button_event");
    cJSON_AddStringToObject(root, "button", button.c_str());
    cJSON_AddStringToObject(root, "action", action.c_str());

    char* json_str = cJSON_PrintUnformatted(root);
    bool ret = SendText(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return ret;
}

bool FeishuProtocol::SendEmergencyStop() {
    return SendButtonEvent("down", "long_press");
}

void FeishuProtocol::HandleServerJson(const char* data, size_t len) {
    cJSON* root = cJSON_ParseWithLength(data, len);
    if (!root) return;

    auto type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }

    std::string type_str = type->valuestring;
    auto display = Board::GetInstance().GetDisplay();

    if (type_str == "dashboard_sync") {
        auto project = cJSON_GetObjectItem(root, "project");
        auto status = cJSON_GetObjectItem(root, "status");
        if (display && cJSON_IsString(project)) {
            std::string info = "[" + std::string(project->valuestring) + "]";
            if (cJSON_IsString(status)) {
                info += " " + std::string(status->valuestring);
            }
            Application::GetInstance().Schedule([display, info]() {
                display->SetStatus(info.c_str());
            });
        }
    } else if (type_str == "ai_state") {
        auto state = cJSON_GetObjectItem(root, "state");
        if (cJSON_IsString(state)) {
            std::string st = state->valuestring;
            Application::GetInstance().Schedule([display, st]() {
                auto& app = Application::GetInstance();
                if (st == "listening") {
                    app.SetDeviceState(kDeviceStateListening);
                } else if (st == "thinking") {
                    if (display) {
                        display->SetStatus("AI思考中...");
                        display->SetEmotion("thinking");
                    }
                } else if (st == "speaking") {
                    app.SetDeviceState(kDeviceStateSpeaking);
                } else {
                    app.SetDeviceState(kDeviceStateIdle);
                }
            });
        }
    } else if (type_str == "ai_speech_start" || type_str == "tts") {
        auto text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text) && display) {
            std::string msg = text->valuestring;
            Application::GetInstance().Schedule([display, msg]() {
                display->SetChatMessage("assistant", msg.c_str());
                Application::GetInstance().SetDeviceState(kDeviceStateSpeaking);
            });
        }
    } else if (type_str == "ai_speech_end") {
        Application::GetInstance().Schedule([]() {
            Application::GetInstance().SetDeviceState(kDeviceStateIdle);
        });
    } else if (type_str == "alert_popup") {
        auto title = cJSON_GetObjectItem(root, "title");
        auto content = cJSON_GetObjectItem(root, "content");
        std::string t = cJSON_IsString(title) ? title->valuestring : "ALERT";
        std::string c = cJSON_IsString(content) ? content->valuestring : "";
        Application::GetInstance().Schedule([t, c]() {
            Application::GetInstance().Alert(t.c_str(), c.c_str(), "danger");
        });
    }

    if (on_incoming_json_) {
        on_incoming_json_(root);
    }
    cJSON_Delete(root);
}
