#include "songs/BuiltinSongs.h"

#include <algorithm>

#include "core/Note.h"

namespace piano {
namespace {

/// 旋律描述：音高序列，每 4 个音的第 4 个时值加倍（与 HTML 原版的节奏处理一致）。
void addMelody(Song& song, const std::vector<int>& notes, std::uint32_t step,
               std::uint32_t longStep, int longEvery) {
    for (std::size_t i = 0; i < notes.size(); ++i) {
        NoteEvent e;
        e.tick     = static_cast<std::uint32_t>(i) * step;
        const bool isLong = (longEvery > 0) && ((static_cast<int>(i) % longEvery) == longEvery - 1);
        e.duration = isLong ? longStep : step;
        e.note     = static_cast<std::uint8_t>(notes[i]);
        e.velocity = 104;
        e.track    = kTrackMelody;
        song.events.push_back(e);
    }
}

/// 伴奏：每小节一个和弦根音（长音）。
void addAccompaniment(Song& song, const std::vector<int>& notes, std::uint32_t barTicks) {
    for (std::size_t i = 0; i < notes.size(); ++i) {
        NoteEvent e;
        e.tick     = static_cast<std::uint32_t>(i) * barTicks;
        e.duration = barTicks;
        e.note     = static_cast<std::uint8_t>(notes[i]);
        e.velocity = 72;
        e.track    = kTrackAccomp;
        song.events.push_back(e);
    }
}

} // namespace

std::vector<Song> builtinSongs() {
    std::vector<Song> songs;

    // ---- 卡农（帕赫贝尔主题片段）----
    {
        Song s;
        s.id = "canon";
        s.title = "卡农";
        s.composer = "J. Pachelbel";
        s.bpm = 84;
        s.difficulty = 2;
        addMelody(s, { 76, 74, 72, 69, 67, 69, 72, 74, 76, 74, 72, 71, 72, 74, 76, 79,
                       81, 79, 76, 74, 72, 71, 69, 67, 69, 71, 72, 74, 76, 74, 72, 69 },
                  480, 480, 0);
        addAccompaniment(s, { 48, 55, 52, 57, 53, 60, 55, 50 }, 1920);
        s.sortEvents();
        songs.push_back(std::move(s));
    }

    // ---- 致爱丽丝 ----
    {
        Song s;
        s.id = "fur-elise";
        s.title = "致爱丽丝";
        s.composer = "L. van Beethoven";
        s.bpm = 108;
        s.difficulty = 1;
        addMelody(s, { 76, 75, 76, 75, 76, 71, 74, 72, 69, 52, 60, 69, 71, 60, 64, 69,
                       72, 60, 76, 75, 76, 75, 76, 71, 74, 72, 69, 52, 60, 69, 72, 71 },
                  360, 720, 8);
        addAccompaniment(s, { 45, 52, 48, 55, 45, 52, 48, 55 }, 1440);
        s.sortEvents();
        songs.push_back(std::move(s));
    }

    // ---- 月光奏鸣曲 ----
    {
        Song s;
        s.id = "moonlight";
        s.title = "月光奏鸣曲";
        s.composer = "L. van Beethoven";
        s.bpm = 72;
        s.difficulty = 2;
        addMelody(s, { 57, 60, 64, 60, 57, 60, 64, 60, 57, 60, 65, 60, 55, 59, 64, 59,
                       57, 60, 64, 60, 57, 60, 65, 60 }, 480, 480, 0);
        addAccompaniment(s, { 45, 52, 48, 55, 45, 52, 48, 55 }, 1920);
        s.sortEvents();
        songs.push_back(std::move(s));
    }

    // ---- 天空之城 ----
    {
        Song s;
        s.id = "castle-in-sky";
        s.title = "天空之城";
        s.composer = "久石让";
        s.bpm = 96;
        s.difficulty = 1;
        addMelody(s, { 72, 74, 76, 79, 81, 79, 76, 74, 72, 74, 76, 79, 84, 81, 79, 76,
                       72, 74, 76, 79, 81, 79, 76, 74 }, 480, 960, 4);
        addAccompaniment(s, { 48, 55, 52, 57, 48, 55, 52, 57 }, 1920);
        s.sortEvents();
        songs.push_back(std::move(s));
    }

    // ---- River Flows in You ----
    {
        Song s;
        s.id = "river-flows-in-you";
        s.title = "River Flows in You";
        s.composer = "Yiruma";
        s.bpm = 92;
        s.difficulty = 1;
        addMelody(s, { 69, 72, 76, 72, 69, 72, 77, 76, 69, 72, 76, 72, 67, 71, 74, 71,
                       69, 72, 76, 72, 69, 72, 77, 81 }, 480, 960, 4);
        addAccompaniment(s, { 45, 52, 48, 55, 45, 52, 48, 55 }, 1920);
        s.sortEvents();
        songs.push_back(std::move(s));
    }

    return songs;
}

bool validateSong(const Song& song, std::string* why) {
    if (song.events.empty()) {
        if (why) *why = "曲目没有音符";
        return false;
    }
    if (song.bpm < 20 || song.bpm > 400) {
        if (why) *why = "BPM 超出合理范围：" + std::to_string(song.bpm);
        return false;
    }
    if (song.ppq <= 0) {
        if (why) *why = "PPQ 非法";
        return false;
    }
    for (const auto& e : song.events) {
        if (e.note < kMidiLow || e.note > kMidiHigh) {
            if (why) *why = "音高越界：" + noteName(e.note) + "（" + std::to_string(e.note) + "）";
            return false;
        }
        if (e.duration == 0) {
            if (why) *why = "存在零时值音符";
            return false;
        }
        if (e.velocity == 0) {
            if (why) *why = "存在零力度音符";
            return false;
        }
    }
    return true;
}

} // namespace piano
