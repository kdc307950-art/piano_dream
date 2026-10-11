//
// tests/selftest.cpp —— 离线自测
//
// 全部测试都不需要开声卡、不需要窗口：Mixer 是纯 DSP，可以直接离线渲染。
// 这正是把音频引擎拆成「Mixer（纯 DSP）+ AudioDevice（设备封装）」的价值。
//
// 运行：piano.exe --selftest
//
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "audio/Mixer.h"
#include "core/Note.h"
#include "core/SpscQueue.h"
#include "midi/MidiReader.h"
#include "midi/MidiInputParser.h"
#include "midi/MidiInputQueue.h"
#include "midi/MidiInputVirtualPlayer.h"
#include "midi/MidiWriter.h"
#include "sequencer/Player.h"
#include "sequencer/PracticeJudge.h"
#include "sequencer/Recorder.h"
#include "songs/BuiltinSongs.h"
#include "ui/UiStrings.h"

namespace piano {
namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool cond, const std::string& name) {
    if (cond) {
        ++g_pass;
        std::printf("  [ OK ] %s\n", name.c_str());
    } else {
        ++g_fail;
        std::printf("  [FAIL] %s\n", name.c_str());
    }
}

void section(const char* title) {
    std::printf("\n== %s ==\n", title);
}

// ---------------------------------------------------------------------------
// 1. 音高工具
// ---------------------------------------------------------------------------
void testNote() {
    section("core/Note");
    check(noteName(21) == "A0", "noteName(21) == A0");
    check(noteName(60) == "C4", "noteName(60) == C4");
    check(noteName(108) == "C8", "noteName(108) == C8");
    check(isBlackKey(61) && !isBlackKey(60), "黑键判定");
    check(whiteKeysBefore(108) == 51, "C8 之前的白键数为 51");
    check(kKeyCount == 88, "88 键");

    check(parseNoteName("C4") == 60, "parseNoteName(C4)");
    check(parseNoteName("Eb4") == 63, "parseNoteName(Eb4)");
    check(parseNoteName("D#4") == 63, "parseNoteName(D#4)");
    check(parseNoteName("F#3") == 54, "parseNoteName(F#3)");
    check(parseNoteName("A0") == 21, "parseNoteName(A0)");
    check(parseNoteName("60") == 60, "parseNoteName(60)");

    const double a4 = noteHz(69);
    const double c4 = noteHz(60);
    check(std::fabs(a4 - 440.0) < 1e-9, "A4 = 440Hz");
    check(std::fabs(c4 - 261.6255653) < 1e-4, "C4 ≈ 261.63Hz");
}

// ---------------------------------------------------------------------------
// 2. 无锁队列
// ---------------------------------------------------------------------------
void testQueue() {
    section("core/SpscQueue");

    SpscQueue<int, 8> q;
    check(q.empty(), "初始为空");
    for (int i = 0; i < 7; ++i) check(q.push(i), "push #" + std::to_string(i));
    check(!q.push(999), "满队列拒绝写入（不阻塞）");
    check(q.size() == 7, "size == 7");

    bool orderOk = true;
    for (int i = 0; i < 7; ++i) {
        int v = -1;
        if (!q.pop(v) || v != i) orderOk = false;
    }
    check(orderOk, "FIFO 顺序正确");
    check(q.empty(), "取空后为空");

    // 环绕测试
    SpscQueue<int, 4> r;
    bool wrapOk = true;
    for (int round = 0; round < 20; ++round) {
        r.push(round);
        int v = -1;
        if (!r.pop(v) || v != round) wrapOk = false;
    }
    check(wrapOk, "环绕读写正确");
}

