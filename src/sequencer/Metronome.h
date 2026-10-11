#pragma once
//
// sequencer/Metronome.h —— 节拍器
//
// 与 HTML 原版一样采用「前瞻调度」的思想，但执行位置在音频回调内，
// 因此咔哒声落在精确的采样点上。UI 线程只写原子参数，音频线程负责重锚。
//
#include <atomic>
#include <cstdint>

#include "audio/Mixer.h"

namespace piano {

class Metronome {
public:
    void setSampleRate(double sr) noexcept { sampleRate_ = sr > 1000.0 ? sr : 48000.0; }

    void setEnabled(bool on) noexcept { enabled_.store(on, std::memory_order_relaxed); }
    bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }

    void setBpm(int bpm) noexcept { bpm_.store(bpm < 20 ? 20 : (bpm > 400 ? 400 : bpm), std::memory_order_relaxed); }
    int  bpm() const noexcept { return bpm_.load(std::memory_order_relaxed); }

    void setBeatsPerBar(int n) noexcept { beatsPerBar_.store(n < 1 ? 1 : n, std::memory_order_relaxed); }
    int  beatsPerBar() const noexcept { return beatsPerBar_.load(std::memory_order_relaxed); }

    /// 当前拍号（0 起）。UI 用它做视觉闪烁。
    int  currentBeat() const noexcept { return lastBeat_.load(std::memory_order_relaxed); }
    std::uint64_t lastClickFrame() const noexcept { return lastClickFrame_.load(std::memory_order_relaxed); }

    /// 作为 AudioDevice::SchedulerFn 使用。
    static std::uint32_t process(void* user, Mixer& mixer,
                                 std::uint64_t blockStartFrame, std::uint32_t maxFrames);

private:
    double sampleRate_ = 48000.0;
    double framesPerBeat_ = 24000.0;
    double nextClickFrame_ = 0.0;
    int    beat_ = 0;
    bool   wasEnabled_ = false;

    std::atomic<bool>     enabled_{false};
    std::atomic<int>      bpm_{100};
    std::atomic<int>      beatsPerBar_{4};
    std::atomic<int>      lastBeat_{0};
    std::atomic<std::uint64_t> lastClickFrame_{0};
};

} // namespace piano
