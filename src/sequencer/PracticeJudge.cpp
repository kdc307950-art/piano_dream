#include "sequencer/PracticeJudge.h"

#include <algorithm>
#include <cmath>

#include "core/Note.h"

namespace piano {
namespace {

const char* gradeText(PracticeJudge::Grade g) {
    switch (g) {
    case PracticeJudge::Grade::Perfect: return "完美";
    case PracticeJudge::Grade::Good:    return "不错";
    case PracticeJudge::Grade::Miss:    return "漏音";
    default:                            return "多余";
    }
}

} // namespace

void PracticeJudge::configure(double sampleRate, const Config& cfg) {
    sampleRate_ = sampleRate > 1000.0 ? sampleRate : 48000.0;
    cfg_ = cfg;
    if (cfg_.goodMs < cfg_.perfectMs) cfg_.goodMs = cfg_.perfectMs;
}

void PracticeJudge::load(const Song& song, std::uint64_t anchorFrame, double speed, int transpose) {
    reset();
    if (song.events.empty()) return;

    const double fpt = framesPerTick(song.bpm, song.ppq, sampleRate_, speed);
    const bool hasMelody = song.hasTrack(kTrackMelody);

    expected_.reserve(song.events.size());
    for (const auto& e : song.events) {
        if (cfg_.melodyOnly && hasMelody && e.track != kTrackMelody) continue;
        Expected ex;
        ex.frame = anchorFrame + static_cast<std::uint64_t>(static_cast<double>(e.tick) * fpt + 0.5);
        ex.note  = clampNote(static_cast<int>(e.note) + transpose);
        ex.done  = false;
        expected_.push_back(ex);
    }
    std::stable_sort(expected_.begin(), expected_.end(),
                     [](const Expected& a, const Expected& b) { return a.frame < b.frame; });
    active_ = true;
}

void PracticeJudge::reset() {
    expected_.clear();
    cursor_ = 0;
    recent_.clear();
    perfect_ = good_ = miss_ = extra_ = 0;
    combo_ = maxCombo_ = 0;
    lastText_.clear();
    lastTextAge_ = 0.0;
    active_ = false;
}

double PracticeJudge::accuracy() const noexcept {
    const int j = judged();
    if (j <= 0) return 0.0;
    return (static_cast<double>(perfect_) + static_cast<double>(good_) * 0.7) / static_cast<double>(j);
}

int PracticeJudge::score() const noexcept {
    // 与常见音游口径接近：完美 100 分，不错 60 分，漏音 0 分，再按总音符数归一
    const int t = total();
    if (t <= 0) return 0;
    const double raw = static_cast<double>(perfect_) * 100.0 + static_cast<double>(good_) * 60.0;
    return static_cast<int>(std::lround(raw / static_cast<double>(t) * 10.0));
}

void PracticeJudge::record(Grade grade, int note, std::uint64_t frame, double deltaMs) {
    Judgement j;
    j.grade = grade;
    j.note = note;
    j.frame = frame;
    j.deltaMs = deltaMs;

    recent_.push_back(j);
    if (recent_.size() > 64) recent_.erase(recent_.begin());

    lastText_ = std::string(gradeText(grade));
    if (grade == Grade::Perfect || grade == Grade::Good) {
        const char* sign = deltaMs >= 0 ? "+" : "";
        lastText_ += "  " + noteName(note) + "  " + sign + std::to_string(static_cast<int>(deltaMs)) + "ms";
    } else if (grade == Grade::Extra) {
        lastText_ += "  " + noteName(note);
    }
    lastTextAge_ = 0.0;
}

void PracticeJudge::onUserNoteOn(int midi, std::uint64_t frame) {
    if (!active_ || expected_.empty()) return;

    const double goodFrames = cfg_.goodMs * 0.001 * sampleRate_;

    // 在窗口内找最近的、尚未判定的同音高期望音符
    int   bestIdx = -1;
    double bestDist = goodFrames + 1.0;
    for (std::size_t i = 0; i < expected_.size(); ++i) {
        const Expected& ex = expected_[i];
        if (ex.done || ex.note != midi) continue;
        const double d = static_cast<double>(frame) - static_cast<double>(ex.frame);
        if (std::fabs(d) > goodFrames) continue;
        if (std::fabs(d) < bestDist) { bestDist = std::fabs(d); bestIdx = static_cast<int>(i); }
    }

    if (bestIdx < 0) {
        ++extra_;
        record(Grade::Extra, midi, frame, 0.0);
        return;
    }

    Expected& ex = expected_[static_cast<std::size_t>(bestIdx)];
    ex.done = true;
    const double deltaMs = (static_cast<double>(frame) - static_cast<double>(ex.frame)) / sampleRate_ * 1000.0;

    if (std::fabs(deltaMs) <= cfg_.perfectMs) {
        ++perfect_;
        ++combo_;
    } else {
        ++good_;
        ++combo_;
    }
    maxCombo_ = std::max(maxCombo_, combo_);
    record(std::fabs(deltaMs) <= cfg_.perfectMs ? Grade::Perfect : Grade::Good, midi, frame, deltaMs);
}

void PracticeJudge::update(std::uint64_t nowFrame) {
    if (!active_ || expected_.empty()) return;

    const double goodFrames = cfg_.goodMs * 0.001 * sampleRate_;
    while (cursor_ < expected_.size()) {
        Expected& ex = expected_[cursor_];
        if (ex.done) { ++cursor_; continue; }
        if (static_cast<double>(ex.frame) + goodFrames >= static_cast<double>(nowFrame)) break;
        // 超时未弹到
        ex.done = true;
        ++miss_;
        combo_ = 0;
        record(Grade::Miss, ex.note, nowFrame, 0.0);
        ++cursor_;
    }
}

void PracticeJudge::finish(std::uint64_t nowFrame) {
    if (expected_.empty()) return;
    for (auto& ex : expected_) {
        if (ex.done) continue;
        ex.done = true;
        ++miss_;
        combo_ = 0;
        record(Grade::Miss, ex.note, nowFrame, 0.0);
    }
    cursor_ = expected_.size();
    active_ = false;
}

} // namespace piano
