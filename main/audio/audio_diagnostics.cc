#include "audio_diagnostics.h"
#include "system_info.h"
#include <wifi_manager.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <cmath>
#include <algorithm>

#define TAG "AudioDiag"

AudioDiagnostics& AudioDiagnostics::GetInstance() {
    static AudioDiagnostics instance;
    return instance;
}

AudioDiagnostics::AudioDiagnostics() = default;

void AudioDiagnostics::StartUplinkSession() {
    std::lock_guard<std::mutex> lock(mutex_);
    uplink_active_ = true;
    current_uplink_id_++;
    uplink_start_time_us_ = esp_timer_get_time();
    uplink_pcm_frames_ = 0;
    uplink_sent_packets_ = 0;
    uplink_drop_packets_ = 0;
    uplink_failed_packets_ = 0;
    uplink_rms_sum_ = 0;
    uplink_max_rms_ = 0;
}

void AudioDiagnostics::RecordUplinkFrame(size_t samples, uint16_t rms) {
    if (!uplink_active_) return;
    uplink_pcm_frames_++;
    uplink_rms_sum_ += rms;
    if (rms > uplink_max_rms_) {
        uplink_max_rms_ = rms;
    }
}

void AudioDiagnostics::RecordUplinkPacketSent(bool success) {
    if (!uplink_active_) return;
    if (success) {
        uplink_sent_packets_++;
    } else {
        uplink_failed_packets_++;
    }
}

void AudioDiagnostics::RecordUplinkDrop(uint32_t count) {
    if (!uplink_active_) return;
    uplink_drop_packets_ += count;
}

void AudioDiagnostics::EndUplinkSession() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!uplink_active_) return;
    uplink_active_ = false;

    int64_t now = esp_timer_get_time();
    uint32_t duration_ms = static_cast<uint32_t>((now - uplink_start_time_us_) / 1000);

    AudioSessionReport report;
    report.type = AudioSessionType::kUplink;
    report.session_id = current_uplink_id_;
    report.start_time_ms = uplink_start_time_us_ / 1000;
    report.duration_ms = duration_ms;
    report.total_packets = uplink_sent_packets_;
    report.processed_frames = uplink_pcm_frames_;
    report.dropped_or_failed = uplink_drop_packets_ + uplink_failed_packets_;
    report.max_energy_rms = uplink_max_rms_;
    report.avg_energy_rms = (uplink_pcm_frames_ > 0) ? (uplink_rms_sum_ / uplink_pcm_frames_) : 0;

    if (report.dropped_or_failed > 2) {
        report.evaluation = "DropFrames";
    } else if (report.processed_frames > 20 && report.avg_energy_rms < 100) {
        report.evaluation = "LowVolume";
    } else {
        report.evaluation = "Smooth";
    }

    if (history_sessions_.size() >= kMaxHistoryRecords) {
        history_sessions_.pop_front();
    }
    history_sessions_.push_back(report);

    ESP_LOGI(TAG, "[Uplink #%lu] Dur: %lums | Sent: %lu pkts | Drop/Fail: %lu | Avg RMS: %u -> %s",
             (unsigned long)report.session_id, (unsigned long)report.duration_ms,
             (unsigned long)report.total_packets, (unsigned long)report.dropped_or_failed,
             report.avg_energy_rms, report.evaluation.c_str());
}

void AudioDiagnostics::StartDownlinkSession() {
    std::lock_guard<std::mutex> lock(mutex_);
    downlink_active_ = true;
    current_downlink_id_++;
    downlink_start_time_us_ = esp_timer_get_time();
    downlink_last_packet_time_us_ = 0;
    downlink_first_play_time_us_ = 0;
    downlink_recv_packets_ = 0;
    downlink_decoded_frames_ = 0;
    downlink_played_frames_ = 0;
    downlink_underrun_count_ = 0;
    downlink_max_interval_ms_ = 0;
    downlink_interval_sum_ms_ = 0;
    downlink_jitter_estimate_ms_ = 0.0f;
    downlink_min_queue_depth_ = 999;
    downlink_max_decode_time_us_ = 0;
    downlink_decode_time_sum_us_ = 0;
}

