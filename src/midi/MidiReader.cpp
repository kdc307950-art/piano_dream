#include "midi/MidiReader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

#include "core/Note.h"

namespace piano {
namespace {

struct Reader {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t pos  = 0;
    bool ok = true;

    std::uint8_t u8() {
        if (pos >= size) { ok = false; return 0; }
        return data[pos++];
    }
    std::uint16_t u16() {
        const std::uint16_t a = u8();
        const std::uint16_t b = u8();
        return static_cast<std::uint16_t>((a << 8) | b);
    }
    std::uint32_t u32() {
        const std::uint32_t a = u16();
        const std::uint32_t b = u16();
        return (a << 16) | b;
    }
    /// 变长数量（VLQ）
    std::uint32_t vlq() {
        std::uint32_t v = 0;
        int count = 0;
        for (;;) {
            const std::uint8_t b = u8();
            if (!ok) return v;
            v = (v << 7) | static_cast<std::uint32_t>(b & 0x7F);
            if ((b & 0x80) == 0) break;
            if (++count >= 4) break;   // 最长 4 字节
        }
        return v;
    }
    void skip(std::uint32_t n) {
        if (pos + n > size) { pos = size; ok = false; return; }
        pos += n;
    }
};

std::uint32_t readU32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8)  |
            static_cast<std::uint32_t>(p[3]);
}

} // namespace

