#include "sequencer/Player.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include "core/Note.h"

namespace piano {
namespace {

/// 进入临界区时置忙，离开时清除（保证所有返回路径都会清除）。
struct BusyGuard {
    std::atomic<bool>* flag;
    explicit BusyGuard(std::atomic<bool>* f) : flag(f) { flag->store(true, std::memory_order_release); }
    ~BusyGuard() { flag->store(false, std::memory_order_release); }
};

} // namespace

void Player::waitForAudioIdle() {
    playing_.store(false, std::memory_order_release);
    while (busy_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

void Player::setSong(const Song& song, double sampleRate) {
    waitForAudioIdle();
    song_ = song;                                  // 副本，生命周期独立
    song_.sortEvents();
    soundingNotes_.assign(song_.events.size(), -1);
    sampleRate_ = sampleRate > 1000.0 ? sampleRate : 48000.0;
    framesPerTick_ = framesPerTick(song_.bpm, song_.ppq, sampleRate_, speed_);
    actions_.clear();
    cursor_ = 0;
    finished_.store(true, std::memory_order_relaxed);
    positionFrame_.store(0.0, std::memory_order_relaxed);
    startTick_ = 0.0;
    anchorFrame_ = 0;
    for (int i = 0; i < kMaxTracks; ++i) muted_[i].store(false, std::memory_order_relaxed);
}

void Player::rebuild(double anchorTick, std::uint64_t anchorFrame, bool preserveHeld) {
    actions_.clear();
    cursor_ = 0;
    startTick_ = anchorTick;
    anchorFrame_ = anchorFrame;

    if (song_.events.empty()) return;

    const double fpt = framesPerTick_;
    auto frameOf = [&](double tick) -> std::uint64_t {
        const double f = static_cast<double>(anchorFrame) + (tick - anchorTick) * fpt;
        return f <= 0.0 ? 0ull : static_cast<std::uint64_t>(f + 0.5);
    };

    actions_.reserve(song_.events.size() * 2);
    for (std::size_t id = 0; id < song_.events.size(); ++id) {
        const auto& e = song_.events[id];
        const double onTick  = static_cast<double>(e.tick);
        const double offTick = static_cast<double>(e.endTick());
        if (offTick <= anchorTick && soundingNotes_[id] < 0) continue;

        Action on;
        on.note     = e.note;
        on.velocity = e.velocity;
        on.track    = e.track;
        on.isOn     = true;
        on.eventId  = id;
        // 变速保留已有声部，跳转则补发跨过锚点的音。
        on.frame    = (onTick >= anchorTick) ? frameOf(onTick) : anchorFrame;
        if (!preserveHeld || (onTick >= anchorTick && soundingNotes_[id] < 0))
            actions_.push_back(on);

        Action off;
        off.note     = e.note;
        off.velocity = 0;
        off.track    = e.track;
        off.isOn     = false;
        off.eventId  = id;
        off.frame    = offTick <= anchorTick ? anchorFrame : frameOf(offTick);
        actions_.push_back(off);
    }

    std::stable_sort(actions_.begin(), actions_.end(),
                     [](const Action& a, const Action& b) {
                         if (a.frame != b.frame) return a.frame < b.frame;
                         // 同一帧先关后开，避免同音高被吞掉
                         return static_cast<int>(a.isOn) < static_cast<int>(b.isOn);
                     });
}

void Player::play(std::uint64_t anchorFrame, double startTick) {
    if (song_.events.empty()) return;
    waitForAudioIdle();                            // 确保可以独占事件表
    std::fill(soundingNotes_.begin(), soundingNotes_.end(), -1);
    framesPerTick_ = framesPerTick(song_.bpm, song_.ppq, sampleRate_, speed_);
    rebuild(startTick, anchorFrame);
    positionFrame_.store(static_cast<double>(anchorFrame), std::memory_order_relaxed);
    lastBlockStart_.store(anchorFrame, std::memory_order_relaxed);
    finished_.store(false, std::memory_order_relaxed);
    playing_.store(true, std::memory_order_release);
}

void Player::stop() noexcept {
    waitForAudioIdle();
    finished_.store(true, std::memory_order_relaxed);
}

void Player::setSpeed(double speed) {
    const double s = static_cast<double>(clampf(static_cast<float>(speed), 0.25f, 4.0f));
    if (std::fabs(s - speed_) < 1e-6) return;

    const bool wasPlaying = playing_.load(std::memory_order_acquire);
    waitForAudioIdle();
    const double tick = wasPlaying ? positionTick() : 0.0;
    speed_ = s;
    framesPerTick_ = framesPerTick(song_.bpm, song_.ppq, sampleRate_, speed_);

    if (wasPlaying) {
        // 以「当前进度 + 最近一次块起点」为锚点重建，保证变速后从原位置继续
        const std::uint64_t anchor = lastBlockStart_.load(std::memory_order_relaxed);
        rebuild(tick, anchor, true);
        positionFrame_.store(static_cast<double>(anchor), std::memory_order_relaxed);
        finished_.store(false, std::memory_order_relaxed);
        playing_.store(true, std::memory_order_release);
    }
}

double Player::positionTick() const noexcept {
    if (song_.events.empty()) return 0.0;
    if (framesPerTick_ <= 0.0) return startTick_;
    const double frame = positionFrame_.load(std::memory_order_relaxed);
    return startTick_ + (frame - static_cast<double>(anchorFrame_)) / framesPerTick_;
}

double Player::progress() const noexcept {
    const std::uint32_t total = song_.durationTicks();
    if (total == 0) return 0.0;
    const double p = positionTick() / static_cast<double>(total);
    return p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
}

std::uint32_t Player::process(void* user, Mixer& mixer,
                              std::uint64_t blockStartFrame, std::uint32_t maxFrames) {
    auto* self = static_cast<Player*>(user);
    if (!self) return maxFrames;

    // 未播放时立即返回，不触碰任何共享数据 —— 这是 UI 线程安全重建的前提
    if (!self->playing_.load(std::memory_order_acquire)) return maxFrames;

    BusyGuard guard(&self->busy_);
    // UI 可能在第一次 playing 检查后暂停了调度，必须再次检查。
    if (!self->playing_.load(std::memory_order_acquire)) return maxFrames;
    self->lastBlockStart_.store(blockStartFrame, std::memory_order_relaxed);
    self->positionFrame_.store(static_cast<double>(blockStartFrame), std::memory_order_relaxed);

    // 1) 处理所有「发生在本帧或之前」的事件
    const int transpose = self->transpose_.load(std::memory_order_relaxed);
    while (self->cursor_ < self->actions_.size() &&
           self->actions_[self->cursor_].frame <= blockStartFrame) {
        const Action& a = self->actions_[self->cursor_++];
        if (a.isOn) {
            if (!self->trackMuted(a.track)) {
                const int note = clampNote(static_cast<int>(a.note) + transpose);
                self->soundingNotes_[a.eventId] = note;
                mixer.noteOn(note, static_cast<float>(a.velocity) / 127.0f, Source::Song);
            }
        } else {
            const int note = self->soundingNotes_[a.eventId];
            if (note >= 0) mixer.noteOff(note, Source::Song);
            self->soundingNotes_[a.eventId] = -1;
        }
    }

    // 2) 计算距离下一个事件的帧数
    if (self->cursor_ >= self->actions_.size()) {
        self->positionFrame_.store(static_cast<double>(blockStartFrame) + maxFrames,
                                   std::memory_order_relaxed);
        self->playing_.store(false, std::memory_order_release);
        self->finished_.store(true, std::memory_order_relaxed);
        return maxFrames;
    }

    const std::uint64_t next = self->actions_[self->cursor_].frame;
    if (next <= blockStartFrame) return 1;
    const std::uint64_t d = next - blockStartFrame;
    return d > maxFrames ? maxFrames : static_cast<std::uint32_t>(d);
}

} // namespace piano
