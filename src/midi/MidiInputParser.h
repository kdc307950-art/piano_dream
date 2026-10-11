#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace piano {

enum class MidiInputType : std::uint8_t {
    NoteOn,
    NoteOff,
    Sustain,
    AllNotesOff,
};

struct MidiInputEvent {
    MidiInputType type = MidiInputType::NoteOn;
    std::uint8_t channel = 0;
    std::uint8_t note = 0;
    std::uint8_t velocity = 0;
};

/// 纯字节流 MIDI 输入解析器，不依赖 RtMidi 或音频设备。
/// 支持运行状态、NoteOn/NoteOff、CC64、CC123；实时消息和 SysEx 会被忽略。
class MidiInputParser {
public:
    void reset() noexcept;
    void feed(std::uint8_t byte, std::vector<MidiInputEvent>& out) noexcept;

    std::size_t ignoredCount() const noexcept { return ignored_; }

private:
    void finishMessage(std::vector<MidiInputEvent>& out) noexcept;

    std::uint8_t status_ = 0;
    std::uint8_t data_[2] = {};
    std::uint8_t dataCount_ = 0;
    std::uint8_t dataNeeded_ = 0;
    bool inSysEx_ = false;
    std::size_t ignored_ = 0;
};

} // namespace piano
