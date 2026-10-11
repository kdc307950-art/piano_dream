#pragma once
//
// midi/MidiReader.h —— 标准 MIDI 文件（SMF）解析
//
// 覆盖 Format 0 / Format 1：
//   - 变长数量（VLQ）：每字节低 7 位有效，最高位为 1 表示后续还有字节
//   - Running status：数据字节最高位为 0 时沿用上一个状态字节
//   - Meta 事件（FF）：读取 Tempo（FF 51 03 tttttt），其余跳过
//   - SysEx（F0 / F7）：跳过
//   - NoteOn velocity == 0 等价于 NoteOff
//   - 同音高的 NoteOn / NoteOff 按「先进先出」配对（同一轨道内可重叠）
//
#include <cstdint>
#include <string>
#include <vector>

#include "core/Song.h"

namespace piano {

struct MidiParseResult {
    bool ok = false;
    Song song;
    std::string error;
    int format = 0;
    int trackCount = 0;
    int ppq = 480;
    int tempoChanges = 0;
    int skippedEvents = 0;
};

/// 从内存缓冲解析 MIDI 文件。
MidiParseResult parseMidi(const std::uint8_t* data, std::size_t size);

/// 从磁盘读取并解析。
MidiParseResult loadMidiFile(const std::string& path);

} // namespace piano
