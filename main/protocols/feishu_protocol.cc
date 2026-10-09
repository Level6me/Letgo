#include "feishu_protocol.h"
#include "application.h"
#include "board.h"
#include "settings.h"
#include "system_info.h"
#include "display.h"
#include <assets/lang_config.h>
#include <wifi_manager.h>

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
    // 1. 读取 NVS 历史网关配置及配对状态
    Settings settings("feishu", false);
    is_paired_ = settings.GetBool("paired", false);
    gateway_ip_ = settings.GetString("gw_ip");
    gateway_port_ = settings.GetInt("gw_port", DEFAULT_FEISHU_PORT);

    if (is_paired_ && !gateway_ip_.empty()) {
        ESP_LOGI(TAG, "Attempting direct connect to paired gateway: %s:%d", gateway_ip_.c_str(), gateway_port_);
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
        char rx_buf[512];
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
                        cJSON* root = cJSON_Parse(rx_buf);
                        if (root) {
                            auto service = cJSON_GetObjectItem(root, "service");
                            if (cJSON_IsString(service) && strcmp(service->valuestring, "feishu_passport") == 0) {
                                struct sockaddr_in* sin = (struct sockaddr_in*)&src_addr;
                                char ip_str[32];
                                inet_ntop(AF_INET, &sin->sin_addr, ip_str, sizeof(ip_str));

                                auto port_item = cJSON_GetObjectItem(root, "ws_port");
                                int port = cJSON_IsNumber(port_item) ? port_item->valueint : DEFAULT_FEISHU_PORT;

                                auto name_item = cJSON_GetObjectItem(root, "name");
                                std::string gw_name = cJSON_IsString(name_item) ? name_item->valuestring : "飞书控制台";

                                ESP_LOGI(TAG, "Discovered Feishu gateway: %s (%s:%d)", gw_name.c_str(), ip_str, port);

                                // 若已配对且当前未连接，直接连接已配对网关
                                if (self->is_paired_ && self->gateway_ip_ == ip_str) {
                                    close(sock);
                                    cJSON_Delete(root);
                                    self->ConnectToGateway(ip_str, port);
                                    vTaskDelay(pdMS_TO_TICKS(5000));
                                    continue;
                                }

                                // 记录并更新到候选网关列表中
                                bool is_new = false;
                                {
                                    std::lock_guard<std::mutex> lock(self->gateways_mutex_);
                                    bool exists = false;
                                    bool is_history_paired = (self->is_paired_ && self->gateway_ip_ == ip_str);
                                    for (auto& gw : self->discovered_gateways_) {
                                        if (gw.ip == ip_str && gw.port == port) {
                                            gw.name = gw_name;
                                            gw.is_paired = is_history_paired;
                                            exists = true;
                                            break;
                                        }
                                    }
                                    if (!exists) {
                                        FeishuGateway new_gw{gw_name, ip_str, port, is_history_paired};
                                        if (is_history_paired) {
                                            self->discovered_gateways_.insert(self->discovered_gateways_.begin(), new_gw);
                                        } else {
                                            self->discovered_gateways_.push_back(new_gw);
                                        }
                                        is_new = true;
                                    }
                                    self->pending_gw_ip_ = ip_str;
                                    self->pending_gw_port_ = port;
                                    self->pending_gw_name_ = gw_name;
                                }

                                if (is_new && self->on_gateways_changed_) {
                                    auto list = self->GetDiscoveredGateways();
                                    Application::GetInstance().Schedule([self, list]() {
                                        if (self->on_gateways_changed_) {
                                            self->on_gateways_changed_(list);
                                        }
                                    });
                                }
                            }
                            cJSON_Delete(root);
                        }
                    }
                    close(sock);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(4000));
        }
    }, "feishu_discovery", 4096, this, 5, &discovery_task_handle_);
}

std::vector<FeishuGateway> FeishuProtocol::GetDiscoveredGateways() const {
    std::lock_guard<std::mutex> lock(gateways_mutex_);
    return discovered_gateways_;
}

