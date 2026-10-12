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
    /// 每个 MIDI 轨道的音符统计（下标即轨道号），供曲库挑选旋律轨
    struct TrackStat {
        int noteCount = 0;
        int lowest = -1;
        int highest = -1;
        std::uint32_t channelMask = 0;
    };
    bool ok = false;
    Song song;
    std::string error;
    int format = 0;
    int trackCount = 0;
    int ppq = 480;
    int tempoChanges = 0;
    int skippedEvents = 0;
    std::vector<TrackStat> trackStats;
    /// 与 song.events 一一对应：每个事件来自哪个原始 MIDI 轨道（排序后仍对齐）
    std::vector<std::uint16_t> eventTracks;
};

/// 从内存缓冲解析 MIDI 文件。
MidiParseResult parseMidi(const std::uint8_t* data, std::size_t size);

/// 从磁盘读取并解析。
MidiParseResult loadMidiFile(const std::string& path);

/// 重新指定旋律轨：把来自 trackIdx 的事件标为旋律，其余标为伴奏。
/// 不改变事件顺序，仅重写 NoteEvent::track。
void reassignMelodyTrack(MidiParseResult& result, std::size_t trackIdx);

} // namespace piano