void AudioDiagnostics::RecordDownlinkPacketReceived(size_t bytes) {
    if (!downlink_active_) {
        StartDownlinkSession();
    }
    int64_t now = esp_timer_get_time();
    downlink_recv_packets_++;

    if (downlink_last_packet_time_us_ > 0) {
        int64_t delta_us = now - downlink_last_packet_time_us_;
        uint32_t interval_ms = static_cast<uint32_t>(delta_us / 1000);
        if (interval_ms > downlink_max_interval_ms_) {
            downlink_max_interval_ms_ = interval_ms;
        }
        downlink_interval_sum_ms_ += interval_ms;

        // RFC 3550 抖动滤波算法 (标称帧间隔 60ms)
        float diff = std::abs(static_cast<float>(interval_ms) - 60.0f);
        downlink_jitter_estimate_ms_ += (diff - downlink_jitter_estimate_ms_) / 16.0f;
    }
    downlink_last_packet_time_us_ = now;
}

void AudioDiagnostics::RecordDownlinkDecoded(int64_t decode_time_us) {
    if (!downlink_active_) return;
    downlink_decoded_frames_++;
    if (decode_time_us > downlink_max_decode_time_us_) {
        downlink_max_decode_time_us_ = static_cast<uint32_t>(decode_time_us);
    }
    downlink_decode_time_sum_us_ += decode_time_us;
}

void AudioDiagnostics::RecordDownlinkPlayed(size_t current_queue_depth) {
    if (!downlink_active_) return;
    downlink_played_frames_++;
    if (downlink_first_play_time_us_ == 0) {
        downlink_first_play_time_us_ = esp_timer_get_time();
    }
    if (current_queue_depth < downlink_min_queue_depth_) {
        downlink_min_queue_depth_ = static_cast<uint32_t>(current_queue_depth);
    }
}

void AudioDiagnostics::RecordDownlinkUnderrun() {
    if (!downlink_active_) return;
    downlink_underrun_count_++;
}

void AudioDiagnostics::EndDownlinkSession() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!downlink_active_) return;
    downlink_active_ = false;

    int64_t now = esp_timer_get_time();
    uint32_t duration_ms = static_cast<uint32_t>((now - downlink_start_time_us_) / 1000);

    AudioSessionReport report;
    report.type = AudioSessionType::kDownlink;
    report.session_id = current_downlink_id_;
    report.start_time_ms = downlink_start_time_us_ / 1000;
    report.duration_ms = duration_ms;
    report.total_packets = downlink_recv_packets_;
    report.processed_frames = downlink_played_frames_;
    report.underrun_count = downlink_underrun_count_;
    report.max_interval_ms = downlink_max_interval_ms_;
    report.avg_interval_ms = (downlink_recv_packets_ > 1)
                                ? static_cast<uint32_t>(downlink_interval_sum_ms_ / (downlink_recv_packets_ - 1))
                                : 0;
    report.jitter_ms = downlink_jitter_estimate_ms_;
    report.min_queue_depth = (downlink_min_queue_depth_ == 999) ? 0 : downlink_min_queue_depth_;
    report.max_decode_time_us = downlink_max_decode_time_us_;
    report.avg_decode_time_us = (downlink_decoded_frames_ > 0)
                                    ? static_cast<uint32_t>(downlink_decode_time_sum_us_ / downlink_decoded_frames_)
                                    : 0;
    report.first_audio_delay_ms = (downlink_first_play_time_us_ > downlink_start_time_us_)
                                      ? static_cast<uint32_t>((downlink_first_play_time_us_ - downlink_start_time_us_) / 1000)
                                      : 0;

    // 综合评价评级
    if (report.underrun_count > 2) {
        report.evaluation = "Frozen";
    } else if (report.underrun_count > 0) {
        report.evaluation = "Stuttering";
    } else if (report.max_interval_ms > 160 || report.jitter_ms > 20.0f) {
        report.evaluation = "Jittery";
    } else {
        report.evaluation = "Smooth";
    }

    if (history_sessions_.size() >= kMaxHistoryRecords) {
        history_sessions_.pop_front();
    }
    history_sessions_.push_back(report);

    ESP_LOGI(TAG, "[Downlink #%lu] Dur: %lums | Recv: %lu | Underruns: %lu | MaxGap: %lums | Jitter: %.1fms | TTFA: %lums -> %s",
             (unsigned long)report.session_id, (unsigned long)report.duration_ms,
             (unsigned long)report.total_packets, (unsigned long)report.underrun_count,
             (unsigned long)report.max_interval_ms, report.jitter_ms,
             (unsigned long)report.first_audio_delay_ms, report.evaluation.c_str());
}

