#pragma once
//
// audio/Mixer.h —— 复音混音器（纯 DSP，不含任何设备 / 线程代码）
//
// 拆分的意义：Mixer 可以在测试里离线渲染，验证复音上限、偷声策略、
// 延音踏板行为、包络收敛等，不需要真的开声卡。
//
// 音频线程约束（render / noteOn / noteOff 内部）：
//   - 不分配内存、不加锁、不做 I/O、不调用可能阻塞的系统函数
//   - 线程间只通过原子变量与无锁队列通信
//
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "audio/Dsp.h"
#include "audio/Reverb.h"
#include "audio/SampleBank.h"
#include "audio/Voice.h"
#include "audio/Wavetable.h"
#include "core/Event.h"
#include "core/VisualEvent.h"

namespace piano {

class Mixer {
public:
    /// 声部槽位数。实际复音上限（kPolyphonyLimit）更低，多出的槽位用于
    /// 让被偷声的声部走完 6ms 淡出，而不是被硬切断。
    static constexpr int kMaxVoices      = 128;
    static constexpr int kPolyphonyLimit = 64;
    static constexpr int kDeclickTails   = 24;
    static constexpr int kClicks         = 8;

    enum class EngineMode : std::uint8_t {
        Auto      = 0,   // 有采样用采样，否则合成
        SampleOnly = 1,  // 强制采样（无采样时回退合成）
        SynthOnly  = 2   // 强制合成
    };

    void prepare(double sampleRate);
    void reset();

    // ---------------- 实时安全 API ----------------

    /// 触发一个音符。返回被分配的声部（可能为 nullptr，表示资源耗尽）。
    Voice* noteOn(int midi, float velocity, Source source);

    /// 松开一个音符。延音踏板踩下时进入 deferred 状态而非立即释放。
    void noteOff(int midi, Source source);

    /// 延音踏板。下降沿会在下一次 render 时释放所有 deferred 声部。
    void setSustain(bool on) noexcept { sustainOn_.store(on, std::memory_order_relaxed); }

    /// 全部止音。hard = true 时立即静音（用于「停止」按钮）。
    void allNotesOff(bool hard);

    /// 只释放指定来源的声部（例如停止曲目播放，但不打断用户正在弹的音）。
    void releaseSource(Source source, bool hard);

    /// 节拍器咔哒声。
    void triggerClick(float freq, float amp, float decaySeconds);

    /// 渲染一块交错立体声样本。
    void render(float* interleaved, std::uint32_t frames);

    // ---------------- 参数 ----------------

    void setMasterVolume(float v) noexcept { masterVol_.store(clampf(v, 0.0f, 2.0f), std::memory_order_relaxed); }
    float masterVolume() const noexcept { return masterVol_.load(std::memory_order_relaxed); }

    void setReverbMix(float v) noexcept { reverb_.setMix(v); }
    float reverbMix() const noexcept { return reverb_.mix(); }

    void setEngineMode(EngineMode m) noexcept { engineMode_.store(m, std::memory_order_relaxed); }
    EngineMode engineMode() const noexcept { return engineMode_.load(std::memory_order_relaxed); }

    void setSampleBank(const SampleBank* bank) noexcept { bank_ = bank; }
    const SampleBank* sampleBank() const noexcept { return bank_; }

    /// 视觉事件出口（音频线程 → UI 线程）。启动前设置、停止后清除，运行中不更改；
    /// 线程启动/退出本身构成 happens-before，普通指针即可，TSan 干净。
    void setVisualEventQueue(VisualEventQueue* q) noexcept { visualQueue_ = q; }

    // ---------------- 状态查询（UI 线程） ----------------

    std::uint64_t currentFrame() const noexcept { return frameCounter_.load(std::memory_order_relaxed); }
    int  activeVoiceCount() const noexcept { return activeVoices_.load(std::memory_order_relaxed); }
    float peakLevel() const noexcept { return peak_.load(std::memory_order_relaxed); }
    bool sustainActive() const noexcept { return sustainOn_.load(std::memory_order_relaxed); }
    int  hardReuseCount() const noexcept { return hardReuse_.load(std::memory_order_relaxed); }

    /// 某个音高当前被「按下」的次数（用于琴键高亮，与 HTML 版行为一致）。
    int visualCount(int midi) const noexcept {
        if (midi < 0 || midi > 127) return 0;
        return visual_[static_cast<std::size_t>(midi)].load(std::memory_order_relaxed);
    }
    int visualTotal() const noexcept {
        int n = 0;
        for (int i = 0; i < 128; ++i) n += visualCount(i);
        return n;
    }
    bool noteSounding(int midi) const noexcept {
        for (const auto& v : voices_)
            if (v.active && !v.released && v.note == midi) return true;
        return false;
    }

    double sampleRate() const noexcept { return sampleRate_; }
    const WavetableBank& wavetables() const noexcept { return wavetables_; }

private:
    struct DeclickTail {
        bool  active = false;
        float l = 0.0f, r = 0.0f, stepL = 0.0f, stepR = 0.0f;
        int   remain = 0;
        void start(float sl, float sr);
        void renderAdd(float* out, std::uint32_t frames);
    };

    struct ClickVoice {
        bool   active = false;
        double phase = 0.0, inc = 0.0, incEnd = 0.0, incCoef = 1.0;
        float  amp = 0.0f, env = 0.0f, coef = 0.0f;
        void renderAdd(float* out, std::uint32_t frames, float sampleRate);
    };

    struct Compressor {
        float env = 0.0f, atkCoef = 0.1f, relCoef = 0.01f;
        float thresholdDb = -19.0f, ratio = 3.2f;
        void prepare(float sr);
        inline float process(float x) noexcept {
            const float ax = std::fabs(x);
            env += (ax > env ? atkCoef : relCoef) * (ax - env);
            if (env <= 1e-6f) return x;
            const float db = 20.0f * std::log10(env);
            if (db <= thresholdDb) return x;
            const float gainDb = (thresholdDb - db) * (1.0f - 1.0f / ratio);
            return x * dbToGain(gainDb);
        }
        void reset() { env = 0.0f; }
    };

    Voice* allocateSlot();
    Voice* pickVictim();
    void   stealOne();
    void   flushDeferred();
    Voice* startVoice(int midi, float vel, Source source);
    int    liveVoiceCount() const;   // 含正在释放的
    int    heldVoiceCount() const;   // 仅统计仍在按住/挂起的，用于复音上限判定

    double sampleRate_ = 48000.0;

    std::array<Voice, kMaxVoices> voices_{};
    std::array<DeclickTail, kDeclickTails> tails_{};
    std::array<ClickVoice, kClicks> clicks_{};
    int clickCursor_ = 0;

    WavetableBank wavetables_;
    Reverb        reverb_;
    Compressor    comp_;
    const SampleBank* bank_ = nullptr;

    std::atomic<std::uint64_t> frameCounter_{0};
    std::atomic<int>   activeVoices_{0};
    std::atomic<float> peak_{0.0f};
    std::atomic<bool>  sustainOn_{false};
    std::atomic<int>   hardReuse_{0};
    bool sustainPrev_ = false;

    // UI 高亮计数（与音频线程无强一致性要求，仅用于显示）
    std::array<std::atomic<int>, 128> visual_{};

    std::atomic<float> masterVol_{0.75f};
    std::atomic<EngineMode> engineMode_{EngineMode::Auto};

    VisualEventQueue* visualQueue_ = nullptr;   // 见 setVisualEventQueue
};

} // namespace piano