// ---------------------------------------------------------------------------
// 3. MIDI 读写
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> makeTestMidi() {
    // 手工构造一个 Format 0 文件，覆盖：Tempo 元事件、Running status、
    // velocity 为 0 的 NoteOn（等价 NoteOff）、变长数量 480 与 240。
    std::vector<std::uint8_t> track = {
        0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,      // tempo 500000us = 120BPM
        0x00, 0x90, 0x3C, 0x40,                        // NoteOn C4 vel 64
        0x83, 0x60, 0x3C, 0x00,                        // delta 480, running status, vel 0 -> NoteOff
        0x00, 0x90, 0x3E, 0x50,                        // NoteOn D4 vel 80
        0x81, 0x70, 0x80, 0x3E, 0x00,                  // delta 240, NoteOff D4
        0x00, 0xFF, 0x2F, 0x00                         // End of track
    };

    std::vector<std::uint8_t> out;
    const char mthd[4] = { 'M', 'T', 'h', 'd' };
    out.insert(out.end(), mthd, mthd + 4);
    out.insert(out.end(), { 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x01, 0x01, 0xE0 });
    const char mtrk[4] = { 'M', 'T', 'r', 'k' };
    out.insert(out.end(), mtrk, mtrk + 4);
    const std::uint32_t len = static_cast<std::uint32_t>(track.size());
    out.insert(out.end(), { static_cast<std::uint8_t>(len >> 24), static_cast<std::uint8_t>(len >> 16),
                            static_cast<std::uint8_t>(len >> 8), static_cast<std::uint8_t>(len) });
    out.insert(out.end(), track.begin(), track.end());
    return out;
}

void testMidi() {
    section("midi/Reader");

    const std::vector<std::uint8_t> bytes = makeTestMidi();
    const MidiParseResult r = parseMidi(bytes.data(), bytes.size());
    check(r.ok, "解析成功");
    check(r.ppq == 480, "PPQ == 480");
    check(r.format == 0, "Format 0");
    check(r.song.bpm == 120, "Tempo 解析为 120 BPM");
    check(r.song.events.size() == 2, "解析出 2 个音符");

    if (r.song.events.size() == 2) {
        const NoteEvent& a = r.song.events[0];
        const NoteEvent& b = r.song.events[1];
        check(a.note == 60 && a.tick == 0 && a.duration == 480, "C4 tick 0 时长 480（running status 生效）");
        check(b.note == 62 && b.tick == 480 && b.duration == 240, "D4 tick 480 时长 240");
    }

    // 拒绝非 MIDI
    const std::uint8_t junk[16] = { 'N', 'O', 'P', 'E', 0, 0, 0, 6, 0, 0, 0, 1, 1, 0xE0, 0, 0 };
    const MidiParseResult bad = parseMidi(junk, sizeof(junk));
    check(!bad.ok, "非 MIDI 文件被拒绝");

    // Format 1：轨道 0 只有元事件，轨道 2 的音符最多；首个 Tempo 应生效。
    auto makeTrack = [](std::vector<std::uint8_t> body) {
        const std::uint32_t n = static_cast<std::uint32_t>(body.size());
        std::vector<std::uint8_t> t{ 'M','T','r','k', static_cast<std::uint8_t>(n >> 24),
                                     static_cast<std::uint8_t>(n >> 16), static_cast<std::uint8_t>(n >> 8),
                                     static_cast<std::uint8_t>(n) };
        t.insert(t.end(), body.begin(), body.end()); return t;
    };
    const std::vector<std::uint8_t> f1a = makeTrack({ 0,0xFF,0x51,3,0x0F,0x42,0x40, 0,0xFF,0x2F,0 }); // 60 BPM
    const std::vector<std::uint8_t> f1b = makeTrack({ 0,0x90,60,80, 0x81,0x70,0x80,60,0, 0,0xFF,0x2F,0 });
    const std::vector<std::uint8_t> f1c = makeTrack({ 0,0x90,62,80, 0x81,0x70,0x80,62,0, 0,0x90,64,80,
                                                       0x81,0x70,0x80,64,0, 0,0xFF,0x2F,0 });
    std::vector<std::uint8_t> f1{ 'M','T','h','d',0,0,0,6,0,1,0,3,1,0xE0 };
    for (const auto& t : { f1a, f1b, f1c }) f1.insert(f1.end(), t.begin(), t.end());
    const MidiParseResult rf1 = parseMidi(f1.data(), f1.size());
    check(rf1.ok && rf1.song.bpm == 60, "Format 1 只取首个 Tempo");
    int melodyNotes = 0, accompNotes = 0;
    for (const auto& e : rf1.song.events) (e.track == kTrackMelody ? melodyNotes : accompNotes)++;
    check(melodyNotes == 2 && accompNotes == 1, "Format 1 选择音符数最多的旋律轨");

    section("midi/Writer");
    Song s;
    s.id = "t";
    s.title = "test";
    s.bpm = 96;
    s.ppq = 480;
    NoteEvent e1{ 0, 480, 64, 100, 0, 0 };
    NoteEvent e2{ 480, 960, 67, 80, 0, 0 };
    s.events = { e1, e2 };

    const std::vector<std::uint8_t> written = writeSongMidi(s);
    check(written.size() > 22, "写出字节流非空");
    check(std::memcmp(written.data(), "MThd", 4) == 0, "MThd 头正确");

    const MidiParseResult back = parseMidi(written.data(), written.size());
    check(back.ok, "写出的文件能被自己解析");
    check(back.song.events.size() == 2, "往返后音符数一致");
    if (back.song.events.size() == 2) {
        check(back.song.events[0].note == 64 && back.song.events[0].tick == 0 &&
              back.song.events[0].duration == 480, "音符 1 往返一致");
        check(back.song.events[1].note == 67 && back.song.events[1].tick == 480 &&
              back.song.events[1].duration == 960, "音符 2 往返一致");
    }

    // 录制事件导出
    std::vector<RecordedEvent> rec;
    rec.push_back({ 0, MsgType::NoteOn, 60, 100 });
    rec.push_back({ 24000, MsgType::NoteOff, 60, 0 });
    rec.push_back({ 24000, MsgType::NoteOn, 64, 90 });
    rec.push_back({ 48000, MsgType::NoteOff, 64, 0 });
    const std::vector<std::uint8_t> recBytes = writeRecordingMidi(rec, 48000.0, 48000, {});
    const MidiParseResult recBack = parseMidi(recBytes.data(), recBytes.size());
    check(recBack.ok && recBack.song.events.size() == 2, "录制导出往返成功");
    if (recBack.ok && recBack.song.events.size() == 2) {
        // 48000 帧 @48kHz = 1 秒；120BPM/480PPQ 下 1 秒 = 960 tick
        check(recBack.song.events[0].tick == 0 && recBack.song.events[0].duration == 480,
              "录制时间戳换算正确（1 秒 = 960 tick）");
        check(recBack.song.events[1].tick == 480 && recBack.song.events[1].duration == 480,
              "第二个音符时间戳正确");
    }

    const Song fromRec = recordingToSong(rec, 48000.0, 48000, {});
    check(fromRec.events.size() == 2, "录制转 Song 成功");

    Recorder recorder;
    recorder.start(100);
    recorder.capture(100, MsgType::Sustain, 0, 127);
    recorder.capture(200, MsgType::Sustain, 0, 0);
    recorder.stop(300);
    check(recorder.events().size() == 2 && recorder.events()[0].type == MsgType::Sustain &&
          recorder.events()[1].velocity == 0, "录制流保留踏板 CC64 事件");
}

