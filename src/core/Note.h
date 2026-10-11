#pragma once
//
// core/Note.h —— MIDI 音高工具
//
// 纯数据层：不依赖 raylib / miniaudio，可在测试中独立使用。
//
#include <cmath>
#include <cstdint>
#include <string>

namespace piano {

inline constexpr int kMidiLow  = 21;   // A0
inline constexpr int kMidiHigh = 108;  // C8
inline constexpr int kKeyCount = kMidiHigh - kMidiLow + 1;  // 88

/// 黑键判定：一个八度内的 C#/Db、D#/Eb、F#/Gb、G#/Ab、A#/Bb。
constexpr bool isBlackKey(int midi) noexcept {
    const int pc = midi % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

inline constexpr const char* kPitchClassNames[12] = {
    "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"
};

inline int octaveOf(int midi) noexcept { return midi / 12 - 1; }

/// 例：60 -> "C4"，21 -> "A0"，108 -> "C8"。
inline std::string noteName(int midi) {
    const int pc = ((midi % 12) + 12) % 12;
    return std::string(kPitchClassNames[pc]) + std::to_string(octaveOf(midi));
}

/// 十二平均律，A4 = 440Hz（MIDI 69）。
inline double noteHz(int midi) noexcept {
    return 440.0 * std::pow(2.0, (midi - 69) / 12.0);
}

/// [kMidiLow, midi) 区间内的白键数量 —— 88 键横向排版的基础。
inline int whiteKeysBefore(int midi) noexcept {
    int n = 0;
    for (int m = kMidiLow; m < midi; ++m)
        if (!isBlackKey(m)) ++n;
    return n;
}

inline constexpr int kWhiteKeyCount = 52;  // 88 键中白键总数

inline int clampNote(int midi) noexcept {
    return midi < kMidiLow ? kMidiLow : (midi > kMidiHigh ? kMidiHigh : midi);
}

/// 解析音名（"C4"、"Eb4"、"D#4"、"F#3"）或纯数字 MIDI 编号。失败返回 -1。
inline int parseNoteName(const std::string& raw) {
    if (raw.empty()) return -1;

    // 纯数字：直接当作 MIDI 编号
    bool numeric = true;
    for (char c : raw) if (c < '0' || c > '9') { numeric = false; break; }
    if (numeric) {
        const int v = std::stoi(raw);
        return (v >= 0 && v <= 127) ? v : -1;
    }

    static const int base[7] = { 9, 11, 0, 2, 4, 5, 7 };  // A B C D E F G
    char letter = raw[0];
    if (letter >= 'a' && letter <= 'g') letter = static_cast<char>(letter - 'a' + 'A');
    int idx = -1;
    for (int i = 0; i < 7; ++i) {
        const char candidates[3] = { "ABCDEFG"[i], "abcdefg"[i], 0 };
        if (letter == candidates[0]) { idx = i; break; }
    }
    if (idx < 0) return -1;

    int pos = 1;
    int semi = base[idx];
    if (pos < static_cast<int>(raw.size())) {
        const char c = raw[pos];
        if (c == '#' || c == 's' || c == 'S') { semi += 1; ++pos; }
        else if (c == 'b' || c == 'B')        { semi -= 1; ++pos; }
    }
    if (pos >= static_cast<int>(raw.size())) return -1;
    const int octave = std::stoi(raw.substr(pos));
    return (octave + 1) * 12 + semi;
}

} // namespace piano
