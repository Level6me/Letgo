#ifndef _AUDIO_SPECTRUM_H_
#define _AUDIO_SPECTRUM_H_

#include <cstdint>
#include <cstddef>
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class AudioSpectrum {
public:
    static constexpr size_t kFftSize = 128;
    static constexpr size_t kNumBands = 15;

    // 分析 16kHz PCM 音频数据，输出 15 频段幅值 (2 ~ 32 像素高度)
    static void Analyze(const int16_t* pcm, size_t samples, uint8_t bands_out[kNumBands], uint8_t decay_bands[kNumBands]) {
        if (pcm == nullptr || samples < kFftSize) {
            for (size_t i = 0; i < kNumBands; i++) {
                if (decay_bands[i] > 3) decay_bands[i] -= 2;
                else decay_bands[i] = 2;
                bands_out[i] = decay_bands[i];
            }
            return;
        }

        // 取最近的 128 个采样点
        const int16_t* input = pcm + (samples - kFftSize);

        float real[kFftSize];
        float imag[kFftSize];

        // 汉宁窗 (Hanning Window) 预处理
        for (size_t i = 0; i < kFftSize; i++) {
            float hanning = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(kFftSize - 1)));
            real[i] = ((float)input[i] / 32768.0f) * hanning;
            imag[i] = 0.0f;
        }

        // 128 点 Cooley-Tukey Radix-2 FFT: 位倒序排列 (Bit-reversal)
        size_t j = 0;
        for (size_t i = 0; i < kFftSize - 1; i++) {
            if (i < j) {
                std::swap(real[i], real[j]);
                std::swap(imag[i], imag[j]);
            }
            size_t k = kFftSize / 2;
            while (k <= j) {
                j -= k;
                k /= 2;
            }
            j += k;
        }

        // 蝶形运算 (Butterfly computation)
        for (size_t len = 2; len <= kFftSize; len <<= 1) {
            float angle = -2.0f * (float)M_PI / (float)len;
            float wlen_r = cosf(angle);
            float wlen_i = sinf(angle);
            for (size_t i = 0; i < kFftSize; i += len) {
                float w_r = 1.0f;
                float w_i = 0.0f;
                for (size_t k = 0; k < len / 2; k++) {
                    float u_r = real[i + k];
                    float u_i = imag[i + k];
                    float v_r = real[i + k + len / 2] * w_r - imag[i + k + len / 2] * w_i;
                    float v_i = real[i + k + len / 2] * w_i + imag[i + k + len / 2] * w_r;
                    real[i + k] = u_r + v_r;
                    imag[i + k] = u_i + v_i;
                    real[i + k + len / 2] = u_r - v_r;
                    imag[i + k + len / 2] = u_i - v_i;
                    float next_w_r = w_r * wlen_r - w_i * wlen_i;
                    float next_w_i = w_r * wlen_i + w_i * wlen_r;
                    w_r = next_w_r;
                    w_i = next_w_i;
                }
            }
        }

        // 计算 64 个频点幅值: mag[k] = sqrt(real^2 + imag^2)
        float mag[64];
        for (size_t k = 0; k < 64; k++) {
            mag[k] = sqrtf(real[k] * real[k] + imag[k] * imag[k]);
        }

        // 8 个特征频段定义 (0~8000Hz 覆盖超低音、重低音、中频人声到高频泛音)
        static constexpr size_t kNumBins = 8;
        static const struct {
            uint8_t start_bin;
            uint8_t end_bin;
            float gain;
        } kBinDefs[kNumBins] = {
            { 1,  2, 45.0f}, // Bin 0: 125~250 Hz (Bass / 低音基准)
            { 3,  4, 40.0f}, // Bin 1: 375~500 Hz (Low-Mid / 男低音/底鼓)
            { 5,  7, 36.0f}, // Bin 2: 625~875 Hz (Mid / 人声主体)
            { 8, 12, 35.0f}, // Bin 3: 1000~1500 Hz (Upper Mid / 核心人声)
            {13, 19, 36.0f}, // Bin 4: 1625~2375 Hz (Presence / 人声明亮度)
            {20, 28, 40.0f}, // Bin 5: 2500~3500 Hz (Clarity / 清晰度)
            {29, 40, 46.0f}, // Bin 6: 3625~5000 Hz (Treble / 高频泛音)
            {41, 62, 54.0f}, // Bin 7: 5125~7750 Hz (Air / 通透气声)
        };

        // 计算 8 个主频段原始能量值
        float raw_heights[kNumBins];
        float total_energy = 0.0f;
        for (size_t b = 0; b < kNumBins; b++) {
            float max_val = 0.0f;
            for (uint8_t k = kBinDefs[b].start_bin; k <= kBinDefs[b].end_bin; k++) {
                if (mag[k] > max_val) {
                    max_val = mag[k];
                }
            }
            total_energy += max_val;
            raw_heights[b] = max_val * kBinDefs[b].gain * 30.0f;
        }

        // 软静音门限 (Noise Gate): 抑制待机麦克风底噪或微弱环境杂音
        // 阈值设定在 ~0.025f，低于门限时直接进入 2px 静息基线状态
        bool is_silent = (total_energy < 0.028f);

        // 方案 B：双向对称镜像排布映射表 (15 根柱子: 外侧高频/低频均衡 -> 正中心为人声/主低频核心)
        // 映射索引: [7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6, 7]
        static const uint8_t kMirrorMap[kNumBands] = {
            7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6, 7
        };

        for (size_t i = 0; i < kNumBands; i++) {
            int target_h = 2;
            if (!is_silent) {
                uint8_t bin_idx = kMirrorMap[i];
                target_h = 2 + (int)raw_heights[bin_idx];
                if (target_h > 32) target_h = 32;
                if (target_h < 2) target_h = 2;
            }

            // 平滑下落衰减 (Peak Decay)
            if (target_h >= decay_bands[i]) {
                decay_bands[i] = (uint8_t)target_h;
            } else {
                if (decay_bands[i] > 3) {
                    decay_bands[i] -= 2; // 每帧平滑下落 2px
                } else {
                    decay_bands[i] = 2;
                }
            }
            bands_out[i] = decay_bands[i];
        }
    }
};

#endif // _AUDIO_SPECTRUM_H_
