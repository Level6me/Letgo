#ifndef AUDIO_DIAGNOSTICS_H
#define AUDIO_DIAGNOSTICS_H

#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <cJSON.h>

enum class AudioSessionType {
    kUplink,    // 语音输入与发送
    kDownlink   // 语音接收与播放
};

struct AudioSessionReport {
    AudioSessionType type;
    uint32_t session_id = 0;
    int64_t start_time_ms = 0;
    uint32_t duration_ms = 0;

    // 通用指标
    uint32_t total_packets = 0;
    uint32_t processed_frames = 0;
    uint32_t dropped_or_failed = 0;

    // 下行专用指标 (接收与播放)
    uint32_t underrun_count = 0;         // 播放欠载卡顿次数
    uint32_t max_interval_ms = 0;        // 下行包最大到达间隔
    uint32_t avg_interval_ms = 0;        // 下行包平均到达间隔
    float jitter_ms = 0.0f;              // 网络抖动 (Jitter)
    uint32_t min_queue_depth = 0;        // 播放队列最低水位
    uint32_t max_decode_time_us = 0;     // 最大解码耗时 (微秒)
    uint32_t avg_decode_time_us = 0;     // 平均解码耗时 (微秒)
    uint32_t first_audio_delay_ms = 0;   // 首音起播延迟

    // 上行专用指标 (采集与发送)
    uint16_t avg_energy_rms = 0;         // 平均音频能量 RMS
    uint16_t max_energy_rms = 0;         // 最大音频能量 RMS

    // 综合评价 ("Smooth", "Jittery", "Stuttering", "Lagged", "Normal")
    std::string evaluation;
};

class AudioDiagnostics {
public:
    static AudioDiagnostics& GetInstance();

    // 上行 (录音发送) 追踪
    void StartUplinkSession();
    void RecordUplinkFrame(size_t samples, uint16_t rms);
    void RecordUplinkPacketSent(bool success);
    void RecordUplinkDrop(uint32_t count = 1);
    void EndUplinkSession();

    // 下行 (接收播放) 追踪
    void StartDownlinkSession();
    void RecordDownlinkPacketReceived(size_t bytes);
    void RecordDownlinkDecoded(int64_t decode_time_us);
    void RecordDownlinkPlayed(size_t current_queue_depth);
    void RecordDownlinkUnderrun();
    void EndDownlinkSession();

    // 查询与诊断导出接口 (支持 MCP 与 协议直接调用)
    cJSON* CreateReportCJson(int max_records = 5, bool verbose = false);
    std::string DumpJson(int max_records = 5, bool verbose = false);

private:
    AudioDiagnostics();
    ~AudioDiagnostics() = default;

    AudioDiagnostics(const AudioDiagnostics&) = delete;
    AudioDiagnostics& operator=(const AudioDiagnostics&) = delete;

    std::mutex mutex_;
    static constexpr size_t kMaxHistoryRecords = 8;
    std::deque<AudioSessionReport> history_sessions_;

    // 上行当前会话临时状态
    bool uplink_active_ = false;
    uint32_t current_uplink_id_ = 0;
    int64_t uplink_start_time_us_ = 0;
    uint32_t uplink_pcm_frames_ = 0;
    uint32_t uplink_sent_packets_ = 0;
    uint32_t uplink_drop_packets_ = 0;
    uint32_t uplink_failed_packets_ = 0;
    uint64_t uplink_rms_sum_ = 0;
    uint16_t uplink_max_rms_ = 0;

    // 下行当前会话临时状态
    bool downlink_active_ = false;
    uint32_t current_downlink_id_ = 0;
    int64_t downlink_start_time_us_ = 0;
    int64_t downlink_last_packet_time_us_ = 0;
    int64_t downlink_first_play_time_us_ = 0;
    uint32_t downlink_recv_packets_ = 0;
    uint32_t downlink_decoded_frames_ = 0;
    uint32_t downlink_played_frames_ = 0;
    uint32_t downlink_underrun_count_ = 0;
    uint32_t downlink_max_interval_ms_ = 0;
    uint64_t downlink_interval_sum_ms_ = 0;
    float downlink_jitter_estimate_ms_ = 0.0f;
    uint32_t downlink_min_queue_depth_ = 999;
    uint32_t downlink_max_decode_time_us_ = 0;
    uint64_t downlink_decode_time_sum_us_ = 0;
};

#endif // AUDIO_DIAGNOSTICS_H
