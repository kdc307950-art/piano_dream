#pragma once
//
// audio/AudioDevice.h —— miniaudio 输出设备封装 + 实时调度入口
//
// 职责：
//   1. 打开/关闭系统音频输出（WASAPI / DirectSound / ALSA ...）
//   2. 在音频回调里：取出无锁队列中的输入事件 → 调用调度器 → 渲染
//   3. 提供主线程可安全调用的 pushNoteOn / pushNoteOff / pushSustain
//
// 采样点精确调度：
//   调度器签名为「返回距离下一个事件的帧数」。音频回调据此把整块切分成若干段，
//   每段渲染完后再调用一次调度器，从而让曲目事件的起始时刻精确到采样点，
//   而不是只能落在块边界上（详见 README「定时策略」）。
//
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "audio/Mixer.h"
#include "core/Event.h"
#include "core/SpscQueue.h"

// 前置声明：避免在头文件里引入 miniaudio（约 1.1MB），只在这里说明类型存在。
struct ma_device;

namespace piano {

/// 音频回调。签名必须与 miniaudio 的 ma_device_data_proc 完全一致（4 个参数，
/// 没有 user 参数 —— 用户指针从 device->pUserData 取）。
void piano_audio_callback(ma_device* device, void* output, const void* input,
                          std::uint32_t frameCount);

class AudioDevice {
public:
    /// 返回值语义：从 blockStartFrame 起，还可以安全渲染多少帧才会遇到下一个事件。
    /// 必须返回 ≥ 1；若本块内已无事件则返回 maxFrames。
    /// 调用时应先处理所有 frame <= blockStartFrame 的事件。
    using SchedulerFn = std::uint32_t (*)(void* user, Mixer& mixer,
                                          std::uint64_t blockStartFrame,
                                          std::uint32_t maxFrames);

    AudioDevice() = default;
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    /// 打开设备（会按设备实际采样率 prepare 混音器）。不阻塞、不启动播放。
    bool open(Mixer& mixer, SchedulerFn scheduler, void* schedulerUser,
              std::string* error = nullptr);

    /// 开始播放回调。
    bool start(std::string* error = nullptr);

    /// 停止并关闭设备。
    void stop();

    bool running() const noexcept { return running_; }
    double sampleRate() const noexcept { return sampleRate_; }
    std::uint32_t periodFrames() const noexcept { return periodFrames_; }
    std::size_t queueDepth() const noexcept { return queue_.size(); }
    std::uint64_t droppedMessages() const noexcept { return dropped_.load(std::memory_order_relaxed); }

    // ---------------- 主线程 API ----------------

    bool pushNoteOn(int midi, float velocity, Source source);
    bool pushNoteOff(int midi, Source source);
    bool pushSustain(bool on);
    bool pushAllNotesOff();
    bool pushReleaseSource(Source source);
    bool pushPanic();

    /// 静音设备（继续推进时间轴，但不写输出）。
    void setMuted(bool muted) noexcept { muted_.store(muted, std::memory_order_relaxed); }
    bool muted() const noexcept { return muted_.load(std::memory_order_relaxed); }

private:
    void applyMessage(const NoteMsg& msg, Mixer& mixer);

    SpscQueue<NoteMsg, 1024> queue_;
    Mixer* mixer_ = nullptr;
    SchedulerFn scheduler_ = nullptr;
    void* schedulerUser_ = nullptr;

    ma_device* device_ = nullptr;
    double sampleRate_ = 48000.0;
    std::uint32_t periodFrames_ = 256;
    bool running_ = false;

    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<bool> muted_{false};

    friend void piano_audio_callback(ma_device*, void*, const void*, std::uint32_t);
};

} // namespace piano
