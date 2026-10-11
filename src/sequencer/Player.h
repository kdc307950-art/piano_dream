#pragma once
//
// sequencer/Player.h —— 曲目演奏器（采样点精确调度）
//
// 与 HTML 原版「setInterval(25ms) + 140ms 前瞻」的做法不同：
// 这里把曲目的每个音符展开成带「绝对采样帧」的事件表，由音频回调按帧驱动，
// 因此节奏抖动为 0（不再受浏览器定时器 / 系统调度粒度影响）。
//
// 线程模型（重要）：
//   actions_ / song_ / cursor_ 只由音频线程在 process() 临界区内访问。
//   UI 线程要修改它们时，必须走 waitForAudioIdle()：
//     1. 先把 playing_ 置为 false（音频线程随即会在入口处提前返回）
//     2. 自旋等待 busy_ 变为 false，确认音频线程已经离开临界区
//     3. 此时独占访问，安全地重建事件表
//   代价是一次播放开始前最多等待一个音频块（约 5ms），不可察觉。
//
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "audio/Mixer.h"
#include "core/Song.h"

namespace piano {

class Player {
public:
    Player() noexcept = default;

    /// UI 线程。载入曲目（内部保存副本，避免调用方容器扩容导致悬垂）。
    void setSong(const Song& song, double sampleRate);

    /// UI 线程。从 startTick 开始播放，锚定到 anchorFrame（通常取 mixer.currentFrame()）。
    void play(std::uint64_t anchorFrame, double startTick = 0.0);

    /// UI 线程。请求停止调度（不触碰共享数据，立即返回）。
    void stop() noexcept;

    bool playing() const noexcept { return playing_.load(std::memory_order_acquire); }
    bool finished() const noexcept { return finished_.load(std::memory_order_relaxed); }

    void   setSpeed(double speed);
    double speed() const noexcept { return speed_; }

    void setTranspose(int semitones) noexcept { transpose_.store(semitones, std::memory_order_relaxed); }
    int  transpose() const noexcept { return transpose_.load(std::memory_order_relaxed); }

    void setTrackMuted(std::uint8_t track, bool muted) noexcept {
        if (track < kMaxTracks) muted_[track].store(muted, std::memory_order_relaxed);
    }
    bool trackMuted(std::uint8_t track) const noexcept {
        return track < kMaxTracks ? muted_[track].load(std::memory_order_relaxed) : false;
    }

    /// 已播放到的 tick（UI 线程读取）。
    double positionTick() const noexcept;
    /// 0..1 播放进度。
    double progress() const noexcept;

    double framesPerTickValue() const noexcept { return framesPerTick_; }
    std::uint64_t anchorFrame() const noexcept { return anchorFrame_; }
    const Song& song() const noexcept { return song_; }
    bool hasSong() const noexcept { return !song_.events.empty(); }

    /// 作为 AudioDevice::SchedulerFn 使用。返回「距离下一个事件还有多少帧」。
    static std::uint32_t process(void* user, Mixer& mixer,
                                 std::uint64_t blockStartFrame, std::uint32_t maxFrames);

private:
    static constexpr int kMaxTracks = 4;

    struct Action {
        std::uint64_t frame = 0;
        std::uint8_t  note = 0;
        std::uint8_t  velocity = 0;
        std::uint8_t  track = 0;
        bool          isOn = true;
        std::size_t   eventId = 0;
    };

    /// 等待音频线程离开 process() 临界区。
    void waitForAudioIdle();
    void rebuild(double anchorTick, std::uint64_t anchorFrame, bool preserveHeld = false);

    Song song_;
    std::vector<Action> actions_;
    // 每个原始事件实际发出的音高；-1 表示没有发声（包括静音轨）。
    std::vector<int> soundingNotes_;
    std::size_t cursor_ = 0;

    double sampleRate_ = 48000.0;
    double speed_ = 1.0;
    double framesPerTick_ = 100.0;
    double startTick_ = 0.0;
    std::uint64_t anchorFrame_ = 0;

    std::atomic<bool>          playing_{false};
    std::atomic<bool>          finished_{true};
    std::atomic<bool>          busy_{false};
    std::atomic<double>        positionFrame_{0.0};
    std::atomic<std::uint64_t> lastBlockStart_{0};
    std::atomic<int>           transpose_{0};
    std::atomic<bool>          muted_[kMaxTracks]{};
};

} // namespace piano
