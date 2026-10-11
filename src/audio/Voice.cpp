#include "audio/Voice.h"

#include <algorithm>
#include <cmath>

#include "core/Note.h"

namespace piano {
namespace {

/// 力度 → 合成音色峰值（与 HTML 原版 pow(velocity,1.3)*0.23/(1+max(0,48-m)*0.007) 对齐）
float synthAmpFor(int midi, float vel) {
    const float lowAtten = 1.0f + static_cast<float>(std::max(0, 48 - midi)) * 0.007f;
    return std::pow(vel, 1.3f) * 0.23f / lowAtten;
}

/// 力度 → 采样音色峰值（HTML 原版 pow(velocity,1.4)*0.76）
float sampleAmpFor(float vel) {
    return std::pow(vel, 1.4f) * 0.76f;
}

/// 低通截止频率随时间下滑：起音明亮、随后收敛到基频的若干倍（模拟音板吸收高频）。
float cutoffStartFor(double hz, float vel) {
    const float base = static_cast<float>(hz) * (5.0f + vel * 13.0f);
    return std::min(18000.0f, std::max(1300.0f, base));
}
float cutoffEndFor(double hz, float vel) {
    const float base = static_cast<float>(hz) * (2.3f + vel * 3.0f);
    return std::min(10000.0f, std::max(750.0f, base));
}

/// 高音衰减更快（与 HTML 原版 decay = max(1.7, 5.8 - high*3.4) 一致）
float decaySecondsFor(int midi) {
    const float high = static_cast<float>(std::max(0, midi - 48)) / 60.0f;
    return std::max(1.7f, 5.8f - high * 3.4f);
}

void setupPan(Voice& v, int midi) {
    // 低音偏左、高音偏右，幅度限制在 ±0.6，避免单侧过重
    const float pan = clampf(static_cast<float>(midi - 64) / 80.0f, -0.6f, 0.6f);
    panGains(pan, v.panL, v.panR);
}

} // namespace

void Voice::startSynth(const WavetableBank& bank, double sampleRate, int midi,
                       float vel, Source src, std::uint64_t frame) {
    kind        = Kind::Synth;
    active      = true;
    held        = true;
    deferred    = false;
    released    = false;
    source      = src;
    note        = static_cast<std::uint8_t>(midi);
    velocity    = vel;
    startFrame  = frame;
    sample      = nullptr;

    const double hz = noteHz(midi);
    const Wavetable& table = bank.tableFor(hz);

    // 三个轻微失谐的振荡器叠加，制造弦组之间的拍频
    static const double kDetune[3] = { -0.8, 0.9, 2.5 };
    for (int i = 0; i < 3; ++i) {
        osc[i].configure(&table, hz, sampleRate, kDetune[i]);
        osc[i].reset(0.0 + static_cast<double>(i) * 0.1731);   // 相位错开，避免起音时叠加过冲
    }

    env.configure(static_cast<float>(sampleRate),
                  0.008f,                       // Attack：8ms，足够短但有淡入，避免咔哒
                  decaySecondsFor(midi),        // Decay：1.7~5.8s
                  0.018f,                       // Sustain：极低，符合钢琴自然衰减
                  0.23f);                       // Release：230ms
    env.noteOn();

    cutoffNow  = cutoffStartFor(hz, vel);
    cutoffEnd  = cutoffEndFor(hz, vel);
    cutoffCoef = std::pow(0.9995f, 16.0f);      // 每 16 样本迭代一次
    filter.reset();
    filter.setLowpass(cutoffNow, 0.55f, static_cast<float>(sampleRate));

    // 击弦噪声瞬态：极短的带通噪声，模拟琴槌接触琴弦
    noise.seed(0x2545F491u + static_cast<std::uint32_t>(midi) * 2654435761u);
    const float bpHz = clampf(static_cast<float>(hz) * 5.0f, 450.0f, 7600.0f);
    hammerFilter.reset();
    hammerFilter.setLowpass(bpHz, 0.9f, static_cast<float>(sampleRate));
    hammerLevel = 0.18f * vel;
    hammerCoef  = std::pow(0.001f, 1.0f / (0.035f * static_cast<float>(sampleRate)));

    ampScale = synthAmpFor(midi, vel);
    setupPan(*this, midi);
    lastL = lastR = 0.0f;
}

void Voice::startSample(const SampleBuffer* buffer, double sampleRate, int midi,
                        float vel, Source src, std::uint64_t frame) {
    kind        = Kind::Sample;
    active      = true;
    held        = true;
    deferred    = false;
    released    = false;
    source      = src;
    note        = static_cast<std::uint8_t>(midi);
    velocity    = vel;
    startFrame  = frame;
    sample      = buffer;

    env.configure(static_cast<float>(sampleRate),
                  0.009f,     // 9ms 淡入
                  0.01f,
                  1.0f,       // 采样自带衰减，包络保持满值
                  0.25f);     // 松键 250ms 淡出
    env.noteOn();

    samplePos = 0.0;
    // 采样音高与目标音高不同时按半音比变调
    const int anchor = buffer ? buffer->anchorNote : midi;
    sampleInc = buffer ? (buffer->sampleRate / sampleRate) * std::pow(2.0, (midi - anchor) / 12.0) : 1.0;

    ampScale = sampleAmpFor(vel);
    setupPan(*this, midi);
    lastL = lastR = 0.0f;
}

void Voice::hardReset() noexcept {
    active = false;
    held = false;
    deferred = false;
    released = false;
    env.hardStop();
    filter.reset();
    hammerFilter.reset();
    hammerLevel = 0.0f;
    sample = nullptr;
    samplePos = 0.0;
    lastL = lastR = 0.0f;
}

void Voice::renderAdd(float* out, std::uint32_t frames, float sampleRate) {
    if (!active) return;

    if (kind == Kind::Synth) {
        std::uint32_t i = 0;
        while (i < frames) {
            // 控制速率：每 16 个样本更新一次滤波器系数，避免每样本调用 tan()
            const std::uint32_t n = std::min<std::uint32_t>(16u, frames - i);
            cutoffNow = cutoffEnd + (cutoffNow - cutoffEnd) * cutoffCoef;
            filter.setLowpass(cutoffNow, 0.55f, sampleRate);

            for (std::uint32_t k = 0; k < n; ++k, ++i) {
                const float e = env.next();
                if (e <= 0.0f && !env.active()) {
                    active = false;
                    return;
                }

                float s = 0.0f;
                s += osc[0].next() * oscLevel[0];
                s += osc[1].next() * oscLevel[1];
                s += osc[2].next() * oscLevel[2];
                s = filter.lp(s);

                // 击弦瞬态不经过包络，是独立的短促脉冲
                float hammer = 0.0f;
                if (hammerLevel > 1e-5f) {
                    hammer = hammerFilter.bp(noise.next()) * hammerLevel;
                    hammerLevel *= hammerCoef;
                }

                const float dry = s * e * ampScale;
                const float l = (dry + hammer) * panL;
                const float r = (dry + hammer) * panR;
                out[i * 2]     += l;
                out[i * 2 + 1] += r;
                lastL = l;
                lastR = r;
            }
        }
        return;
    }

    // ---- 采样路径 ----
    const SampleBuffer* buf = sample;
    if (!buf) { active = false; return; }
    const double lastFrame = static_cast<double>(buf->frameCount()) - 2.0;

    for (std::uint32_t i = 0; i < frames; ++i) {
        const float e = env.next();
        if (e <= 0.0f && !env.active()) { active = false; return; }
        if (samplePos >= lastFrame)     { active = false; return; }

        float sl = 0.0f, sr = 0.0f;
        buf->sampleAt(samplePos, sl, sr);
        samplePos += sampleInc;

        const float a = e * ampScale;
        const float l = sl * a * panL;
        const float r = sr * a * panR;
        out[i * 2]     += l;
        out[i * 2 + 1] += r;
        lastL = l;
        lastR = r;
    }
}

} // namespace piano
