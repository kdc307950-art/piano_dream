#pragma once
//
// audio/Wavetable.h —— 分级波表（mipmap）振荡器
//
// 音色来源与 HTML 原版一致：16 次谐波加法合成，谐波幅度 1/i^1.65，
// 每 3 次谐波衰减 0.78（模拟钢琴弦振动的非谐性 / 音板吸收）。
//
// 关键点：直接按公式求和需要每样本数十次 sin()，成本过高。
// 这里预生成 16 张波表（谐波数 1..16），按基频选择不超过奈奎斯特的
// 最大谐波层，既避免混叠又保持低 CPU 占用。
//
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "audio/Dsp.h"

namespace piano {

inline constexpr int kMaxHarmonics = 16;
inline constexpr int kTableSize    = 2048;

struct Wavetable {
    std::vector<float> data;   // 长度 kTableSize + 1（末位复制首样本，省一次取模）

    inline float sample(float phase) const noexcept {
        const float x  = phase * static_cast<float>(kTableSize);
        const int   i0 = static_cast<int>(x) & (kTableSize - 1);
        const float f  = x - std::floor(x);
        return data[i0] + (data[i0 + 1] - data[i0]) * f;
    }
};

class WavetableBank {
public:
    void build(double sampleRate) {
        nyquist_ = sampleRate * 0.5;
        tables_.resize(kMaxHarmonics + 1);

        for (int h = 1; h <= kMaxHarmonics; ++h) {
            Wavetable& t = tables_[h];
            t.data.assign(kTableSize + 1, 0.0f);

            double peak = 0.0;
            for (int n = 0; n < kTableSize; ++n) {
                const double theta = 2.0 * 3.14159265358979323846 * n / kTableSize;
                double acc = 0.0;
                for (int i = 1; i <= h; ++i) {
                    double amp = 1.0 / std::pow(static_cast<double>(i), 1.65);
                    if (i % 3 == 0) amp *= 0.78;
                    acc += amp * std::sin(theta * i);
                }
                t.data[n] = static_cast<float>(acc);
                peak = std::max(peak, std::fabs(acc));
            }
            if (peak > 1e-12) {
                const float inv = static_cast<float>(1.0 / peak);
                for (float& s : t.data) s *= inv;
            }
            t.data[kTableSize] = t.data[0];
        }
    }

    /// 按基频选取无混叠的谐波层。
    const Wavetable& tableFor(double freq) const noexcept {
        int h = kMaxHarmonics;
        if (freq > 1.0)
            h = static_cast<int>(nyquist_ * 0.92 / freq);
        h = h < 1 ? 1 : (h > kMaxHarmonics ? kMaxHarmonics : h);
        return tables_[h];
    }

    int harmonicCountFor(double freq) const noexcept {
        int h = kMaxHarmonics;
        if (freq > 1.0)
            h = static_cast<int>(nyquist_ * 0.92 / freq);
        return h < 1 ? 1 : (h > kMaxHarmonics ? kMaxHarmonics : h);
    }

    bool ready() const noexcept { return !tables_.empty(); }

private:
    double nyquist_ = 24000.0;
    std::vector<Wavetable> tables_;
};

/// 单振荡器：相位累加 + 线性插值读表。
struct WavetableOsc {
    const Wavetable* table = nullptr;
    double phase = 0.0;
    double inc   = 0.0;

    inline void configure(const Wavetable* t, double freq, double sampleRate, double detuneCents) noexcept {
        table = t;
        const double f = freq * std::pow(2.0, detuneCents / 1200.0);
        inc = (f > 0.0) ? f / sampleRate : 0.0;
    }

    inline void reset(double phase0) noexcept {
        phase = phase0 - std::floor(phase0);
    }

    inline float next() noexcept {
        if (!table || table->data.empty()) return 0.0f;
        const float v = table->sample(static_cast<float>(phase));
        phase += inc;
        if (phase >= 1.0) phase -= std::floor(phase);
        return v;
    }
};

} // namespace piano
