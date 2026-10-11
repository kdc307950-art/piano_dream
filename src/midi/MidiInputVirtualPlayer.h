#pragma once

#include <algorithm>
#include <cstdint>

#include "core/Song.h"
#include "midi/MidiInputQueue.h"

namespace piano {

/// 把解析后的曲目转换为 MIDI 输入队列事件，用于 CI 和无硬件回归测试。
/// 真实 RtMidi 回调与此虚拟源共享 MidiInputQueue 下游路径。
inline std::size_t enqueueVirtualMidiSong(const Song& song, MidiInputQueue& queue,
                                          double startTimestampSeconds = 0.0) {
    if (song.ppq <= 0 || song.bpm <= 0) return 0;
    const double secondsPerTick = 60.0 / (static_cast<double>(song.bpm) * song.ppq);
    std::size_t pushed = 0;
    for (const auto& note : song.events) {
        const double on = startTimestampSeconds + note.tick * secondsPerTick;
        const double off = startTimestampSeconds + note.endTick() * secondsPerTick;
        if (queue.push({ { MidiInputType::NoteOn, note.channel, note.note, note.velocity }, on })) ++pushed;
        if (queue.push({ { MidiInputType::NoteOff, note.channel, note.note, 0 }, off })) ++pushed;
    }
    return pushed;
}

} // namespace piano
