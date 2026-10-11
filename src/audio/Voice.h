#pragma once
//
// audio/Voice.h —— 单个发声体
//
// 两条音色路径：
//   1. Synth  —— 加法合成波表 + 低通滤波 + 击弦噪声瞬态（离线可用，无外部资源）
//   2. Sample —— 播放钢琴采样并按音高变调（需要 assets/samples/ 有文件）
//
// 生命周期：Active(按键) → Released(松键，包络进入释放)
//                        → Deferred(松键但延音踏板仍踩下，暂不释放)
// 所有渲染函数在音频线程调用，内部不分配内存、不加锁。
//
#include <cstdint>

#include "audio/Dsp.h"
#include "audio/SampleBank.h"
#include "audio/Wavetable.h"
#include "core/Event.h"

namespace piano {

class WavetableBank;

struct Voice {
    enum class Kind : std::uint8_t { Synth = 0, Sample = 1 };

    // ---- 生命周期 ----
    bool          active    = false;
    bool          held      = false;   // 琴键仍按住
    bool          deferred  = false;   // 已松键，但被延音踏板挂住
    bool          released  = false;   // 包络已进入 Release 阶段
    Kind          kind      = Kind::Synth;
    Source        source    = Source::User;
    std::uint8_t  note      = 0;
    float         velocity  = 0.0f;
    std::uint64_t startFrame = 0;

    // ---- 合成路径 ----
    Envelope    env;
    WavetableOsc osc[3];
    float       oscLevel[3] = { 1.0f, 0.66f, 0.31f };
    Svf         filter;
    float       cutoffNow   = 1000.0f;
    float       cutoffEnd   = 800.0f;
    float       cutoffCoef  = 0.999f;   // 每 16 样本更新一次系数时使用
    NoiseGen    noise;
    Svf         hammerFilter;
    float       hammerLevel = 0.0f;
    float       hammerCoef  = 1.0f;

    // ---- 采样路径 ----
    const SampleBuffer* sample = nullptr;
    double      samplePos = 0.0;
    double      sampleInc = 1.0;

    // ---- 公共 ----
    float ampScale = 0.2f;          // 力度与音高决定的峰值增益
    float panL = 0.7071f, panR = 0.7071f;
    float lastL = 0.0f, lastR = 0.0f;   // 最后一次输出，用于硬复用时的去咔哒

    /// 启动一个合成音色。
    void startSynth(const WavetableBank& bank, double sampleRate, int midi,
                    float vel, Source src, std::uint64_t frame);

    /// 启动一个采样音色。
    void startSample(const SampleBuffer* buffer, double sampleRate, int midi,
                     float vel, Source src, std::uint64_t frame);

    /// 累加式渲染到交错立体声缓冲。返回后若 active 变为 false 表示该声部已结束。
    void renderAdd(float* interleaved, std::uint32_t frames, float sampleRate);

    /// 立即复位（硬复用前调用）。
    void hardReset() noexcept;

    bool sounding() const noexcept { return active; }
};

} // namespace piano
