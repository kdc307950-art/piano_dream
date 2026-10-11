#pragma once
//
// sequencer/Recorder.h —— 演奏录制
//
// 时间戳以「采样帧」记录（取自 Mixer 的单调帧计数），而不是 wall-clock 毫秒。
// 好处：录制结果与音频时钟同源，回放与导出 MIDI 都不会累积漂移。
//
// 本类只在 UI 线程使用，无需加锁。
//
#include <cstdint>
#include <vector>

#include "core/Event.h"

namespace piano {

class Recorder {
public:
    void start(std::uint64_t nowFrame) noexcept;
    void stop(std::uint64_t nowFrame) noexcept;

    bool recording() const noexcept { return recording_; }

    /// 记录一个事件。frame 取 mixer.currentFrame()。
    void capture(std::uint64_t frame, MsgType type, int note, int velocity);

    const std::vector<RecordedEvent>& events() const noexcept { return events_; }
    std::uint64_t durationFrames() const noexcept { return durationFrames_; }
    std::uint64_t startFrame() const noexcept { return startFrame_; }

    bool hasNotes() const noexcept;
    int  noteOnCount() const noexcept;
    int  eventCount() const noexcept { return static_cast<int>(events_.size()); }
    void clear() noexcept;

    /// 已录制时长（秒）
    double durationSeconds(double sampleRate) const noexcept;

private:
    std::vector<RecordedEvent> events_;
    std::uint64_t startFrame_ = 0;
    std::uint64_t durationFrames_ = 0;
    bool recording_ = false;
};

} // namespace piano
