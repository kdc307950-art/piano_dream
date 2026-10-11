#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <algorithm>
#include <mutex>
#include <vector>

#include "midi/MidiInputParser.h"

namespace piano {

struct TimedMidiInputEvent {
    MidiInputEvent event{};
    double timestampSeconds = 0.0;
};

/// MIDI 回调线程与 UI/音频调度线程之间的独立有界队列。
/// 不复用音频侧 SpscQueue，回调线程只做一次加锁和拷贝，不触碰音频状态。
class MidiInputQueue {
public:
    static constexpr std::size_t kCapacity = 2048;

    bool push(const TimedMidiInputEvent& event) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (events_.size() >= kCapacity) {
            const bool critical = event.event.type == MidiInputType::NoteOff ||
                                  event.event.type == MidiInputType::AllNotesOff ||
                                  event.event.type == MidiInputType::Sustain;
            if (!critical) { ++dropped_; return false; }
            // 释放事件优先：先丢一个 NoteOn；若队列全是控制事件，清空旧事件
            // 后仍要把当前 NoteOff 送入下游，避免挂音。
            auto it = std::find_if(events_.begin(), events_.end(), [](const auto& e) {
                return e.event.type == MidiInputType::NoteOn;
            });
            if (it != events_.end()) events_.erase(it);
            else events_.clear();
            ++dropped_;
        }
        events_.push_back(event);
        return true;
    }

    std::size_t drain(std::vector<TimedMidiInputEvent>& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::size_t n = events_.size();
        out.insert(out.end(), events_.begin(), events_.end());
        events_.clear();
        return n;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return events_.size();
    }

    std::size_t dropped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

private:
    mutable std::mutex mutex_;
    std::deque<TimedMidiInputEvent> events_;
    std::size_t dropped_ = 0;
};

/// 把 MIDI 回调给出的单调秒时间戳映射到音频帧域。
class MidiInputClock {
public:
    explicit MidiInputClock(double sampleRate = 48000.0) : sampleRate_(sampleRate) {}

    void reset(std::uint64_t audioFrame, double midiTimestampSeconds) noexcept {
        audioFrame_ = audioFrame;
        midiTimestamp_ = midiTimestampSeconds;
        initialized_ = true;
    }

    std::uint64_t toFrame(double midiTimestampSeconds) const noexcept {
        if (!initialized_) return audioFrame_;
        const double delta = (midiTimestampSeconds - midiTimestamp_) * sampleRate_;
        return delta <= 0.0 ? audioFrame_ : audioFrame_ + static_cast<std::uint64_t>(delta + 0.5);
    }

private:
    double sampleRate_ = 48000.0;
    std::uint64_t audioFrame_ = 0;
    double midiTimestamp_ = 0.0;
    bool initialized_ = false;
};

} // namespace piano
