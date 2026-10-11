#pragma once
//
// midi/MidiWriter.h —— 标准 MIDI 文件（SMF）写出
//
// 统一导出为 Format 0（单轨），PPQ 与源数据一致（默认 480），
// 文件开头写入 Tempo 元事件，结尾补齐所有悬空音符的 NoteOff 与踏板抬起，
// 保证在任何播放器里都不会出现「卡住不断音」。
//
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/Event.h"
#include "core/Song.h"

namespace piano {

struct MidiWriteOptions {
    int  bpm      = kDefaultBpm;
    int  ppq      = kDefaultPpq;
    bool withTempo = true;
};

/// 把曲目导出为 SMF 字节流。
std::vector<std::uint8_t> writeSongMidi(const Song& song, const MidiWriteOptions& opts = {});

/// 把录制事件导出为 SMF 字节流（保留踏板 CC64）。
std::vector<std::uint8_t> writeRecordingMidi(const std::vector<RecordedEvent>& events,
                                             double sampleRate,
                                             std::uint64_t durationFrames,
                                             const MidiWriteOptions& opts = {});

bool saveMidiFile(const std::string& path, const std::vector<std::uint8_t>& bytes, std::string* error = nullptr);
bool saveMidiFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes, std::string* error = nullptr);

/// 把录制事件转成 tick 制的 Song（用于回放 / 跟练比对）。
Song recordingToSong(const std::vector<RecordedEvent>& events,
                     double sampleRate,
                     std::uint64_t durationFrames,
                     const MidiWriteOptions& opts = {});

} // namespace piano