void testMidiInputParser() {
    section("midi/MidiInputParser");
    MidiInputParser parser;
    std::vector<MidiInputEvent> events;
    const std::vector<std::uint8_t> bytes = {
        0x90, 60, 100, 62, 0,             // running status + velocity-zero NoteOff
        0xF8,                              // realtime interleaved, ignored
        0x80, 60, 40,                     // explicit NoteOff
        0xB0, 64, 127, 64, 0,             // CC64 on/off with running status
        0xB0, 123, 0,                     // CC123 all notes off
        0xF0, 0x01, 0x02, 0xF7,           // SysEx ignored
        0x90, 65, 90,
    };
    for (const auto b : bytes) parser.feed(b, events);
    check(events.size() == 7, "解析 NoteOn/NoteOff、CC64、CC123 并忽略 SysEx");
    if (events.size() == 7) {
        check(events[0].type == MidiInputType::NoteOn && events[0].note == 60,
              "NoteOn 事件正确");
        check(events[1].type == MidiInputType::NoteOff && events[1].note == 62,
              "速度 0 的 NoteOn 转为 NoteOff");
        check(events[3].type == MidiInputType::Sustain && events[3].velocity == 127 &&
              events[4].type == MidiInputType::Sustain && events[4].velocity == 0,
              "CC64 踏板开关正确");
        check(events[5].type == MidiInputType::AllNotesOff && events[6].note == 65,
              "CC123 和后续运行状态正确");
    }
    check(parser.ignoredCount() >= 4, "实时消息与 SysEx 被计数并忽略");

    parser.reset();
    events.clear();
    parser.feed(60, events);               // 没有状态字节
    parser.feed(0xF0, events);
    parser.feed(60, events);               // SysEx 内数据
    check(events.empty() && parser.ignoredCount() >= 2, "无状态数据和 SysEx 数据不会产生音符");
}

