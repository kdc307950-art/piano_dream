#pragma once
//
// sequencer/PracticeJudge.h —— 跟练判定
//
// 把曲目的「期望音符」按采样帧展开，与用户实际按键的采样帧比对：
//   Perfect  |Δ| <= perfectMs（默认 60ms）
//   Good     |Δ| <= goodMs（默认 150ms）
//   Miss     超出窗口仍未弹到
//   Extra    弹了曲谱上没有的音（不扣分，只统计）
//
// 只在 UI 线程使用。
//
#include <cstdint>
#include <string>
#include <vector>

#include "core/Song.h"

namespace piano {

class PracticeJudge {
public:
    enum class Grade : std::uint8_t { Perfect = 0, Good = 1, Miss = 2, Extra = 3 };

    struct Judgement {
        Grade grade = Grade::Miss;
        int   note = 0;
        double deltaMs = 0.0;
        std::uint64_t frame = 0;
    };

    struct Config {
        double perfectMs = 60.0;
        double goodMs    = 150.0;
        bool   melodyOnly = true;
    };

    /// 使用默认判定窗口（完美 ±60ms / 不错 ±150ms）。
    void configure(double sampleRate) {
        Config c;
        configure(sampleRate, c);
    }
    void configure(double sampleRate, const Config& cfg);

    /// 以 anchorFrame 为曲目 tick 0 的对应帧。
    void load(const Song& song, std::uint64_t anchorFrame, double speed, int transpose);
    void reset();

    bool loaded() const noexcept { return !expected_.empty(); }
    bool active() const noexcept { return active_; }
    void setActive(bool on) noexcept { active_ = on; }

    /// 用户按下某个音（frame 取 mixer.currentFrame()）。
    void onUserNoteOn(int midi, std::uint64_t frame);

    /// 推进时间轴，把超时未弹到的期望音符判为 Miss。
    void update(std::uint64_t nowFrame);

    /// 曲目结束时收尾（把剩余未判定的一并结算）。
    void finish(std::uint64_t nowFrame);

    // ---- 统计 ----
    int perfect() const noexcept { return perfect_; }
    int good() const noexcept { return good_; }
    int miss() const noexcept { return miss_; }
    int extra() const noexcept { return extra_; }
    int combo() const noexcept { return combo_; }
    int maxCombo() const noexcept { return maxCombo_; }
    int judged() const noexcept { return perfect_ + good_ + miss_; }
    int total() const noexcept { return static_cast<int>(expected_.size()); }
    double accuracy() const noexcept;
    int score() const noexcept;

    const std::vector<Judgement>& recent() const noexcept { return recent_; }
    /// 最近一次判定的中文描述（UI 直接显示）。
    const std::string& lastText() const noexcept { return lastText_; }
    double lastTextAgeMs() const noexcept { return lastTextAge_; }

private:
    struct Expected {
        std::uint64_t frame = 0;
        int  note = 0;
        bool done = false;
    };

    void record(Grade grade, int note, std::uint64_t frame, double deltaMs);

    std::vector<Expected> expected_;
    std::size_t cursor_ = 0;
    std::vector<Judgement> recent_;

    double sampleRate_ = 48000.0;
    Config cfg_{};
    bool active_ = false;

    int perfect_ = 0, good_ = 0, miss_ = 0, extra_ = 0;
    int combo_ = 0, maxCombo_ = 0;
    std::string lastText_;
    double lastTextAge_ = 0.0;
};

} // namespace piano