cJSON* AudioDiagnostics::CreateReportCJson(int max_records, bool verbose) {
    std::lock_guard<std::mutex> lock(mutex_);

    cJSON* root = cJSON_CreateObject();

    // 系统运行状态快照
    cJSON* sys = cJSON_CreateObject();
    cJSON_AddNumberToObject(sys, "wifi_rssi", WifiManager::GetInstance().GetRssi());
    cJSON_AddNumberToObject(sys, "free_heap_kb", static_cast<double>(SystemInfo::GetFreeHeapSize() / 1024));
    cJSON_AddNumberToObject(sys, "min_free_heap_kb", static_cast<double>(SystemInfo::GetMinimumFreeHeapSize() / 1024));
    cJSON_AddNumberToObject(sys, "uptime_sec", static_cast<double>(esp_timer_get_time() / 1000000));
    cJSON_AddItemToObject(root, "system", sys);

    // 历史会话指标
    cJSON* sessions = cJSON_CreateArray();
    int count = 0;
    int total = static_cast<int>(history_sessions_.size());
    int start_idx = std::max(0, total - max_records);

    for (int i = start_idx; i < total; i++) {
        const auto& s = history_sessions_[i];
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "session_id", s.session_id);
        cJSON_AddStringToObject(item, "type", s.type == AudioSessionType::kUplink ? "uplink" : "downlink");
        cJSON_AddNumberToObject(item, "duration_ms", s.duration_ms);
        cJSON_AddStringToObject(item, "evaluation", s.evaluation.c_str());

        if (s.type == AudioSessionType::kDownlink) {
            cJSON_AddNumberToObject(item, "recv_packets", s.total_packets);
            cJSON_AddNumberToObject(item, "played_frames", s.processed_frames);
            cJSON_AddNumberToObject(item, "underrun_count", s.underrun_count);
            cJSON_AddNumberToObject(item, "max_gap_ms", s.max_interval_ms);
            cJSON_AddNumberToObject(item, "avg_gap_ms", s.avg_interval_ms);
            cJSON_AddNumberToObject(item, "jitter_ms", static_cast<int>(s.jitter_ms));
            cJSON_AddNumberToObject(item, "min_queue_depth", s.min_queue_depth);
            cJSON_AddNumberToObject(item, "ttfa_ms", s.first_audio_delay_ms);
            if (verbose) {
                cJSON_AddNumberToObject(item, "max_dec_time_us", s.max_decode_time_us);
                cJSON_AddNumberToObject(item, "avg_dec_time_us", s.avg_decode_time_us);
            }
        } else {
            cJSON_AddNumberToObject(item, "sent_packets", s.total_packets);
            cJSON_AddNumberToObject(item, "pcm_frames", s.processed_frames);
            cJSON_AddNumberToObject(item, "dropped_or_failed", s.dropped_or_failed);
            cJSON_AddNumberToObject(item, "avg_rms", s.avg_energy_rms);
            cJSON_AddNumberToObject(item, "max_rms", s.max_energy_rms);
        }
        cJSON_AddItemToArray(sessions, item);
    }
    cJSON_AddItemToObject(root, "recent_sessions", sessions);

    return root;
}

std::string AudioDiagnostics::DumpJson(int max_records, bool verbose) {
    cJSON* root = CreateReportCJson(max_records, verbose);
    if (!root) return "{}";
    char* str = cJSON_PrintUnformatted(root);
    std::string res = str ? str : "{}";
    if (str) cJSON_free(str);
    cJSON_Delete(root);
    return res;
}
