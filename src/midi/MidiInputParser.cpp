#include "midi/MidiInputParser.h"

namespace piano {

void MidiInputParser::reset() noexcept {
    status_ = 0;
    data_[0] = data_[1] = 0;
    dataCount_ = dataNeeded_ = 0;
    inSysEx_ = false;
    ignored_ = 0;
}

void MidiInputParser::finishMessage(std::vector<MidiInputEvent>& out) noexcept {
    const std::uint8_t kind = status_ & 0xF0;
    const std::uint8_t channel = status_ & 0x0F;
    const std::uint8_t a = data_[0];
    const std::uint8_t b = data_[1];

    if (a > 127 || b > 127) {
        ++ignored_;
    } else if (kind == 0x90) {
        out.push_back({ b == 0 ? MidiInputType::NoteOff : MidiInputType::NoteOn,
                        channel, a, b });
    } else if (kind == 0x80) {
        out.push_back({ MidiInputType::NoteOff, channel, a, b });
    } else if (kind == 0xB0 && a == 64) {
        out.push_back({ MidiInputType::Sustain, channel, 0,
                        static_cast<std::uint8_t>(b >= 64 ? 127 : 0) });
    } else if (kind == 0xB0 && a == 123) {
        out.push_back({ MidiInputType::AllNotesOff, channel, 0, 0 });
    }
    dataCount_ = 0;
}

void MidiInputParser::feed(std::uint8_t byte, std::vector<MidiInputEvent>& out) noexcept {
    // MIDI realtime bytes may appear between any data bytes and do not affect running status.
    if (byte >= 0xF8) { ++ignored_; return; }

    if (inSysEx_) {
        if (byte == 0xF7) inSysEx_ = false;
        else ++ignored_;
        return;
    }

    if (byte == 0xF0) {
        inSysEx_ = true;
        dataCount_ = dataNeeded_ = 0;
        status_ = 0;
        ++ignored_;
        return;
    }

    if (byte & 0x80) {
        if (byte >= 0xF1 && byte <= 0xF6) {
            status_ = 0;
            dataCount_ = 0;
            dataNeeded_ = (byte == 0xF2) ? 2 : ((byte == 0xF1 || byte == 0xF3) ? 1 : 0);
            ++ignored_;
            return;
        }
        const std::uint8_t kind = byte & 0xF0;
        if (kind == 0x80 || kind == 0x90 || kind == 0xA0 || kind == 0xB0 || kind == 0xE0) {
            status_ = byte;
            dataCount_ = 0;
            dataNeeded_ = 2;
        } else if (kind == 0xC0 || kind == 0xD0) {
            status_ = byte;
            dataCount_ = 0;
            dataNeeded_ = 1;
        } else {
            ++ignored_;
        }
        return;
    }

    if (status_ == 0 || dataNeeded_ == 0) { ++ignored_; return; }
    if (byte > 127) { ++ignored_; dataCount_ = 0; return; }
    data_[dataCount_++] = byte;
    if (dataCount_ >= dataNeeded_) finishMessage(out);
}

} // namespace piano
