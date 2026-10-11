#include "sequencer/Recorder.h"

#include <algorithm>

namespace piano {

void Recorder::start(std::uint64_t nowFrame) noexcept {
    events_.clear();
    startFrame_ = nowFrame;
    durationFrames_ = 0;
    recording_ = true;
}

void Recorder::stop(std::uint64_t nowFrame) noexcept {
    if (!recording_) return;
    recording_ = false;
    if (nowFrame > startFrame_)
        durationFrames_ = std::max(durationFrames_, nowFrame - startFrame_);
}

void Recorder::capture(std::uint64_t frame, MsgType type, int note, int velocity) {
    if (!recording_) return;
    RecordedEvent e;
    e.frame    = frame > startFrame_ ? frame - startFrame_ : 0;
    e.type     = type;
    e.note     = static_cast<std::uint8_t>(note < 0 ? 0 : (note > 127 ? 127 : note));
    e.velocity = static_cast<std::uint8_t>(velocity < 0 ? 0 : (velocity > 127 ? 127 : velocity));
    events_.push_back(e);
    durationFrames_ = std::max(durationFrames_, e.frame);
}

bool Recorder::hasNotes() const noexcept {
    for (const auto& e : events_)
        if (e.type == MsgType::NoteOn) return true;
    return false;
}

int Recorder::noteOnCount() const noexcept {
    int n = 0;
    for (const auto& e : events_)
        if (e.type == MsgType::NoteOn) ++n;
    return n;
}

void Recorder::clear() noexcept {
    events_.clear();
    durationFrames_ = 0;
    recording_ = false;
}

double Recorder::durationSeconds(double sampleRate) const noexcept {
    const double sr = sampleRate > 1.0 ? sampleRate : 48000.0;
    return static_cast<double>(durationFrames_) / sr;
}

} // namespace piano
