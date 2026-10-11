#pragma once
//
// audio/Reverb.h —— Schroeder/Freeverb 式混响
//
// HTML 原版用 ConvolverNode + 程序生成的噪声脉冲响应（2.4s 指数衰减）做空间感。
// 在 C++ 里用同样的卷积需要 2.4s * 48k * 2ch ≈ 1.15MB 的 IR 与 FFT 卷积；
// 这里改用 Freeverb 结构（8 梳状 + 4 全通 / 声道），效果接近、CPU 低一个数量级。
//
#include <cmath>
#include <cstddef>
#include <vector>
#include <atomic>

#include "audio/Dsp.h"

namespace piano {

class Reverb {
public:
    void prepare(double sampleRate) {
        const double k = sampleRate / 44100.0;
        static const int combTune[8]   = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
        static const int apTune[4]     = { 556, 441, 341, 225 };
        static const int stereoSpread  = 23;

        for (int i = 0; i < 8; ++i) {
            combL_[i].resize(static_cast<std::size_t>(combTune[i] * k));
            combR_[i].resize(static_cast<std::size_t>((combTune[i] + stereoSpread) * k));
            combL_[i].feedback = 0.84f;
            combR_[i].feedback = 0.84f;
        }
        for (int i = 0; i < 4; ++i) {
            apL_[i].resize(static_cast<std::size_t>(apTune[i] * k));
            apR_[i].resize(static_cast<std::size_t>((apTune[i] + stereoSpread) * k));
        }
        sampleRate_ = sampleRate;
        prepared_ = true;
    }

    void reset() {
        for (int i = 0; i < 8; ++i) { combL_[i].clear(); combR_[i].clear(); }
        for (int i = 0; i < 4; ++i) { apL_[i].clear();  apR_[i].clear();  }
    }

    /// mix ∈ [0, 1]，0 表示完全干声。
    void setMix(float mix) noexcept { mix_.store(clampf(mix, 0.0f, 1.0f), std::memory_order_relaxed); }
    float mix() const noexcept { return mix_.load(std::memory_order_relaxed); }

    void setDamping(float d) noexcept {
        damping_ = clampf(d, 0.0f, 1.0f);
        for (int i = 0; i < 8; ++i) { combL_[i].damp = damping_; combR_[i].damp = damping_; }
    }

    /// 原地处理一对立体声样本，返回已混合的输出。
    inline void process(float& l, float& r) noexcept {
        const float mix = mix_.load(std::memory_order_relaxed);
        if (!prepared_ || mix <= 1e-4f) return;

        const float input = (l + r) * 0.015f;   // 送入增益，避免反馈回路过载

        float outL = 0.0f, outR = 0.0f;
        for (int i = 0; i < 8; ++i) {
            outL += combL_[i].process(input);
            outR += combR_[i].process(input);
        }
        for (int i = 0; i < 4; ++i) {
            outL = apL_[i].process(outL);
            outR = apR_[i].process(outR);
        }

        // HTML 版：wet ≈ mix*0.66，dry ≈ 1 - mix*0.32
        const float wet = mix * 0.66f * 3.0f;   // Freeverb 输出偏小，补一个系数
        const float dry = 1.0f - mix * 0.32f;
        l = l * dry + outL * wet;
        r = r * dry + outR * wet;
    }

private:
    struct Comb {
        std::vector<float> buf;
        std::size_t idx = 0;
        float feedback = 0.84f;
        float damp = 0.2f;
        float store = 0.0f;

        void resize(std::size_t n) { buf.assign(n < 4 ? 4 : n, 0.0f); idx = 0; store = 0.0f; }
        void clear() { std::fill(buf.begin(), buf.end(), 0.0f); idx = 0; store = 0.0f; }

        inline float process(float in) noexcept {
            const float out = buf[idx];
            store = out * (1.0f - damp) + store * damp;
            buf[idx] = in + store * feedback;
            if (++idx >= buf.size()) idx = 0;
            return out;
        }
    };

    struct Allpass {
        std::vector<float> buf;
        std::size_t idx = 0;
        float feedback = 0.5f;

        void resize(std::size_t n) { buf.assign(n < 4 ? 4 : n, 0.0f); idx = 0; }
        void clear() { std::fill(buf.begin(), buf.end(), 0.0f); idx = 0; }

        inline float process(float in) noexcept {
            const float b = buf[idx];
            const float out = -in + b;
            buf[idx] = in + b * feedback;
            if (++idx >= buf.size()) idx = 0;
            return out;
        }
    };

    Comb combL_[8], combR_[8];
    Allpass apL_[4], apR_[4];
    double sampleRate_ = 48000.0;
    std::atomic<float> mix_{0.0f};
    float  damping_ = 0.22f;
    bool   prepared_ = false;
};

} // namespace piano
