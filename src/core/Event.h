#pragma once
//
// core/Event.h —— 事件类型定义（跨线程消息 / 录制事件）
//
#include <cstdint>

namespace piano {

enum class MsgType : std::uint8_t {
    NoteOn = 0,
    NoteOff,
    Sustain,       // velocity > 0 表示踩下踏板
    AllNotesOff,   // 释放全部声部（含用户弹奏）
    ReleaseSource, // 只释放某个来源的声部（用于停止曲目播放）
    Panic
};

/// 事件来源，用于区分「用户弹奏」与「自动播放」，避免互相干扰。
enum class Source : std::uint8_t {
    User = 0,
    Playback = 1,   // 录制回放
    Song = 2,       // 内置曲目 / 导入 MIDI 自动演奏
    Metronome = 3
};

/// 跨线程传递的音符消息。POD，可安全放入无锁队列。
struct NoteMsg {
    MsgType      type     = MsgType::NoteOn;
    std::uint8_t note     = 0;
    std::uint8_t velocity = 0;
    Source       source   = Source::User;
};

/// 录制事件：时间戳以「采样帧」为单位，避免 setInterval/timer 的抖动。
struct RecordedEvent {
    std::uint64_t frame    = 0;
    MsgType       type     = MsgType::NoteOn;
    std::uint8_t  note     = 0;
    std::uint8_t  velocity = 0;
};

} // namespace piano
