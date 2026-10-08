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

        // 15 个频段映射 (16kHz 采样率下，每个频点分辨率 125Hz，0~8000Hz)
        static const struct {
            uint8_t start_bin;
            uint8_t end_bin;
            float gain;
        } kBandDefs[kNumBands] = {
            { 1,  1, 45.0f}, // Band 0:  125 Hz (Sub-bass)
            { 2,  2, 42.0f}, // Band 1:  250 Hz (Bass)
            { 3,  3, 40.0f}, // Band 2:  375 Hz
            { 4,  4, 38.0f}, // Band 3:  500 Hz (Low-mid)
            { 5,  6, 36.0f}, // Band 4:  625~750 Hz
            { 7,  8, 35.0f}, // Band 5:  875~1000 Hz (Mid)
            { 9, 11, 35.0f}, // Band 6:  1125~1375 Hz
            {12, 15, 36.0f}, // Band 7:  1500~1875 Hz
            {16, 20, 38.0f}, // Band 8:  2000~2500 Hz (Upper-mid)
            {21, 26, 40.0f}, // Band 9:  2625~3250 Hz
            {27, 32, 42.0f}, // Band 10: 3375~4000 Hz (Presence)
            {33, 39, 45.0f}, // Band 11: 4125~4875 Hz
            {40, 47, 48.0f}, // Band 12: 5000~5875 Hz
            {48, 55, 52.0f}, // Band 13: 6000~6875 Hz (Brilliance)
            {56, 63, 56.0f}, // Band 14: 7000~8000 Hz (Highs)
        };

        for (size_t b = 0; b < kNumBands; b++) {
            float max_val = 0.0f;
            for (uint8_t k = kBandDefs[b].start_bin; k <= kBandDefs[b].end_bin; k++) {
                if (mag[k] > max_val) {
                    max_val = mag[k];
                }
            }
            int height = 2 + (int)(max_val * kBandDefs[b].gain * 32.0f);
            if (height > 32) height = 32;
            if (height < 2) height = 2;

            // 平滑下落衰减 (Peak Decay)
            if (height >= decay_bands[b]) {
                decay_bands[b] = (uint8_t)height;
            } else {
                if (decay_bands[b] > 3) {
                    decay_bands[b] -= 2; // 每帧平滑下落 2px
                } else {
                    decay_bands[b] = 2;
                }
            }
            bands_out[b] = decay_bands[b];
        }
    }
};

#endif // _AUDIO_SPECTRUM_H_