void testMidiInputQueue() {
    section("midi/MidiInputQueue");
    MidiInputQueue queue;
    for (int i = 0; i < 3; ++i) {
        queue.push({ MidiInputEvent{ MidiInputType::NoteOn, 0,
                                     static_cast<std::uint8_t>(60 + i), 100 },
                     static_cast<double>(i) * 0.01 });
    }
    std::vector<TimedMidiInputEvent> drained;
    check(queue.drain(drained) == 3 && drained.size() == 3 && drained[2].event.note == 62,
          "独立 MIDI 队列按顺序批量取出");
    check(queue.size() == 0, "批量取出后队列为空");

    for (std::size_t i = 0; i < MidiInputQueue::kCapacity + 1; ++i)
        queue.push({});
    check(queue.dropped() == 1, "队列满时丢弃并计数，不影响已入队事件");

    MidiInputClock clock(48000.0);
    clock.reset(1000, 10.0);
    check(clock.toFrame(10.5) == 25000 && clock.toFrame(9.0) == 1000,
          "MIDI 秒时间戳准确映射到音频帧");

    Song song;
    song.bpm = 120; song.ppq = 480;
    song.events.push_back(NoteEvent{ 480, 240, 60, 100, 0, 0 });
    MidiInputQueue virtualQueue;
    check(enqueueVirtualMidiSong(song, virtualQueue, 2.0) == 2,
          "虚拟 MIDI 曲目写入同一输入队列");
    drained.clear();
    virtualQueue.drain(drained);
    MidiInputClock virtualClock(48000.0);
    virtualClock.reset(0, 2.0);
    check(drained.size() == 2 && virtualClock.toFrame(drained[0].timestampSeconds) == 24000 &&
          virtualClock.toFrame(drained[1].timestampSeconds) == 36000,
          "虚拟 MIDI 事件时间戳可回放到音频帧");
}

void testLanguageTable() {
    section("ui/UiStrings");
    ui::setLanguage(ui::Language::Chinese);
    check(std::string(ui::t(ui::kGood)) == "不错", "中文文案为默认语言");
    ui::setLanguage(ui::Language::English);
    check(std::string(ui::t(ui::kGood)) == "Good" &&
          std::string(ui::t(ui::kTranspose)) == "Transpose", "英文文案表可切换");
    ui::setLanguage(ui::Language::Chinese);
}

// ---------------------------------------------------------------------------
// 4. 混音器（离线渲染）
// ---------------------------------------------------------------------------
void renderFrames(Mixer& mixer, std::uint32_t frames, std::vector<float>& scratch) {
    std::uint32_t done = 0;
    while (done < frames) {
        const std::uint32_t chunk = std::min<std::uint32_t>(512u, frames - done);
        mixer.render(scratch.data(), chunk);
        done += chunk;
    }
}

