#pragma once
//
// input/KeyboardMap.h —— 电脑键盘 → 音高 的映射表
//
// 刻意做成「数据 + 查表」，而不是一串 if/else：
//   - 换键位只改表，不动逻辑
//   - 琴键上的快捷键标签直接从表里取
//   - 单元测试可以整体替换映射（例如换成德沃夏克布局或 MIDI 吉他布局）
//
// 键位沿用 HTML 原版：
//   下排 Z S X D C V G B H N J M   → C3 起的 12 个半音
//   上排 Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P → C4 起的 17 个半音
// 注意：raylib 的字母/数字键码就是 ASCII 大写值（KEY_Z = 90 = 'Z'），
// 所以这里用 char 常量即可，无需引入 raylib。
//
#include <array>
#include <cstdint>
#include <vector>

#include "core/Note.h"

namespace piano {

/// 电脑键盘映射的基准音：offset 0 对应 MIDI 48（C3）。
inline constexpr int kKeyboardBaseNote = 48;
inline constexpr int kKeyboardMinShift = -2;   // 最低可移调 -2 个八度
inline constexpr int kKeyboardMaxShift = 2;    // 最高可移调 +2 个八度

struct KeyBinding {
    char        key = 0;      // ASCII 键码（与 raylib KeyboardKey 一致）
    std::int8_t offset = 0;   // 相对基准音的半音偏移
    const char* label = "";   // 显示在琴键上的字符
};

inline constexpr std::array<KeyBinding, 29> kDefaultBindings = { {
    { 'Z',  0, "Z" }, { 'S',  1, "S" }, { 'X',  2, "X" }, { 'D',  3, "D" },
    { 'C',  4, "C" }, { 'V',  5, "V" }, { 'G',  6, "G" }, { 'B',  7, "B" },
    { 'H',  8, "H" }, { 'N',  9, "N" }, { 'J', 10, "J" }, { 'M', 11, "M" },
    { 'Q', 12, "Q" }, { '2', 13, "2" }, { 'W', 14, "W" }, { '3', 15, "3" },
    { 'E', 16, "E" }, { 'R', 17, "R" }, { '5', 18, "5" }, { 'T', 19, "T" },
    { '6', 20, "6" }, { 'Y', 21, "Y" }, { '7', 22, "7" }, { 'U', 23, "U" },
    { 'I', 24, "I" }, { '9', 25, "9" }, { 'O', 26, "O" }, { '0', 27, "0" },
    { 'P', 28, "P" },
} };

class KeyboardMap {
public:
    KeyboardMap() { index_.fill(-1); }

    /// 整体替换映射表（允许外部配置）。
    void setBindings(const KeyBinding* table, std::size_t count) {
        if (count == 0) bindings_.clear();
        else bindings_.assign(table, table + count);
        rebuildIndex();
    }

    static const KeyboardMap& defaultMap() {
        static const KeyboardMap instance = [] {
            KeyboardMap m;
            m.setBindings(kDefaultBindings.data(), kDefaultBindings.size());
            return m;
        }();
        return instance;
    }

    void setOctaveShift(int shift) {
        octaveShift_ = shift < kKeyboardMinShift ? kKeyboardMinShift
                     : (shift > kKeyboardMaxShift ? kKeyboardMaxShift : shift);
    }
    int octaveShift() const noexcept { return octaveShift_; }

    /// 当前映射的起始音高。
    int baseNote() const noexcept { return kKeyboardBaseNote + octaveShift_ * 12; }

    /// 某个键码对应的音高；未绑定返回 -1。
    int noteForKey(int key) const {
        if (key < 0 || key > 255) return -1;
        const int idx = index_[static_cast<std::size_t>(key)];
        if (idx < 0) return -1;
        const int midi = baseNote() + bindings_[static_cast<std::size_t>(idx)].offset;
        return (midi >= kMidiLow && midi <= kMidiHigh) ? midi : -1;
    }

    /// 某个音高在当前移调下对应的键标签；无绑定返回 nullptr。
    const char* labelForNote(int midi) const {
        for (const auto& b : bindings_) {
            if (baseNote() + b.offset == midi) return b.label;
        }
        return nullptr;
    }

    const std::vector<KeyBinding>& bindings() const noexcept { return bindings_; }

private:
    void rebuildIndex() {
        index_.fill(-1);
        for (std::size_t i = 0; i < bindings_.size(); ++i) {
            const int k = static_cast<unsigned char>(bindings_[i].key);
            index_[static_cast<std::size_t>(k)] = static_cast<int>(i);
        }
    }

    std::vector<KeyBinding> bindings_;
    std::array<int, 256> index_{};
    int octaveShift_ = 0;
};

} // namespace piano