MidiParseResult parseMidi(const std::uint8_t* data, std::size_t size) {
    MidiParseResult out;
    if (!data || size < 14) {
        out.error = "文件过小，不是有效的 MIDI 文件";
        return out;
    }
    if (std::memcmp(data, "MThd", 4) != 0) {
        out.error = "缺少 MThd 头，不是标准 MIDI 文件";
        return out;
    }

    const std::uint32_t headerLen = readU32(data + 4);
    if (headerLen < 6) {
        out.error = "MThd 块长度非法";
        return out;
    }

    Reader head{data, size, 8};
    const std::uint16_t format   = head.u16();
    const std::uint16_t ntrks    = head.u16();
    const std::uint16_t division = head.u16();
    if (!head.ok) {
        out.error = "MThd 内容不完整";
        return out;
    }

    out.format     = format;
    out.trackCount = ntrks;

    if (division & 0x8000) {
        out.error = "暂不支持 SMPTE 时间码格式的 MIDI 文件";
        return out;
    }
    const int ppq = division > 0 ? division : kDefaultPpq;
    out.ppq = ppq;

    std::size_t pos = 8 + headerLen;
    std::uint32_t tempoUs = 500000;   // 默认 120 BPM
    bool tempoSeen = false;
    int tempoChanges = 0;
    int skipped = 0;
    std::string title;
    std::vector<NoteEvent> events;
    std::vector<std::size_t> trackEventCounts(ntrks, 0);
    std::vector<std::uint16_t> eventTracks;

    for (std::uint16_t t = 0; t < ntrks; ++t) {
        if (pos + 8 > size) break;
        if (std::memcmp(data + pos, "MTrk", 4) != 0) break;

        const std::uint32_t len = readU32(data + pos + 4);
        const std::size_t trackStart = pos + 8;
        const std::size_t trackEnd   = std::min(size, trackStart + len);
        pos = trackEnd;

        Reader tr{data, trackEnd, trackStart};
        std::uint32_t cur = 0;
        std::uint8_t status = 0;

        // 每个音高一个 FIFO，正确处理同音高重叠（简单 map 会配对错）
        std::vector<std::vector<NoteEvent>> pending(128);

        while (tr.pos < trackEnd && tr.ok) {
            cur += tr.vlq();
            if (!tr.ok) break;

            std::uint8_t b = tr.u8();
            if (!tr.ok) break;

            if (b < 0x80) {
                // Running status：沿用上一个状态字节，当前字节是数据
                if (status == 0) { ++skipped; break; }
                --tr.pos;
                b = status;
            } else {
                status = b;
            }

            // ---- Meta 事件 ----
            if (b == 0xFF) {
                const std::uint8_t type = tr.u8();
                const std::uint32_t mlen = tr.vlq();
                if (!tr.ok) break;
                if (type == 0x51 && mlen == 3) {
                    const std::uint32_t a = tr.u8();
                    const std::uint32_t c = tr.u8();
                    const std::uint32_t e = tr.u8();
                    if (!tempoSeen) {
                        tempoUs = (a << 16) | (c << 8) | e;
                        tempoSeen = true;
                    }
                    ++tempoChanges;
                } else if (type == 0x03 && title.empty() && mlen > 0) {
                    const std::size_t avail = std::min<std::size_t>(mlen, trackEnd - tr.pos);
                    std::string s(reinterpret_cast<const char*>(data + tr.pos), avail);
                    // 去掉尾部空白与 NUL
                    while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
                    title = s;
                    tr.skip(mlen);
                } else {
                    tr.skip(mlen);
                    ++skipped;
                }
                if (!tr.ok) break;
                continue;
            }

            // ---- SysEx ----
            if (b == 0xF0 || b == 0xF7) {
                const std::uint32_t slen = tr.vlq();
                tr.skip(slen);
                ++skipped;
                if (!tr.ok) break;
                continue;
            }

            const std::uint8_t kind = static_cast<std::uint8_t>(b & 0xF0);
            const std::uint8_t chan = static_cast<std::uint8_t>(b & 0x0F);

            // ---- 音符事件 ----
            if (kind == 0x90 || kind == 0x80) {
                const std::uint8_t note = tr.u8();
                const std::uint8_t vel  = tr.u8();
                if (!tr.ok) break;

                if (note < kMidiLow || note > kMidiHigh) { ++skipped; continue; }

                if (kind == 0x90 && vel > 0) {
                    NoteEvent ev;
                    ev.tick     = cur;
                    ev.note     = note;
                    ev.velocity = vel;
                    ev.channel  = chan;
                    // Format 1 的轨道 0 往往只有 Tempo/拍号元事件；先按轨道收集，
                    // 解析结束后再把音符数最多的轨道标为旋律。
                    ev.track = (ntrks > 1) ? kTrackAccomp
                                           : (chan == 0 ? kTrackMelody : kTrackAccomp);
                    pending[note].push_back(ev);
                    if (t < trackEventCounts.size()) ++trackEventCounts[t];
                } else {
                    // velocity == 0 的 NoteOn 同样视为 NoteOff
                    auto& q = pending[note];
                    if (!q.empty()) {
                        NoteEvent ev = q.front();
                        q.erase(q.begin());
                        ev.duration = std::max<std::uint32_t>(1, cur - ev.tick);
                        events.push_back(ev);
                        eventTracks.push_back(t);
                    } else {
                        ++skipped;
                    }
                }
                continue;
            }

            // ---- 单数据字节的通道消息 ----
            if (kind == 0xC0 || kind == 0xD0) { tr.skip(1); continue; }

            // ---- 双数据字节的通道消息 ----
            tr.skip(2);
            if (!tr.ok) break;
        }

        // 轨道结束仍未收到 NoteOff 的音符：给一个不跨越轨道末尾的时长
        for (auto& q : pending) {
            for (auto& ev : q) {
                ev.duration = std::max<std::uint32_t>(1, cur > ev.tick ? cur - ev.tick : ppq / 4);
                events.push_back(ev);
                eventTracks.push_back(t);
            }
        }
    }

    if (ntrks > 1 && !events.empty()) {
        std::size_t melodyTrack = 0;
        for (std::size_t t = 1; t < trackEventCounts.size(); ++t)
            if (trackEventCounts[t] > trackEventCounts[melodyTrack]) melodyTrack = t;
        for (std::size_t i = 0; i < events.size() && i < eventTracks.size(); ++i)
            events[i].track = eventTracks[i] == melodyTrack ? kTrackMelody : kTrackAccomp;
    }

    if (events.empty()) {
        out.error = "文件中没有可播放的音符事件";
        return out;
    }

    int bpm = static_cast<int>(std::lround(60000000.0 / static_cast<double>(tempoUs > 0 ? tempoUs : 500000)));
    bpm = std::max(40, std::min(240, bpm));

    out.song.id         = "import";
    out.song.title      = title.empty() ? "导入的 MIDI" : title;
    out.song.composer   = "用户文件";
    out.song.bpm        = bpm;
    out.song.ppq        = ppq;
    out.song.difficulty = 2;
    out.song.userImport = true;
    out.song.events     = std::move(events);
    out.song.sortEvents();

    out.tempoChanges = tempoChanges;
    out.skippedEvents = skipped;
    out.ok = true;
    return out;
}

MidiParseResult loadMidiFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        MidiParseResult r;
        r.error = "无法打开文件：" + path;
        return r;
    }
    std::vector<std::uint8_t> buf((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
    return parseMidi(buf.data(), buf.size());
}

} // namespace piano
