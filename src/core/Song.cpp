#include "core/Song.h"

#include <algorithm>

namespace piano {

void Song::sortEvents() {
    std::stable_sort(events.begin(), events.end(),
                     [](const NoteEvent& a, const NoteEvent& b) {
                         if (a.tick != b.tick) return a.tick < b.tick;
                         if (a.track != b.track) return a.track < b.track;
                         return a.note < b.note;
                     });
}

std::uint32_t Song::durationTicks() const {
    std::uint32_t end = 0;
    for (const auto& e : events)
        end = std::max(end, e.endTick());
    return end;
}

double Song::durationSeconds() const {
    const double beats = static_cast<double>(durationTicks()) / static_cast<double>(ppq);
    return beats * 60.0 / static_cast<double>(bpm > 0 ? bpm : kDefaultBpm);
}

int Song::trackCount() const {
    int n = 0;
    for (const auto& e : events)
        if (e.track + 1 > n) n = e.track + 1;
    return n;
}

bool Song::hasTrack(std::uint8_t track) const {
    for (const auto& e : events)
        if (e.track == track) return true;
    return false;
}

double framesPerTick(int bpm, int ppq, double sampleRate, double speed) {
    if (bpm <= 0) bpm = kDefaultBpm;
    if (ppq <= 0) ppq = kDefaultPpq;
    if (speed <= 1e-6) speed = 1.0;
    return sampleRate * 60.0 / (static_cast<double>(bpm) * static_cast<double>(ppq) * speed);
}

} // namespace piano
