#pragma once
//
// core/Song.h —— 曲目 / 录制数据模型
//
// 统一表示：tick + 音符 + 力度 + 时长（PPQ = 480，与 HTML 原版一致）。
// 录制、曲目播放、MIDI 导出三者共用这一份结构，不需要中间转换。
//
#include <cstdint>
#include <string>
#include <vector>

namespace piano {

inline constexpr int kDefaultPpq = 480;
inline constexpr int kDefaultBpm = 120;

/// 音轨编号约定：0 = 旋律，1 = 伴奏。导入 MIDI 时保留原始轨道号。
inline constexpr std::uint8_t kTrackMelody = 0;
inline constexpr std::uint8_t kTrackAccomp = 1;

struct NoteEvent {
    std::uint32_t tick     = 0;
    std::uint32_t duration = kDefaultPpq;
    std::uint8_t  note     = 60;
    std::uint8_t  velocity = 104;
    std::uint8_t  track    = kTrackMelody;
    std::uint8_t  channel  = 0;

    std::uint32_t endTick() const { return tick + duration; }
};

struct Song {
    std::string id;
    std::string title;
    std::string composer;
    int  bpm        = kDefaultBpm;
    int  ppq        = kDefaultPpq;
    int  difficulty = 1;          // 1 = 入门，2 = 进阶
    bool userImport = false;      // 是否为用户导入的 MIDI
    std::vector<NoteEvent> events;

    void sortEvents();
    std::uint32_t durationTicks() const;
    double durationSeconds() const;
    bool empty() const { return events.empty(); }
    int  trackCount() const;
    bool hasTrack(std::uint8_t track) const;
};

/// 帧 ↔ tick 换算（采样点精确调度的基础）。
/// framesPerTick = sampleRate * 60 / (bpm * ppq * speed)
double framesPerTick(int bpm, int ppq, double sampleRate, double speed);

} // namespace piano
