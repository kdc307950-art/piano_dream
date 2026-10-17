#pragma once
//
// sequencer/RecordedPlayer.h —— 录制回放器（采样点精确）
//
// 与 Player 的区别：回放的是「带踏板 CC64 的原始事件流」，
// 因此不复用 tick 制的事件表，而是直接按采样帧调度。
//
// 线程模型与 Player 相同：事件表由音频线程独占访问，
// UI 线程修改前先 waitForAudioIdle()。
//
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "audio/Mixer.h"
#include "core/Event.h"

namespace piano {

class RecordedPlayer {
public:
    void setEvents(std::vector<RecordedEvent> events, double sampleRate) {
        waitForAudioIdle();
        events_ = std::move(events);
        std::stable_sort(events_.begin(), events_.end(),
                         [](const RecordedEvent& a, const RecordedEvent& b) {
                             if (a.frame != b.frame) return a.frame < b.frame;
                             return static_cast<int>(a.type) < static_cast<int>(b.type);
                         });
        sampleRate_ = sampleRate > 1000.0 ? sampleRate : 48000.0;
        cursor_ = 0;
        durationFrames_ = 0;
        for (const auto& e : events_) durationFrames_ = std::max(durationFrames_, e.frame);
        playing_.store(false, std::memory_order_relaxed);
    }

    void play(std::uint64_t anchorFrame) {
        if (events_.empty()) return;
        waitForAudioIdle();
        anchorFrame_ = anchorFrame;
        cursor_ = 0;
        position_.store(anchorFrame, std::memory_order_relaxed);
        playing_.store(true, std::memory_order_release);
    }

    void stop() noexcept { playing_.store(false, std::memory_order_release); }

    bool playing() const noexcept { return playing_.load(std::memory_order_acquire); }
    bool empty() const noexcept { return events_.empty(); }
    std::uint64_t durationFrames() const noexcept { return durationFrames_; }
    double sampleRate() const noexcept { return sampleRate_; }

    /// 0..1 进度。
    double progress() const noexcept {
        if (durationFrames_ == 0) return 0.0;
        const std::uint64_t pos = position_.load(std::memory_order_relaxed);
        const double rel = static_cast<double>(pos > anchorFrame_ ? pos - anchorFrame_ : 0);
        const double p = rel / static_cast<double>(durationFrames_);
        return p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
    }

    static std::uint32_t process(void* user, Mixer& mixer,
                                 std::uint64_t blockStartFrame, std::uint32_t maxFrames) {
        auto* self = static_cast<RecordedPlayer*>(user);
        if (!self) return maxFrames;
        // 先登记 busy_ 再检查 playing_，避免 UI 在线程切换窗口中修改 events_。
        self->busy_.store(true, std::memory_order_release);
        if (!self->playing_.load(std::memory_order_acquire)) {
            self->busy_.store(false, std::memory_order_release);
            return maxFrames;
        }
        while (self->cursor_ < self->events_.size()) {
            const RecordedEvent& e = self->events_[self->cursor_];
            const std::uint64_t when = self->anchorFrame_ + e.frame;
            if (when > blockStartFrame) break;
            ++self->cursor_;

            switch (e.type) {
            case MsgType::NoteOn:
                mixer.noteOn(e.note, static_cast<float>(e.velocity) / 127.0f, Source::Playback);
                break;
            case MsgType::NoteOff:
                mixer.noteOff(e.note, Source::Playback);
                break;
            case MsgType::Sustain:
                // 回放期间由录制事件掌管踏板，UI 侧会暂停自己的踏板推送
                mixer.setSustain(e.velocity > 0);
                break;
            default:
                break;
            }
            self->position_.store(when, std::memory_order_relaxed);
        }

        std::uint32_t result = maxFrames;
        if (self->cursor_ >= self->events_.size()) {
            self->playing_.store(false, std::memory_order_release);
            self->busy_.store(false, std::memory_order_release);
            return maxFrames;
        }
        const std::uint64_t next = self->anchorFrame_ + self->events_[self->cursor_].frame;
        if (next > blockStartFrame) {
            const std::uint64_t d = next - blockStartFrame;
            result = d > maxFrames ? maxFrames : static_cast<std::uint32_t>(d);
        } else {
            result = 1;
        }
        self->busy_.store(false, std::memory_order_release);
        return result;
    }

private:
    void waitForAudioIdle() {
        playing_.store(false, std::memory_order_release);
        while (busy_.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    std::vector<RecordedEvent> events_;
    std::size_t cursor_ = 0;
    std::uint64_t anchorFrame_ = 0;
    std::uint64_t durationFrames_ = 0;
    double sampleRate_ = 48000.0;

    std::atomic<bool> playing_{false};
    std::atomic<bool> busy_{false};
    std::atomic<std::uint64_t> position_{0};
};

} // namespace piano
