#pragma once
//
// audio/Dsp.h —— 基础 DSP 组件（无外部依赖，可离线单测）
//
// 包含：滤波器、噪声源、钢琴式包络、混音工具函数。
// 所有 process 函数均为「逐样本」调用，内部无分配、无分支预测失败热点。
//
#include <cmath>
#include <cstdint>

namespace piano {

inline constexpr float kPi = 3.14159265358979323846f;

inline float clampf(float v, float lo, float hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float dbToGain(float db) noexcept { return std::pow(10.0f, db * 0.05f); }

/// 软削波：|x| 很大时平滑趋于 ±1，避免数字溢出导致的刺耳爆音。
inline float softClip(float x) noexcept {
    // tanh 的廉价近似，误差 < 0.3%
    const float ax = std::fabs(x);
    if (ax < 0.5f) return x * (1.0f - 0.3333333f * x * x * 0.5f);
    const float e = std::exp(2.0f * ax);
    const float t = (e - 1.0f) / (e + 1.0f);
    return x < 0.0f ? -t : t;
}

/// 等功率声像：pan ∈ [-1, 1]，返回 (左增益, 右增益)。
inline void panGains(float pan, float& gl, float& gr) noexcept {
    const float a = (clampf(pan, -1.0f, 1.0f) + 1.0f) * 0.25f * kPi;
    gl = std::cos(a);
    gr = std::sin(a);
}

// ---------------------------------------------------------------------------
// TPT 状态变量滤波器（Zavalishin 拓扑）
// 相比双二阶 biquad，它在截止频率被音频速率调制时依然稳定，且每样本成本更低。
// ---------------------------------------------------------------------------
struct Svf {
    float ic1 = 0.0f, ic2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;

    /// 计算系数。低通与带通共用同一组系数，区别只在于读取 lp() 还是 bp() 的输出。
    void setLowpass(float cutoffHz, float q, float sr) noexcept {
        const float nyq = sr * 0.5f;
        cutoffHz = clampf(cutoffHz, 12.0f, nyq * 0.98f);
        const float g = std::tan(kPi * cutoffHz / sr);
        const float k = 1.0f / (q > 0.02f ? q : 0.02f);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    inline float lp(float v) noexcept {
        const float v3 = v - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return v2;
    }

    inline float bp(float v) noexcept {
        const float v3 = v - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return v1;
    }

    /// 语义别名：与 setLowpass 完全等价，仅表达「将读取带通输出」的意图。
    void setBandpass(float cutoffHz, float q, float sr) noexcept { setLowpass(cutoffHz, q, sr); }

    void reset() noexcept { ic1 = ic2 = 0.0f; }
};

// ---------------------------------------------------------------------------
// 一阶低通（用于阻尼 / 平滑参数，例如混响阻尼）
// ---------------------------------------------------------------------------
struct OnePole {
    float z = 0.0f, a = 1.0f;
    void setCutoff(float hz, float sr) noexcept {
        a = 1.0f - std::exp(-2.0f * kPi * clampf(hz, 1.0f, sr * 0.49f) / sr);
    }
    inline float lp(float x) noexcept { z += a * (x - z); return z; }
    void reset() noexcept { z = 0.0f; }
};

// ---------------------------------------------------------------------------
// 噪声源：xorshift32。每个 Voice 独立一份状态，音频线程内无共享、无锁。
// ---------------------------------------------------------------------------
struct NoiseGen {
    std::uint32_t s = 0x9E3779B9u;

    void seed(std::uint32_t v) noexcept { s = v ? v : 0x9E3779B9u; }

    inline float next() noexcept {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<float>(static_cast<std::int32_t>(s)) * (1.0f / 2147483648.0f);
    }
};

// ---------------------------------------------------------------------------
// 钢琴式包络
//
// 与常见的「持续音」ADSR 不同：钢琴击弦后能量按指数衰减，
// 因此 Sustain 电平取得极低（默认 0.018），Decay 时间按音高在 1.7~5.8s 之间。
// 松键后进入 Release，指数收敛到释放底噪以下即判定结束。
// ---------------------------------------------------------------------------
struct Envelope {
    enum class Stage { Idle, Attack, Decay, Sustain, Release };

    Stage stage = Stage::Idle;
    float level = 0.0f;

    float attackInc   = 1.0f;
    float decayCoef   = 1.0f;
    float sustainLvl  = 0.018f;
    float releaseCoef = 0.999f;
    float releaseFloor = 1e-4f;

    /// attackSec：到达峰值的时间；decaySec：从峰值指数衰减到 sustain 的时间。
    void configure(float sr, float attackSec, float decaySec, float sustain, float releaseSec) noexcept {
        const float a = attackSec > 1e-5f ? attackSec : 1e-5f;
        attackInc = 1.0f / (a * sr);

        sustainLvl = clampf(sustain, 1e-5f, 0.999f);
        const float d = decaySec > 1e-3f ? decaySec : 1e-3f;
        decayCoef = std::pow(sustainLvl, 1.0f / (d * sr));

        const float r = releaseSec > 1e-3f ? releaseSec : 1e-3f;
        releaseCoef = std::pow(releaseFloor, 1.0f / (r * sr));
    }

    /// 强制以指定时长释放（用于偷声 / 紧急止音）。
    void forceRelease(float sr, float releaseSec) noexcept {
        if (stage == Stage::Idle) return;
        const float r = releaseSec > 1e-4f ? releaseSec : 1e-4f;
        releaseCoef = std::pow(releaseFloor, 1.0f / (r * sr));
        stage = Stage::Release;
    }

    void noteOn() noexcept { stage = Stage::Attack; }
    void noteOff() noexcept { if (stage != Stage::Idle) stage = Stage::Release; }

    /// 立刻静音（硬停，不做淡出）。
    void hardStop() noexcept { stage = Stage::Idle; level = 0.0f; }

    bool active() const noexcept { return stage != Stage::Idle; }
    bool releasing() const noexcept { return stage == Stage::Release; }

    inline float next() noexcept {
        switch (stage) {
        case Stage::Attack:
            level += attackInc;
            if (level >= 1.0f) { level = 1.0f; stage = Stage::Decay; }
            break;
        case Stage::Decay:
            level *= decayCoef;
            if (level <= sustainLvl) { level = sustainLvl; stage = Stage::Sustain; }
            break;
        case Stage::Sustain:
            // 钢琴不像风琴那样「按住就永远响」：越过 sustain 电平后继续指数衰减，
            // 直到低于释放底噪才真正结束。约 3 倍 Decay 时间后自然消失。
            level *= decayCoef;
            if (level <= releaseFloor) { level = 0.0f; stage = Stage::Idle; }
            break;
        case Stage::Release:
            level *= releaseCoef;
            if (level <= releaseFloor) { level = 0.0f; stage = Stage::Idle; }
            break;
        case Stage::Idle:
            level = 0.0f;
            break;
        }
        return level;
    }
};

} // namespace piano