void FeishuProtocol::ConnectToGatewayByIndex(size_t index) {
    std::string ip;
    int port = DEFAULT_FEISHU_PORT;
    std::string name;
    {
        std::lock_guard<std::mutex> lock(gateways_mutex_);
        if (index < discovered_gateways_.size()) {
            ip = discovered_gateways_[index].ip;
            port = discovered_gateways_[index].port;
            name = discovered_gateways_[index].name;
            pending_gw_ip_ = ip;
            pending_gw_port_ = port;
            pending_gw_name_ = name;
        }
    }
    if (!ip.empty()) {
        ESP_LOGI(TAG, "Connecting to selected gateway [%d] %s (%s:%d)", (int)index, name.c_str(), ip.c_str(), port);
        ConnectToGateway(ip, port);
    }
}

void FeishuProtocol::SetOnGatewaysChanged(std::function<void(const std::vector<FeishuGateway>&)> cb) {
    on_gateways_changed_ = std::move(cb);
}

void FeishuProtocol::RequestProjectList() {
    ESP_LOGI(TAG, "Requesting project list from Feishu gateway");
    SendText("{\"type\":\"project_list\"}");
}

void FeishuProtocol::SwitchProject(const std::string& project_name) {
    ESP_LOGI(TAG, "Switching to project: %s", project_name.c_str());
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "switch_project");
    cJSON_AddStringToObject(root, "project", project_name.c_str());
    char* json_str = cJSON_PrintUnformatted(root);
    SendText(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
}

void FeishuProtocol::SetOnProjectListReceived(std::function<void(const std::vector<std::string>&, const std::string&)> cb) {
    on_project_list_received_ = std::move(cb);
}

void FeishuProtocol::ConnectSelectedGateway() {
    if (pending_gw_ip_.empty()) return;
    ESP_LOGI(TAG, "Connecting to selected gateway for pairing: %s:%d", pending_gw_ip_.c_str(), pending_gw_port_);

    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        display->ShowNotification("⏳ 正在发起配对申请\n请在飞书确认...", 8000);
        display->SetStatus("请求配对中...");
    }

    ConnectToGateway(pending_gw_ip_, pending_gw_port_);
}

void FeishuProtocol::SendPairRequest() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "pair_request");
    cJSON_AddStringToObject(root, "mac", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(root, "ip", WifiManager::GetInstance().GetIpAddress().c_str());
    cJSON_AddStringToObject(root, "device_name", "FoloToy AI Passport");

    char* json_str = cJSON_PrintUnformatted(root);
    SendText(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Pair request sent: MAC=%s, IP=%s",
             SystemInfo::GetMacAddress().c_str(), WifiManager::GetInstance().GetIpAddress().c_str());
}

