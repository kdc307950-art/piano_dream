#include "midi/MidiWriter.h"

#include <algorithm>
#include <cmath>
#include <fstream>

#include "core/Note.h"

namespace piano {
namespace {

void pushU16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
}

void pushU32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>((x >> 24) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
}

/// 变长数量（VLQ）：每字节低 7 位有效，最高位标记「后面还有」。
void pushVLQ(std::vector<std::uint8_t>& v, std::uint32_t n) {
    std::uint8_t buf[5];
    int i = 4;
    buf[4] = static_cast<std::uint8_t>(n & 0x7F);
    while ((n >>= 7) != 0)
        buf[--i] = static_cast<std::uint8_t>((n & 0x7F) | 0x80);
    v.insert(v.end(), buf + i, buf + 5);
}

struct RawEvent {
    std::uint32_t tick = 0;
    int order = 0;                        // 同一 tick 内的排序：越小越先
    std::vector<std::uint8_t> bytes;
};

/// 组装 MThd + MTrk。
std::vector<std::uint8_t> assemble(const std::vector<RawEvent>& rawsIn, int ppq) {
    std::vector<RawEvent> raws = rawsIn;
    std::stable_sort(raws.begin(), raws.end(), [](const RawEvent& a, const RawEvent& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        return a.order < b.order;
    });

    std::vector<std::uint8_t> payload;
    std::uint32_t prev = 0;
    for (const auto& e : raws) {
        const std::uint32_t delta = e.tick >= prev ? e.tick - prev : 0;
        pushVLQ(payload, delta);
        payload.insert(payload.end(), e.bytes.begin(), e.bytes.end());
        prev = e.tick;
    }
    // End of Track
    pushVLQ(payload, 0);
    payload.push_back(0xFF);
    payload.push_back(0x2F);
    payload.push_back(0x00);

    std::vector<std::uint8_t> out;
    out.reserve(payload.size() + 32);
    const char mthd[4] = { 'M', 'T', 'h', 'd' };
    out.insert(out.end(), mthd, mthd + 4);
    pushU32(out, 6);
    pushU16(out, 0);                                   // format 0
    pushU16(out, 1);                                   // 1 track
    pushU16(out, static_cast<std::uint16_t>(ppq > 0 ? ppq : kDefaultPpq));
    const char mtrk[4] = { 'M', 'T', 'r', 'k' };
    out.insert(out.end(), mtrk, mtrk + 4);
    pushU32(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

void pushTempo(std::vector<RawEvent>& raws, int bpm) {
    const int safeBpm = (bpm > 0 && bpm < 1000) ? bpm : kDefaultBpm;
    const std::uint32_t usPerBeat = static_cast<std::uint32_t>(60000000.0 / safeBpm + 0.5);
    RawEvent e;
    e.tick  = 0;
    e.order = -1;                                      // Tempo 永远排在最前
    e.bytes = { 0xFF, 0x51, 0x03,
                static_cast<std::uint8_t>((usPerBeat >> 16) & 0xFF),
                static_cast<std::uint8_t>((usPerBeat >> 8) & 0xFF),
                static_cast<std::uint8_t>(usPerBeat & 0xFF) };
    raws.push_back(std::move(e));
}

double framesToTicks(std::uint64_t frames, double sampleRate, int bpm, int ppq) {
    const double sr = sampleRate > 1.0 ? sampleRate : 48000.0;
    const int b = bpm > 0 ? bpm : kDefaultBpm;
    const int p = ppq > 0 ? ppq : kDefaultPpq;
    return static_cast<double>(frames) * static_cast<double>(p) * static_cast<double>(b) / (60.0 * sr);
}

} // namespace

std::vector<std::uint8_t> writeSongMidi(const Song& song, const MidiWriteOptions& opts) {
    const int bpm = opts.bpm > 0 ? opts.bpm : song.bpm;
    const int ppq = opts.ppq > 0 ? opts.ppq : song.ppq;

    std::vector<RawEvent> raws;
    raws.reserve(song.events.size() * 2 + 2);
    if (opts.withTempo) pushTempo(raws, bpm);

    for (const auto& e : song.events) {
        const std::uint8_t chan = static_cast<std::uint8_t>(e.channel & 0x0F);
        const std::uint8_t vel  = static_cast<std::uint8_t>(e.velocity < 1 ? 1 : (e.velocity > 127 ? 127 : e.velocity));
        RawEvent on;
        on.tick  = e.tick;
        on.order = 2;
        on.bytes = { static_cast<std::uint8_t>(0x90 | chan), e.note, vel };
        RawEvent off;
        off.tick  = e.endTick();
        off.order = 0;                                  // 同一 tick 上先关后开，避免同音高被吞
        off.bytes = { static_cast<std::uint8_t>(0x80 | chan), e.note, 0 };
        raws.push_back(std::move(on));
        raws.push_back(std::move(off));
    }
    return assemble(raws, ppq);
}

std::vector<std::uint8_t> writeRecordingMidi(const std::vector<RecordedEvent>& events,
                                             double sampleRate,
                                             std::uint64_t durationFrames,
                                             const MidiWriteOptions& opts) {
    const int bpm = opts.bpm > 0 ? opts.bpm : kDefaultBpm;
    const int ppq = opts.ppq > 0 ? opts.ppq : kDefaultPpq;

    std::vector<RawEvent> raws;
    raws.reserve(events.size() + 8);
    if (opts.withTempo) pushTempo(raws, bpm);

    // 统计每个音高的悬空次数，文件末尾统一补齐 NoteOff
    int balance[128] = { 0 };

    for (const auto& e : events) {
        RawEvent raw;
        raw.tick = static_cast<std::uint32_t>(framesToTicks(e.frame, sampleRate, bpm, ppq) + 0.5);
        switch (e.type) {
        case MsgType::NoteOn:
            raw.order = 2;
            raw.bytes = { 0x90, e.note, static_cast<std::uint8_t>(e.velocity < 1 ? 1 : e.velocity) };
            if (e.note < 128) ++balance[e.note];
            break;
        case MsgType::NoteOff:
            raw.order = 0;
            raw.bytes = { 0x80, e.note, 0 };
            if (e.note < 128 && balance[e.note] > 0) --balance[e.note];
            break;
        case MsgType::Sustain:
            raw.order = 1;
            raw.bytes = { 0xB0, 64, static_cast<std::uint8_t>(e.velocity > 0 ? 127 : 0) };
            break;
        default:
            continue;
        }
        raws.push_back(std::move(raw));
    }

    const std::uint32_t endTick =
        static_cast<std::uint32_t>(framesToTicks(durationFrames + static_cast<std::uint64_t>(sampleRate * 0.06),
                                                 sampleRate, bpm, ppq) + 0.5);

    for (int note = 0; note < 128; ++note) {
        for (int k = 0; k < balance[note]; ++k) {
            RawEvent raw;
            raw.tick  = endTick;
            raw.order = 0;
            raw.bytes = { 0x80, static_cast<std::uint8_t>(note), 0 };
            raws.push_back(std::move(raw));
        }
    }
    {
        RawEvent raw;
        raw.tick  = endTick;
        raw.order = 1;
        raw.bytes = { 0xB0, 64, 0 };                    // 踏板抬起
        raws.push_back(std::move(raw));
    }

    return assemble(raws, ppq);
}

Song recordingToSong(const std::vector<RecordedEvent>& events,
                     double sampleRate,
                     std::uint64_t durationFrames,
                     const MidiWriteOptions& opts) {
    Song song;
    song.id         = "recording";
    song.title      = "我的录制";
    song.composer   = "本机录制";
    song.bpm        = opts.bpm > 0 ? opts.bpm : kDefaultBpm;
    song.ppq        = opts.ppq > 0 ? opts.ppq : kDefaultPpq;
    song.difficulty = 1;

    std::vector<RecordedEvent> ordered = events;
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const RecordedEvent& a, const RecordedEvent& b) {
                         if (a.frame != b.frame) return a.frame < b.frame;
                         return static_cast<int>(a.type) < static_cast<int>(b.type);
                     });

