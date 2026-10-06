#ifndef _FEISHU_PROTOCOL_H_
#define _FEISHU_PROTOCOL_H_

#include "protocol.h"
#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <string>

class FeishuProtocol : public Protocol {
public:
    FeishuProtocol();
    ~FeishuProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;

    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;
    void SendAbortSpeaking(AbortReason reason) override;

    // 飞书专属控制事件
    bool SendButtonEvent(const std::string& button, const std::string& action);
    bool SendEmergencyStop();
    bool SendBargeIn();
    bool IsConnected() const { return connected_; }
    const std::string& GetGatewayIp() const { return gateway_ip_; }
    void TriggerDiscovery();
    void SendPairRequest();
    void ConnectSelectedGateway();
    bool HasPendingGateway() const { return !pending_gw_ip_.empty() && !connected_; }
    const std::string& GetPendingGatewayName() const { return pending_gw_name_; }

private:
    std::unique_ptr<WebSocket> websocket_;
    std::string gateway_ip_;
    int gateway_port_ = 8765;
    std::string pending_gw_ip_;
    int pending_gw_port_ = 8765;
    std::string pending_gw_name_;
    bool is_audio_channel_opened_ = false;
    bool connected_ = false;
    bool is_paired_ = false;

    TaskHandle_t discovery_task_handle_ = nullptr;
    esp_timer_handle_t ping_timer_ = nullptr;

    bool SendText(const std::string& text) override;
    void StartDiscovery();
    void ConnectToGateway(const std::string& ip, int port);
    void HandleServerJson(const char* data, size_t len);
    void SendHandshake();
    void SendPing();
};

#endif  // _FEISHU_PROTOCOL_H_
