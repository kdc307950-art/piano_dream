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

    // ---- 欢乐颂（贝多芬第九交响曲主题）----
    {
        Song s;
        s.id = "ode";
        s.title = "欢乐颂";
        s.composer = "L. van Beethoven";
        s.bpm = 92;
        s.difficulty = 1;
        addMelody(s, { 64, 64, 65, 67, 67, 65, 64, 62, 60, 60, 62, 64, 64, 62, 62, 60 },
                  480, 960, 4);
        addAccompaniment(s, { 48, 55, 45, 48 }, 1920);
        s.sortEvents();
        songs.push_back(std::move(s));
    }

    // ---- 小星星 ----
    {
        Song s;
        s.id = "twinkle";
        s.title = "小星星";
        s.composer = "法国民谣";
        s.bpm = 104;
        s.difficulty = 1;
        addMelody(s, { 60, 60, 67, 67, 69, 69, 67, 65, 65, 64, 64, 62, 62, 60 },
                  480, 960, 2);
        addAccompaniment(s, { 48, 55, 53, 48 }, 1920);
        s.sortEvents();
        songs.push_back(std::move(s));
    }

    // ---- 铃儿响叮当 ----
    {
        Song s;
        s.id = "jingle";
        s.title = "铃儿响叮当";
        s.composer = "J. Pierpont";
        s.bpm = 120;
        s.difficulty = 1;
        addMelody(s, { 64, 64, 64, 64, 64, 64, 64, 67, 60, 62, 64, 65, 65, 64, 62, 60 },
                  480, 960, 8);
        addAccompaniment(s, { 48, 48, 55, 48 }, 1920);
        s.sortEvents();
        songs.push_back(std::move(s));
    }

    // ---- 卡农片段（进阶：含黑键与更密集的音符）----
    {
        Song s;
        s.id = "canon";
        s.title = "卡农 · 片段";
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