void testMixer() {
    section("audio/Mixer");

    Mixer mixer;
    mixer.prepare(48000.0);
    std::vector<float> buf(512 * 2);

    // 静默渲染
    mixer.render(buf.data(), 512);
    float maxAbs = 0.0f;
    for (float v : buf) maxAbs = std::max(maxAbs, std::fabs(v));
    check(maxAbs == 0.0f, "无声部时输出为 0");

    // 单音
    mixer.noteOn(60, 0.8f, Source::User);
    renderFrames(mixer, 4800, buf);
    float peak = 0.0f;
    for (std::uint32_t i = 0; i < 4800; ++i) peak = std::max(peak, std::fabs(buf[i % 1024]));
    check(mixer.visualCount(60) == 1, "按下后高亮计数为 1");
    check(mixer.noteSounding(60), "音符正在发声");

    // 数值健康度：整段渲染不得出现 NaN / Inf / 越界
    bool healthy = true;
    float globalPeak = 0.0f;
    for (int block = 0; block < 400; ++block) {
        mixer.render(buf.data(), 512);
        for (float v : buf) {
            if (!std::isfinite(v) || std::fabs(v) > 1.0001f) healthy = false;
            globalPeak = std::max(globalPeak, std::fabs(v));
        }
    }
    check(healthy, "输出无 NaN/Inf 且不超过 ±1（软削波生效）");
    check(globalPeak > 0.001f, "确实有声音输出");

    // 自然衰减：钢琴音应当自行消失，而不是永久停在 sustain 电平
    {
        Mixer m2;
        m2.prepare(48000.0);
        m2.noteOn(72, 0.9f, Source::User);
        std::vector<float> b(1024);
        renderFrames(m2, static_cast<std::uint32_t>(48000 * 25), b);
        check(m2.activeVoiceCount() == 0, "高音 25 秒后自然衰减至静音");
    }

    // 复音上限与偷声
    {
        Mixer m3;
        m3.prepare(48000.0);
        std::vector<float> b(512 * 2);
        for (int i = 0; i < 100; ++i) m3.noteOn(40 + (i % 40), 0.8f, Source::User);
        renderFrames(m3, 4800, b);
        check(m3.activeVoiceCount() <= Mixer::kPolyphonyLimit,
              "复音数被限制在 " + std::to_string(Mixer::kPolyphonyLimit) + " 以内（实际 " +
              std::to_string(m3.activeVoiceCount()) + "）");
        bool finite = true;
        for (float v : b) if (!std::isfinite(v)) finite = false;
        check(finite, "大量偷声后输出仍然正常");
    }

    // 延音踏板
    {
        Mixer m4;
        m4.prepare(48000.0);
        std::vector<float> b(1024);
        m4.noteOn(60, 0.9f, Source::User);
        renderFrames(m4, 2400, b);

        m4.setSustain(true);
        m4.render(b.data(), 512);           // 让音频线程感知踏板下降沿
        m4.noteOff(60, Source::User);
        renderFrames(m4, 4800, b);
        check(m4.noteSounding(60), "踏板踩下时松键，声音被挂住继续响");

        m4.setSustain(false);
        renderFrames(m4, 24000, b);         // 0.5s，足够走完 230ms 释放
        check(!m4.noteSounding(60), "踏板抬起后声音被释放");
    }

    // 节拍器咔哒
    {
        Mixer m5;
        m5.prepare(48000.0);
        std::vector<float> b(512 * 2);      // render 写的是交错立体声，需要 2×帧数
        m5.triggerClick(1320.0f, 0.21f, 0.07f);
        m5.render(b.data(), 512);
        float p = 0.0f;
        for (float v : b) p = std::max(p, std::fabs(v));
        check(p > 0.001f, "节拍器咔哒能产生输出");
    }
}

// ---------------------------------------------------------------------------
// 5. 演奏器：采样点精确调度
// ---------------------------------------------------------------------------
void testPlayer() {
    section("sequencer/Player");

    Song s;
    s.id = "sched";
    s.bpm = 120;
    s.ppq = 480;
    NoteEvent e;
    e.tick = 480;          // 第 2 拍
    e.duration = 480;
    e.note = 60;
    e.velocity = 100;
    e.track = 0;
    s.events.push_back(e);

    Mixer mixer;
    mixer.prepare(48000.0);
    Player player;
    player.setSong(s, 48000.0);

    // framesPerTick = 48000 * 60 / (120 * 480) = 50
    check(std::fabs(player.framesPerTickValue() - 50.0) < 1e-9, "framesPerTick == 50");

    std::vector<float> scratch(2);
    player.play(0, 0.0);

    // 逐帧推进：一次 step = 先让调度器处理「当前帧」的事件，再渲染这一帧。
    auto step = [&]() {
        Player::process(&player, mixer, mixer.currentFrame(), 1);
        mixer.render(scratch.data(), 1);
    };

    // 渲染第 0 ~ 23999 帧
    for (int i = 0; i < 24000; ++i) step();
    check(mixer.visualCount(60) == 0, "第 23999 帧渲染完毕，音符尚未触发");

    step();   // 渲染第 24000 帧（= tick 480）
    check(mixer.visualCount(60) == 1, "第 24000 帧（tick 480）精确触发");

    // 再推进到第 48000 帧（= tick 960）。此刻 currentFrame 已是 24001，
    // 还需 23999 步才能恰好停在「第 47999 帧渲染完毕、currentFrame == 48000」。
    for (int i = 0; i < 23999; ++i) step();
    check(mixer.visualCount(60) == 1, "第 47999 帧渲染完毕，音符仍在");

    step();   // 渲染第 48000 帧
    check(mixer.visualCount(60) == 0, "第 48000 帧（tick 960）精确松键");

    // 播放中移调：NoteOff 必须使用 NoteOn 当时的实际音高。
    Song shifted;
    shifted.bpm = 120; shifted.ppq = 480;
    shifted.events.push_back(NoteEvent{0, 480, 60, 100, 0, 0});
    Player shiftedPlayer;
    shiftedPlayer.setSong(shifted, 48000.0);
    shiftedPlayer.play(0, 0.0);
    shiftedPlayer.setTranspose(2);
    std::vector<float> one(2);
    Player::process(&shiftedPlayer, mixer, mixer.currentFrame(), 1);
    mixer.render(one.data(), 1);
    shiftedPlayer.setTranspose(-2);
    for (int i = 0; i < 24000; ++i) {
        Player::process(&shiftedPlayer, mixer, mixer.currentFrame(), 1);
        mixer.render(one.data(), 1);
    }
    check(mixer.visualCount(62) == 0 && mixer.visualCount(58) == 0, "移调变化后 NoteOff 释放原实际音高");

    // 曲目结束
    player.stop();
    check(!player.playing(), "stop() 后停止");
}

