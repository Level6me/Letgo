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
    static constexpr size_t kNumBands = 21;

    // 分析 16kHz PCM 音频数据，输出 21 频段幅值 (2 ~ 34 像素高度)
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

        // 11 个特征频段定义 (0~8000Hz 全频段细分: 覆盖低音、中频核心人声到高频泛音)
        static constexpr size_t kNumBins = 11;
        static const struct {
            uint8_t start_bin;
            uint8_t end_bin;
            float gain;
        } kBinDefs[kNumBins] = {
            { 1,  2, 3.2f}, // Bin 0:  125~250 Hz (Sub-bass / 低频底鼓与重低音)
            { 3,  4, 3.0f}, // Bin 1:  375~500 Hz (Bass / 男声厚度基频)
            { 5,  6, 2.8f}, // Bin 2:  625~750 Hz (Low-mid / 人声主体频带)
            { 7,  9, 2.8f}, // Bin 3:  875~1125 Hz (Mid / 元音第一共振峰)
            {10, 13, 2.9f}, // Bin 4: 1250~1625 Hz (Mid-high / 核心语音频带)
            {14, 18, 3.1f}, // Bin 5: 1750~2250 Hz (Presence / 人声明亮度与第二共振峰)
            {19, 25, 3.4f}, // Bin 6: 2375~3125 Hz (Clarity / 清晰度与辅音)
            {26, 34, 3.8f}, // Bin 7: 3250~4250 Hz (Crisp / 齿音与清脆感)
            {35, 43, 4.3f}, // Bin 8: 4375~5375 Hz (Treble / 高频泛音)
            {44, 52, 4.8f}, // Bin 9: 5500~6500 Hz (Air / 细腻空气感)
            {53, 62, 5.5f}, // Bin 10: 6625~7750 Hz (Brilliance / 超高频泛音)
        };

        // 计算 11 个频段最大能量并采用高灵敏度宽动态范围对数压缩 (Dynamic Range Audio Mapping)
        // 彻底解决原先灵敏度过低、说话无波动反应的问题；微弱声音细腻起伏，大声饱满有力且不爆顶
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

            // 宽动态范围高灵敏度对数压缩: 将 max_val * gain 映射至 0 ~ 32px 动态高度
            // 采用自适应 log10 曲线，在弱信号(0.01~0.05)下具备优异斜率响应，中强信号自然渐进饱和
            float scaled = max_val * kBinDefs[b].gain * 3.6f;
            float norm = 0.0f;
            if (scaled > 0.002f) {
                norm = log10f(1.0f + scaled * 8.0f) / 0.95f;
                if (norm > 1.0f) norm = 1.0f;
            }
            raw_heights[b] = norm * 32.0f;
        }

        // 软静音门限 (Noise Gate): 仅滤除纯底噪，超低阈值保证人声呼吸/轻语也能灵敏起伏
        bool is_silent = (total_energy < 0.0015f);

        // 双向对称镜像排布映射表 (21 根柱子: 外侧高频/空气感 -> 中间为人声与低频核心)
        static const uint8_t kMirrorMap[kNumBands] = {
            10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
        };

        for (size_t i = 0; i < kNumBands; i++) {
            int target_h = 2;
            if (!is_silent) {
                uint8_t bin_idx = kMirrorMap[i];
                target_h = 2 + (int)roundf(raw_heights[bin_idx]);
                if (target_h > 34) target_h = 34;
                if (target_h < 2) target_h = 2;
            }

            // 平滑下落衰减 (Peak Decay): 每帧下落 2px，保证 20Hz 刷新时如水波般流动丝滑
            if (target_h >= decay_bands[i]) {
                decay_bands[i] = (uint8_t)target_h;
            } else {
                if (decay_bands[i] > 3) {
                    decay_bands[i] -= 2;
                } else {
                    decay_bands[i] = 2;
                }
            }
            bands_out[i] = decay_bands[i];
        }
    }
};

#endif // _AUDIO_SPECTRUM_H_
