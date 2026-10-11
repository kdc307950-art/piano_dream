#include "sequencer/Metronome.h"

#include <cmath>

namespace piano {

std::uint32_t Metronome::process(void* user, Mixer& mixer,
                                 std::uint64_t blockStartFrame, std::uint32_t maxFrames) {
    auto* self = static_cast<Metronome*>(user);
    if (!self) return maxFrames;

    if (!self->enabled_.load(std::memory_order_relaxed)) {
        self->wasEnabled_ = false;     // 下次启用时重新对齐，避免补一串历史咔哒
        return maxFrames;
    }

    const int    bpm = self->bpm_.load(std::memory_order_relaxed);
    const double fpb = 60.0 / static_cast<double>(bpm > 0 ? bpm : 100) * self->sampleRate_;

    if (!self->wasEnabled_) {
        self->wasEnabled_      = true;
        self->beat_            = 0;
        self->framesPerBeat_   = fpb;
        self->nextClickFrame_  = static_cast<double>(blockStartFrame);
    } else if (std::fabs(fpb - self->framesPerBeat_) > 1e-9) {
        self->framesPerBeat_ = fpb;    // 变速从下一拍开始生效
    }

    // 落后超过一拍（例如刚被系统挂起）时重新对齐，不补偿历史拍点
    if (static_cast<double>(blockStartFrame) - self->nextClickFrame_ > self->framesPerBeat_)
        self->nextClickFrame_ = static_cast<double>(blockStartFrame);

    const int beatsPerBar = self->beatsPerBar_.load(std::memory_order_relaxed);
    while (self->nextClickFrame_ <= static_cast<double>(blockStartFrame)) {
        const bool strong = (self->beat_ % (beatsPerBar > 0 ? beatsPerBar : 4)) == 0;
        mixer.triggerClick(strong ? 1320.0f : 920.0f, 0.21f, 0.07f);
        self->lastBeat_.store(self->beat_, std::memory_order_relaxed);
        self->lastClickFrame_.store(static_cast<std::uint64_t>(self->nextClickFrame_),
                                    std::memory_order_relaxed);
        ++self->beat_;
        self->nextClickFrame_ += self->framesPerBeat_;
    }

    const double d = self->nextClickFrame_ - static_cast<double>(blockStartFrame);
    if (d <= 0.0) return 1;
    return d >= static_cast<double>(maxFrames) ? maxFrames : static_cast<std::uint32_t>(d);
}

} // namespace piano