// ---------------------------------------------------------------------------
// 6. 跟练判定
// ---------------------------------------------------------------------------
void testJudge() {
    section("sequencer/PracticeJudge");

    Song s;
    s.bpm = 120;
    s.ppq = 480;
    NoteEvent e;
    e.tick = 480;
    e.duration = 480;
    e.note = 60;
    e.track = 0;
    s.events.push_back(e);

    PracticeJudge j;
    j.configure(48000.0);
    j.load(s, 0, 1.0, 0);
    check(j.total() == 1, "期望音符数为 1");

    // 期望帧 = 480 * 50 = 24000；给一个 +10 帧（约 0.2ms）的命中
    j.onUserNoteOn(60, 24010);
    check(j.perfect() == 1, "±60ms 内判为完美");
    check(j.combo() == 1, "连击为 1");

    // 迟到的命中
    PracticeJudge j2;
    j2.configure(48000.0);
    j2.load(s, 0, 1.0, 0);
    j2.onUserNoteOn(60, 24000 + static_cast<std::uint64_t>(48000 * 0.10));   // +100ms
    check(j2.good() == 1, "60~150ms 内判为不错");

    // 漏音
    PracticeJudge j3;
    j3.configure(48000.0);
    j3.load(s, 0, 1.0, 0);
    j3.update(24000 + static_cast<std::uint64_t>(48000 * 0.30));
    check(j3.miss() == 1, "超过窗口判为漏音");

    // 多余音
    PracticeJudge j4;
    j4.configure(48000.0);
    j4.load(s, 0, 1.0, 0);
    j4.onUserNoteOn(72, 24000);
    check(j4.extra() == 1, "曲谱外的音计为多余");

    // 移调
    PracticeJudge j5;
    j5.configure(48000.0);
    j5.load(s, 0, 1.0, 2);
    j5.onUserNoteOn(62, 24000);
    check(j5.perfect() == 1, "移调 +2 后判定正确");
}

// ---------------------------------------------------------------------------
// 7. 内置曲目
// ---------------------------------------------------------------------------
void testSongs() {
    section("songs/BuiltinSongs");

    const std::vector<Song> songs = builtinSongs();
    check(songs.size() >= 4, "内置曲目数量 >= 4（实际 " + std::to_string(songs.size()) + "）");

    bool allValid = true;
    for (const auto& s : songs) {
        std::string why;
        if (!validateSong(s, &why)) {
            allValid = false;
            std::printf("       曲目「%s」不合法：%s\n", s.title.c_str(), why.c_str());
        }
    }
    check(allValid, "全部内置曲目数据合法（音高范围、时值、力度）");

    bool hasAccomp = true;
    for (const auto& s : songs)
        if (!s.hasTrack(kTrackAccomp)) hasAccomp = false;
    check(hasAccomp, "每首曲目都含伴奏轨");

    bool sorted = true;
    for (const auto& s : songs) {
        for (std::size_t i = 1; i < s.events.size(); ++i)
            if (s.events[i].tick < s.events[i - 1].tick) sorted = false;
    }
    check(sorted, "事件按 tick 升序排列");
}

} // namespace

int runSelfTest(bool verbose) {
    (void)verbose;
    // 关掉缓冲：这样即使某个测试中途崩掉，也能看到它跑到哪一步了
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    std::printf("Grand Studio · C++ 88 键钢琴 —— 离线自测\n");
    std::printf("(不需要声卡与窗口，全部为纯逻辑 / 纯 DSP 验证)\n");

    testNote();
    testQueue();
    testMidi();
    testMidiInputParser();
    testMidiInputQueue();
    testLanguageTable();
    testMixer();
    testPlayer();
    testJudge();
    testSongs();

    std::printf("\n----------------------------------------\n");
    std::printf("通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    std::printf("----------------------------------------\n");
    return g_fail == 0 ? 0 : 1;
}

} // namespace piano