void FeishuProtocol::TriggerDiscovery() {
    if (connected_) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(gateways_mutex_);
        discovered_gateways_.clear();
        pending_gw_ip_.clear();
    }
    if (on_gateways_changed_) {
        on_gateways_changed_({});
    }
    ESP_LOGI(TAG, "Triggering active UDP gateway discovery probe...");
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock >= 0) {
        int broadcast = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
        struct sockaddr_in dest_addr;
        dest_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(DEFAULT_FEISHU_PORT);
        const char* probe = "DISCOVER_FEISHU_PASSPORT";
        sendto(sock, probe, strlen(probe), 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
        close(sock);
    }
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
            if (len >= 2) {
                // 收到飞书服务端下发的 16kHz 16bit 单声道 PCM 流，直接塞入播放队列
                size_t sample_count = len / sizeof(int16_t);
                const int16_t* pcm_samples = reinterpret_cast<const int16_t*>(data);
                std::vector<int16_t> pcm_vec(pcm_samples, pcm_samples + sample_count);
                Application::GetInstance().GetAudioService().PushPcmToPlaybackQueue(std::move(pcm_vec));
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
        if (!is_paired_) {
            SendPairRequest();
        } else {
            SendHandshake();
        }

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
    cJSON_AddStringToObject(root, "mac", SystemInfo::GetMacAddress().c_str());
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
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "voice_start");
    cJSON_AddStringToObject(root, "mac", SystemInfo::GetMacAddress().c_str());
    char* json_str = cJSON_PrintUnformatted(root);
    SendText(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    is_audio_channel_opened_ = true;
}

void FeishuProtocol::SendStopListening() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "voice_end");
    cJSON_AddStringToObject(root, "mac", SystemInfo::GetMacAddress().c_str());
    char* json_str = cJSON_PrintUnformatted(root);
    SendText(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
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

    if (type_str == "pair_ack") {
        auto status = cJSON_GetObjectItem(root, "status");
        if (cJSON_IsString(status)) {
            std::string st = status->valuestring;
            if (st == "approved") {
                is_paired_ = true;
                pending_gw_ip_.clear();
                pending_gw_name_.clear();

                Settings settings("feishu", true);
                settings.SetString("gw_ip", gateway_ip_);
                settings.SetInt("gw_port", gateway_port_);
                settings.SetBool("paired", true);
                auto token = cJSON_GetObjectItem(root, "token");
                if (cJSON_IsString(token)) {
                    settings.SetString("gw_token", token->valuestring);
                }

                Application::GetInstance().Schedule([display]() {
                    if (display) {
                        display->SetStatus("飞书控制台就绪");
                        display->ShowNotification("🎉 配对成功！\n长按OK键开始对讲", 5000);
                        display->SetEmotion("neutral");
                    }
                });
            } else if (st == "pending") {
                Application::GetInstance().Schedule([display]() {
                    if (display) {
                        display->SetStatus("等待飞书审批...");
                        display->ShowNotification("⏳ 等待飞书管理员审批...", 5000);
                    }
                });
            } else if (st == "rejected") {
                is_paired_ = false;
                Application::GetInstance().Schedule([display]() {
                    if (display) {
                        display->SetStatus("配对已被拒绝");
                        display->ShowNotification("❌ 飞书管理员已拒绝配对", 5000);
                        display->SetEmotion("sad");
                    }
                });
            }
        }
    } else if (type_str == "feishu_voice_sent") {
        auto status_item = cJSON_GetObjectItem(root, "status");
        std::string status = cJSON_IsString(status_item) ? status_item->valuestring : "success";
        auto text_item = cJSON_GetObjectItem(root, "text");
        std::string text = cJSON_IsString(text_item) ? text_item->valuestring : "";

        Application::GetInstance().Schedule([display, status, text]() {
            if (!display) return;
            if (status == "success" || status == "ok") {
                char tip[128];
                if (!text.empty() && text != "(未检测到有效语音)") {
                    snprintf(tip, sizeof(tip), "✅ 已发送至飞书!\n「%s」", text.c_str());
                    display->SetChatMessage("user", text.c_str());
                } else {
                    snprintf(tip, sizeof(tip), "✅ 语音已发送至飞书!\n等待AI回复中...");
                }
                display->ShowNotification(tip, 4000);
                display->SetStatus(Lang::Strings::FEISHU_SENT_AWAITING);
                display->SetEmotion("neutral");
            } else {
                display->ShowNotification("❌ 发送至飞书失败", 3000);
                display->SetStatus(Lang::Strings::FEISHU_SEND_FAILED);
                display->SetEmotion("sad");
                Application::GetInstance().SetFeishuAwaitingReply(false);
            }
        });
    } else if (type_str == "voice_reply_start") {
        Application::GetInstance().Schedule([display]() {
            Application::GetInstance().SetDeviceState(kDeviceStateSpeaking);
            if (display) {
                display->SetStatus(Lang::Strings::FEISHU_PLAYING);
                display->SetEmotion("speaking");
            }
        });
    } else if (type_str == "voice_reply_end") {
        Application::GetInstance().Schedule([display]() {
            Application::GetInstance().SetFeishuAwaitingReply(false);
            if (display) {
                display->SetEmotion("neutral");
                display->SetStatus(Lang::Strings::FEISHU_CONSOLE_READY);
                display->SetChatMessage("system", Lang::Strings::FEISHU_HOLD_OK_TALK);
            }
        });
    } else if (type_str == "dashboard_sync") {
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
    } else if (type_str == "project_list") {
        std::vector<std::string> projects;
        std::string current_project;
        auto current_item = cJSON_GetObjectItem(root, "current");
        if (cJSON_IsString(current_item)) {
            current_project = current_item->valuestring;
        }
        auto list_item = cJSON_GetObjectItem(root, "projects");
        if (cJSON_IsArray(list_item)) {
            cJSON* elem = nullptr;
            cJSON_ArrayForEach(elem, list_item) {
                if (cJSON_IsString(elem)) {
                    projects.push_back(elem->valuestring);
                }
            }
        }
        if (on_project_list_received_) {
            Application::GetInstance().Schedule([this, projects, current_project]() {
                if (on_project_list_received_) {
                    on_project_list_received_(projects, current_project);
                }
            });
        }
    } else if (type_str == "project_switched") {
        auto proj = cJSON_GetObjectItem(root, "project");
        std::string name = cJSON_IsString(proj) ? proj->valuestring : "";
        Application::GetInstance().Schedule([display, name]() {
            if (display) {
                std::string tip = "✅ 已切换至项目:\n" + name;
                display->ShowNotification(tip.c_str(), 2500);
                std::string st = "[" + name + "]";
                display->SetStatus(st.c_str());
            }
        });
    } else if (type_str == "handshake_ack") {
        if (display) {
            Application::GetInstance().Schedule([display]() {
                display->SetStatus("飞书控制台就绪");
                display->SetEmotion("neutral");
            });
        }
    } else if (type_str == "ai_state") {
        auto state = cJSON_GetObjectItem(root, "state");
        auto detail = cJSON_GetObjectItem(root, "detail");
        std::string detail_str = cJSON_IsString(detail) ? detail->valuestring : "";
        if (cJSON_IsString(state)) {
            std::string st = state->valuestring;
            Application::GetInstance().Schedule([display, st, detail_str]() {
                auto& app = Application::GetInstance();
                if (st == "listening") {
                    app.SetDeviceState(kDeviceStateListening);
                    if (display) {
                        display->SetStatus("正在聆听...");
                        display->SetEmotion("listening");
                    }
                } else if (st == "thinking") {
                    if (display) {
                        if (detail_str.find("Transcribing") != std::string::npos) {
                            display->SetStatus("飞书识别中...");
                        } else if (detail_str.find("Analyzing") != std::string::npos) {
                            display->SetStatus("已发送飞书，AI思考中...");
                        } else {
                            display->SetStatus("飞书处理中...");
                        }
                        display->SetEmotion("thinking");
                    }
                } else if (st == "speaking") {
                    app.SetDeviceState(kDeviceStateSpeaking);
                    if (display) {
                        display->SetStatus(Lang::Strings::FEISHU_PLAYING);
                        display->SetEmotion("speaking");
                    }
                } else {
                    app.SetFeishuAwaitingReply(false);
                    app.SetDeviceState(kDeviceStateIdle);
                    if (display) {
                        display->SetStatus(Lang::Strings::FEISHU_CONSOLE_READY);
                        display->SetEmotion("neutral");
                        display->SetChatMessage("system", Lang::Strings::FEISHU_HOLD_OK_TALK);
                    }
                }
            });
        }
    } else if (type_str == "ai_speech_start" || type_str == "tts") {
        auto text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text) && display) {
            std::string msg = text->valuestring;
            Application::GetInstance().Schedule([display, msg]() {
                display->SetChatMessage("assistant", msg.c_str());
                display->SetEmotion("speaking");
                display->SetStatus(Lang::Strings::FEISHU_PLAYING);
                Application::GetInstance().SetDeviceState(kDeviceStateSpeaking);
            });
        }
    } else if (type_str == "ai_speech_end") {
        Application::GetInstance().Schedule([display]() {
            Application::GetInstance().SetFeishuAwaitingReply(false);
            Application::GetInstance().SetDeviceState(kDeviceStateIdle);
            if (display) {
                display->SetEmotion("neutral");
                display->SetStatus(Lang::Strings::FEISHU_CONSOLE_READY);
                display->SetChatMessage("system", Lang::Strings::FEISHU_HOLD_OK_TALK);
            }
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