    std::vector<std::vector<NoteEvent>> pending(128);
    for (const auto& e : ordered) {
        const std::uint32_t tick =
            static_cast<std::uint32_t>(framesToTicks(e.frame, sampleRate, song.bpm, song.ppq) + 0.5);
        if (e.note > 127) continue;
        if (e.type == MsgType::NoteOn) {
            NoteEvent ev;
            ev.tick     = tick;
            ev.note     = e.note;
            ev.velocity = e.velocity < 1 ? 1 : e.velocity;
            ev.track    = kTrackMelody;
            pending[e.note].push_back(ev);
        } else if (e.type == MsgType::NoteOff) {
            auto& q = pending[e.note];
            if (!q.empty()) {
                NoteEvent ev = q.front();
                q.erase(q.begin());
                ev.duration = std::max<std::uint32_t>(1, tick > ev.tick ? tick - ev.tick : 1);
                song.events.push_back(ev);
            }
        }
    }

    const std::uint32_t endTick =
        static_cast<std::uint32_t>(framesToTicks(durationFrames, sampleRate, song.bpm, song.ppq) + 0.5);
    for (auto& q : pending) {
        for (auto& ev : q) {
            ev.duration = std::max<std::uint32_t>(1, endTick > ev.tick ? endTick - ev.tick : song.ppq / 4);
            song.events.push_back(ev);
        }
    }

    song.sortEvents();
    return song;
}

bool saveMidiFile(const std::string& path, const std::vector<std::uint8_t>& bytes, std::string* error) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "无法写入文件：" + path;
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        if (error) *error = "写入过程中出错：" + path;
        return false;
    }
    return true;
}

bool saveMidiFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes, std::string* error) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "无法写入文件：" + path.string();
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        if (error) *error = "写入过程中出错：" + path.string();
        return false;
    }
    return true;
}

} // namespace piano
