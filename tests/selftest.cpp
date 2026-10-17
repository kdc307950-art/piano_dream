//
// tests/selftest.cpp —— 离线自测
//
// 全部测试都不需要开声卡、不需要窗口：Mixer 是纯 DSP，可以直接离线渲染。
// 这正是把音频引擎拆成「Mixer（纯 DSP）+ AudioDevice（设备封装）」的价值。
//
// 运行：piano.exe --selftest
//
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <stdlib.h>   // setenv / _putenv_s / _wputenv_s

#include "audio/Mixer.h"
#include "core/Note.h"
#include "core/SpscQueue.h"
#include "core/UserData.h"
#include "core/VisualEvent.h"
#include "fx/FxSettings.h"
#include "fx/NoteParticles.h"
#include "library/SongLibrary.h"
#include "midi/MidiReader.h"
#include "midi/MidiInputParser.h"
#include "midi/MidiInputQueue.h"
#include "midi/MidiInputVirtualPlayer.h"
#include "midi/MidiWriter.h"
#include "sequencer/Player.h"
#include "sequencer/PracticeJudge.h"
#include "sequencer/Recorder.h"
#include "songs/BuiltinSongs.h"
#include "ui/Theme.h"
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

    MidiInputQueue releaseQueue;
    for (std::size_t i = 0; i < MidiInputQueue::kCapacity; ++i)
        releaseQueue.push({ { MidiInputType::NoteOn, 0, 60, 100 }, 0.0 });
    check(releaseQueue.push({ { MidiInputType::NoteOff, 0, 60, 0 }, 1.0 }),
          "队列满时 NoteOff 不丢弃");
    std::vector<TimedMidiInputEvent> releaseDrain;
    releaseQueue.drain(releaseDrain);
    check(!releaseDrain.empty() && releaseDrain.back().event.type == MidiInputType::NoteOff,
          "队列满后仍能取到 NoteOff");

    MidiInputClock clock(48000.0);
    clock.reset(1000, 10.0);
    check(clock.toFrame(10.5) == 25000 && clock.toFrame(9.0) == 1000,
          "MIDI 秒时间戳准确映射到音频帧");

    Song song;
    song.bpm = 120; song.ppq = 480;
    song.events.push_back(NoteEvent{ 960, 240, 64, 100, 0, 0 });
    song.events.push_back(NoteEvent{ 480, 240, 60, 100, 0, 0 }); // 故意逆序，验证入队前排序
    MidiInputQueue virtualQueue;
    check(enqueueVirtualMidiSong(song, virtualQueue, 2.0) == 4,
          "虚拟 MIDI 曲目写入同一输入队列");
    drained.clear();
    virtualQueue.drain(drained);
    MidiInputClock virtualClock(48000.0);
    virtualClock.reset(0, 2.0);
    check(drained.size() == 4 && drained[0].event.type == MidiInputType::NoteOn &&
          drained[0].event.note == 60 && virtualClock.toFrame(drained[0].timestampSeconds) == 24000 &&
          drained[1].event.type == MidiInputType::NoteOff &&
          virtualClock.toFrame(drained[1].timestampSeconds) == 36000,
          "虚拟 MIDI 事件按时间排序并可回放到音频帧");
}

void testLanguageTable() {
    section("ui/UiStrings");
    ui::setLanguage(ui::Language::Chinese);
    check(std::string(ui::t(ui::kGood)) == "不错", "中文文案为默认语言");
    ui::setLanguage(ui::Language::English);
    check(std::string(ui::t(ui::kGood)) == "Good" &&
          std::string(ui::t(ui::kTranspose)) == "Transpose", "英文文案表可切换");
    check(std::string(ui::t(ui::kFxLabel)) == "Notes FX" &&
          std::string(ui::t(ui::kFxHigh)) == "High" &&
          std::string(ui::t(ui::kFxReducedMotion)) == "Reduced motion",
          "特效设置文案有英文翻译");
    check(std::string(ui::t(ui::kMidiDisconnected)) == "MIDI not connected" &&
          std::string(ui::t(ui::kSettings)) == "Settings" &&
          std::string(ui::t(ui::kInstrumentName)) == "Grand piano",
          "工作区状态与设置文案有英文翻译");
    check(std::string(ui::t(ui::kMidiHint)) == "Connect a MIDI keyboard to play directly" &&
          std::string(ui::t(ui::kPracticeHint)) ==
              "Live practice session · chord timing tolerance enabled",
          "新增状态提示文案有英文翻译");
    ui::setLanguage(ui::Language::Chinese);
}

void testFxSettings() {
    section("fx/FxSettings");

    const fx::FxSettings def = fx::parseFxSettings("");
    check(def.level == 2 && !def.reducedMotion, "空文本回落默认（Standard / 关）");

    const fx::FxSettings a = fx::parseFxSettings("level=0\nreducedMotion=1\n");
    check(a.level == 0 && a.reducedMotion, "level=0 / reducedMotion=1 解析正确");

    const fx::FxSettings b = fx::parseFxSettings("level=3\n");
    check(b.level == 3 && !b.reducedMotion, "缺行容忍（reducedMotion 回落默认）");

    const fx::FxSettings c = fx::parseFxSettings("level=9\nreducedMotion=maybe\ngarbage\nlevel=x");
    check(c.level == 2 && !c.reducedMotion, "坏值/越界/无等号行全部回落默认");

    const fx::FxSettings d = fx::parseFxSettings(fx::formatFxSettings(fx::FxSettings{ 1, true }));
    check(d.level == 1 && d.reducedMotion, "format → parse 往返一致");
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

// ---------------------------------------------------------------------------
// 8. 音符粒子特效（阶段 1：纯逻辑）
// ---------------------------------------------------------------------------
void testNoteFx() {
    namespace fx = piano::fx;
    namespace tk = piano::theme::fx;
    section("fx/NoteParticles");

    // a. 容量不增长：High 档 500 次灌注，alive ≤ 160 且池容量恒定 160
    {
        fx::NoteFx fxh;
        fxh.seedRng(1u);
        fxh.setLevel(fx::Level::High);
        bool bounded = true;
        for (int i = 0; i < 500; ++i) {
            fxh.ingest(static_cast<std::uint8_t>(kMidiLow + (i % kKeyCount)),
                       0.6f, 0, fxh.now());
            fxh.update(0.005);
            if (fxh.aliveCount() > tk::kAliveCapHigh ||
                fxh.capacity() != tk::kPoolCapacity) bounded = false;
        }
        check(bounded, "High 档灌注 500 次：alive ≤ 160 且池容量恒定");

        fx::NoteFx fxl;
        fxl.seedRng(1u);
        fxl.setLevel(fx::Level::Low);
        bool lowBounded = true;
        for (int i = 0; i < 500; ++i) {
            fxl.ingest(static_cast<std::uint8_t>(kMidiLow + (i % kKeyCount)),
                       0.6f, 0, fxl.now());
            fxl.update(0.005);
            if (fxl.aliveCount() > tk::kAliveCapLow) lowBounded = false;
        }
        check(lowBounded, "Low 档灌注 500 次：alive ≤ 48");
    }

    // b. 寿命结束全部回收（含延迟队列中的第二音符）
    {
        fx::NoteFx fxb;
        fxb.seedRng(7u);
        fxb.setLevel(fx::Level::High);
        fxb.ingest(40, 0.9f, 0, fxb.now());
        check(fxb.pendingCount() == 1, "力度 ≥ 0.85 产生延迟第二音符");
        fxb.update(0.02);
        fxb.update(0.02);
        fxb.update(0.02);                   // now = 0.06 ≥ due，第二音符到期
        check(fxb.pendingCount() == 0 && fxb.aliveCount() == 2, "延迟 60ms 后第二音符生成");
        for (int i = 0; i < 4; ++i) {
            fxb.ingest(static_cast<std::uint8_t>(50 + i * 10), 0.9f, 0, fxb.now());
            fxb.update(0.02);
        }
        for (int i = 0; i < 200; ++i) fxb.update(tk::kDtMaxSeconds);   // 10s，远超最大寿命+延迟
        check(fxb.aliveCount() == 0 && fxb.pendingCount() == 0,
              "推进 10 秒后粒子与延迟队列全部回收");
    }

    // c. 30fps 与 144fps 推进结果一致（误差 ≤ 1e-4）
    //    双精度时钟沿不同步长累加，并非逐位相等；解析式设计保证误差在 1e-12 量级。
    {
        auto run = [](double dt, int steps, fx::RenderItem* out) {
            fx::NoteFx f;
            f.seedRng(42u);
            f.setLevel(fx::Level::Standard);
            f.setStageHeight(120.0f);
            f.ingest(48, 0.9f, 0, f.now());
            f.ingest(60, 0.5f, 1, f.now());
            f.ingest(72, 0.7f, 2, f.now());
            for (int i = 0; i < steps; ++i) f.update(dt);   // 总计 1.0s
            return f.collect(out, tk::kPoolCapacity);
        };
        fx::RenderItem a[tk::kPoolCapacity], b[tk::kPoolCapacity];
        const int na = run(1.0 / 30.0, 30, a);
        const int nb = run(1.0 / 144.0, 144, b);
        check(na == nb && na > 0, "30/144fps 收集数量一致");
        bool close = na == nb;
        for (int i = 0; i < na && close; ++i) {
            close = a[i].midi == b[i].midi && a[i].shape == b[i].shape &&
                    a[i].source == b[i].source &&
                    std::fabs(a[i].offsetX - b[i].offsetX) <= 1e-4f &&
                    std::fabs(a[i].offsetY - b[i].offsetY) <= 1e-4f &&
                    std::fabs(a[i].scale - b[i].scale) <= 1e-4f &&
                    std::fabs(a[i].alpha - b[i].alpha) <= 1e-4f &&
                    std::fabs(a[i].rotation - b[i].rotation) <= 1e-4f;
        }
        check(close, "30/144fps 逐字段误差 ≤ 1e-4");
    }

    // d. 同种子逐位一致；换种子结果不同
    {
        auto run = [](std::uint32_t seed, fx::RenderItem* out) {
            fx::NoteFx f;
            f.seedRng(seed);
            f.setLevel(fx::Level::Standard);
            f.setStageHeight(120.0f);
            f.ingest(48, 0.9f, 0, f.now());
            f.ingest(60, 0.5f, 1, f.now());
            for (int i = 0; i < 60; ++i) f.update(1.0 / 60.0);
            return f.collect(out, tk::kPoolCapacity);
        };
        fx::RenderItem a[tk::kPoolCapacity], b[tk::kPoolCapacity], c[tk::kPoolCapacity];
        const int na = run(123u, a);
        const int nb = run(123u, b);
        const int nc = run(456u, c);
        // 逐字段 == 比较，不用 memcmp：RenderItem 第 3 字节是 padding，
        // collect 的成员逐一拷贝不写它，栈上数组的 padding 是不确定的（ARM64 上尤其如此）。
        // 解析式设计在同平台必须逐位一致，字段比较不许放松成 epsilon。
        auto fieldEq = [](const fx::RenderItem& x, const fx::RenderItem& y) {
            return x.midi == y.midi && x.shape == y.shape && x.source == y.source &&
                   x.offsetX == y.offsetX && x.offsetY == y.offsetY && x.scale == y.scale &&
                   x.alpha == y.alpha && x.rotation == y.rotation;
        };
        bool sameBits = na == nb && na > 0;
        for (int i = 0; i < na && sameBits; ++i) sameBits = fieldEq(a[i], b[i]);
        check(sameBits, "同种子同 dt 序列：collect 逐位一致");
        bool differs = nc != na;
        for (int i = 0; i < na && i < nc && !differs; ++i) differs = !fieldEq(a[i], c[i]);
        check(differs, "换种子后至少一个字段不同");
    }

    // e. 映射单调性（含端点 21 / 108）
    {
        bool scaleVel = true, alphaVel = true, scaleMidi = true, lifeMidi = true;
        for (float v = 0.0f; v < 1.0f; v += 0.05f) {
            if (fx::NoteFx::baseScaleFor(v + 0.05f, 60) < fx::NoteFx::baseScaleFor(v, 60)) scaleVel = false;
            if (fx::NoteFx::baseAlphaFor(v + 0.05f) < fx::NoteFx::baseAlphaFor(v)) alphaVel = false;
        }
        for (int m = kMidiLow; m < kMidiHigh; ++m) {
            if (fx::NoteFx::baseScaleFor(0.8f, m + 1) > fx::NoteFx::baseScaleFor(0.8f, m)) scaleMidi = false;
            if (fx::NoteFx::baseLifetimeFor(m + 1) > fx::NoteFx::baseLifetimeFor(m)) lifeMidi = false;
        }
        check(scaleVel, "baseScaleFor 对 velocity 单调不减");
        check(scaleMidi, "baseScaleFor 对 midi 单调不增");
        check(lifeMidi, "baseLifetimeFor 对 midi 单调不增");
        check(alphaVel, "baseAlphaFor 对 velocity 单调不减");
        check(fx::NoteFx::baseLifetimeFor(kMidiLow) > fx::NoteFx::baseLifetimeFor(kMidiHigh) &&
              fx::NoteFx::baseScaleFor(0.8f, kMidiLow) > fx::NoteFx::baseScaleFor(0.8f, kMidiHigh),
              "端点 21/108：低音更久更大");
    }

    // f. 限流生效
    {
        fx::NoteFx fc;
        fc.seedRng(9u);
        fc.setLevel(fx::Level::High);
        fc.ingest(60, 0.5f, 0, fc.now());
        fc.update(0.05);                      // 50ms < 80ms 冷却
        fc.ingest(60, 0.5f, 0, fc.now());
        check(fc.aliveCount() == 1, "同 midi 间隔 50ms：冷却拒绝第二次");

        fx::NoteFx ff;
        ff.seedRng(9u);
        ff.setLevel(fx::Level::High);
        for (int m = 0; m < 20; ++m)
            ff.ingest(static_cast<std::uint8_t>(kMidiLow + m), 0.5f, 0, ff.now());
        check(ff.aliveCount() > 0 && ff.aliveCount() <= tk::kPerFrameSpawnMax,
              "同帧 20 个不同 midi：当帧生成 ≤ 6");

        fx::NoteFx fl;
        fl.seedRng(9u);
        fl.setLevel(fx::Level::Low);
        bool lowOk = true;
        for (int i = 0; i < 300; ++i) {
            fl.ingest(static_cast<std::uint8_t>(kMidiLow + (i % kKeyCount)), 0.5f, 0, fl.now());
            fl.update(0.01);
            if (fl.aliveCount() > tk::kAliveCapLow) lowOk = false;
        }
        check(lowOk && fl.aliveCount() > 0, "Low 档灌满后 alive ≤ 48");

        // 「少」档简化：摇摆归零在模块内完成（渲染层只跳过柔光）
        fx::RenderItem lowItems[tk::kPoolCapacity];
        const int lowN = fl.collect(lowItems, tk::kPoolCapacity);
        bool noSway = lowN > 0;
        for (int i = 0; i < lowN; ++i)
            if (lowItems[i].offsetX != 0.0f) noSway = false;
        check(noSway, "Low 档 collect 的 offsetX 全为 0");

        fx::NoteFx fo;
        fo.setLevel(fx::Level::Off);
        fo.ingest(60, 0.9f, 0, fo.now());
        fo.update(0.05);
        check(fo.aliveCount() == 0 && fo.pendingCount() == 0, "Off 档拒收一切");
    }

    // g. 减少动态效果：柔光点无位移，0.3s 后全部回收
    {
        fx::NoteFx fr;
        fr.seedRng(11u);
        fr.setLevel(fx::Level::Standard);
        fr.setReducedMotion(true);
        fr.ingest(60, 0.5f, 0, fr.now());
        fr.update(0.05);
        fr.update(0.05);                     // 推进到中程
        fx::RenderItem items[tk::kPoolCapacity];
        const int n = fr.collect(items, tk::kPoolCapacity);
        bool still = n > 0;
        for (int i = 0; i < n; ++i)
            if (items[i].offsetX != 0.0f || items[i].offsetY != 0.0f ||
                items[i].rotation != 0.0f || items[i].shape != 3) still = false;
        check(still, "减少动态效果：柔光点 offset/rotation 全为 0，shape == 3");
        for (int i = 0; i < 10; ++i) fr.update(tk::kDtMaxSeconds);   // 再推进 0.5s
        check(fr.aliveCount() == 0, "减少动态效果：0.3s 后全部回收");
    }

    // h. VisualEventQueue 双线程压力：数据完整，push 成功数 == pop 数
    {
        VisualEventQueue q;
        constexpr int kTotal = 200000;
        std::atomic<bool> done{false};
        std::atomic<int> pushed{0};
        std::atomic<int> dropped{0};
        std::atomic<int> popped{0};
        std::atomic<bool> intact{true};

        std::thread producer([&]() {
            for (int i = 0; i < kTotal; ++i) {
                VisualEvent ev;
                ev.audioFrame = static_cast<std::uint64_t>(i);
                ev.velocity   = 0.5f;
                ev.midi       = static_cast<std::uint8_t>(i & 0x7F);
                ev.source     = Source::User;
                if (q.push(ev)) pushed.fetch_add(1, std::memory_order_relaxed);
                else            dropped.fetch_add(1, std::memory_order_relaxed);
            }
            done.store(true, std::memory_order_release);
        });
        std::thread consumer([&]() {
            std::uint64_t last = 0;
            bool first = true;
            VisualEvent ev;
            while (!done.load(std::memory_order_acquire) || !q.empty()) {
                if (!q.pop(ev)) continue;
                const bool patternOk = ev.midi == static_cast<std::uint8_t>(ev.audioFrame & 0x7F) &&
                                       ev.velocity == 0.5f && ev.source == Source::User;
                const bool orderOk = first || ev.audioFrame > last;
                if (!patternOk || !orderOk) intact.store(false, std::memory_order_relaxed);
                first = false;
                last = ev.audioFrame;
                popped.fetch_add(1, std::memory_order_relaxed);
            }
        });
        producer.join();
        consumer.join();
        check(intact.load(), "队列并发压力：模式数据完整且严格有序");
        check(pushed.load() == popped.load() &&
              pushed.load() + dropped.load() == kTotal,
              "队列并发压力：push 成功数 == pop 数，丢弃数自洽");
    }

    // i. Mixer 推送视觉事件
    {
        Mixer mixer;
        mixer.prepare(48000.0);
        VisualEventQueue q;
        mixer.setVisualEventQueue(&q);
        std::vector<float> buf(512 * 2);

        mixer.noteOn(60, 0.8f, Source::User);
        VisualEvent ev;
        check(q.pop(ev), "noteOn(User) 后队列有事件");
        check(ev.midi == 60 && ev.source == Source::User &&
              std::fabs(ev.velocity - 0.8f) < 1e-6f && ev.audioFrame == 0,
              "事件字段正确（midi/velocity/source/audioFrame）");

        mixer.render(buf.data(), 512);
        mixer.noteOn(64, 5.0f, Source::Song);   // 力度超界 → Mixer 钳到 1.0
        check(q.pop(ev) && ev.midi == 64 && ev.source == Source::Song &&
              std::fabs(ev.velocity - 1.0f) < 1e-6f && ev.audioFrame == 512,
              "渲染 512 帧后事件带新帧号且力度已钳制");

        mixer.noteOn(65, 0.8f, Source::Metronome);
        check(!q.pop(ev), "Source::Metronome 不推送视觉事件");

        mixer.noteOn(66, 0.8f, Source::User);
        check(q.pop(ev), "摘除前事件仍可送达");
        mixer.setVisualEventQueue(nullptr);
        mixer.noteOn(67, 0.8f, Source::User);
        check(!q.pop(ev), "摘除队列后 noteOn 不再推送");
    }

    // 布局契约：三栏首屏、稳定键盘、运输栏与谱面在两种特效状态下对齐
    {
        UiLayout off;
        off.fxEnabled = false;
        off.compute(1520.0f, 1000.0f);
        check(off.fxStage.height == 0.0f && off.fxStage.width == 0.0f,
              "fx 关闭：fxStage 为空矩形");
        check(std::fabs(off.keys.y - (off.rail.y + 28.0f)) < 1e-4f &&
              std::fabs(off.keyH - 154.0f) < 1e-4f,
              "fx 关闭：琴键位置/高度符合新桌面布局");

        UiLayout on;
        on.fxEnabled = true;
        on.compute(1520.0f, 1000.0f);
        check(std::fabs(on.fxStage.height - 26.0f) < 1e-4f &&
              std::fabs(on.fxStage.y - (on.keys.y - 26.0f)) < 1e-4f,
              "fx 开启：舞台带位于琴键上方");
        check(std::fabs(on.keys.y - off.keys.y) < 1e-4f &&
              std::fabs(on.transport.y - off.transport.y) < 1e-4f,
              "fx 开启：特效不改变键盘与运输栏位置");
        check(std::fabs(off.controls.y - 84.0f) < 1e-3f && std::fabs(off.rail.y - 448.0f) < 1e-3f &&
              std::fabs(off.keys.y - 476.0f) < 1e-3f && std::fabs(off.transport.y - 668.0f) < 1e-3f &&
              std::fabs(off.song.y - 776.0f) < 1e-3f,
              "桌面布局：三栏/琴键/运输栏/谱面位置稳定");

        UiLayout narrow;
        narrow.compute(1280.0f, 880.0f);
        check(narrow.library.width >= 300.0f && narrow.controls.width >= 300.0f &&
              narrow.heroPanel.width > 0.0f && narrow.song.height >= 150.0f,
              "窄桌面布局：三栏仍有可用宽度");
        UiLayout wide;
        wide.compute(1920.0f, 1080.0f);
        check(wide.library.x + wide.library.width < wide.heroPanel.x &&
              wide.heroPanel.x + wide.heroPanel.width < wide.controls.x &&
              wide.keys.width == wide.console.width,
              "宽桌面布局：三栏与键盘横向边界不溢出");
    }
}

// ---------------------------------------------------------------------------
// 13. 用户曲库（SongLibrary + MidiReader 扩展 + UserData）
// ---------------------------------------------------------------------------

/// 带标题元事件的 Format 0 样例（标题长度须 < 128，单字节 VLQ）
std::vector<std::uint8_t> makeTestMidiTitled(const std::string& title) {
    std::vector<std::uint8_t> track = {
        0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,      // tempo 120BPM
        0x00, 0xFF, 0x03, static_cast<std::uint8_t>(title.size())
    };
    track.insert(track.end(), title.begin(), title.end());
    const std::vector<std::uint8_t> notes = {
        0x00, 0x90, 0x3C, 0x40,
        0x83, 0x60, 0x3C, 0x00,
        0x00, 0x90, 0x3E, 0x50,
        0x81, 0x70, 0x80, 0x3E, 0x00,
        0x00, 0xFF, 0x2F, 0x00
    };
    track.insert(track.end(), notes.begin(), notes.end());

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

bool writeBytesTo(const std::filesystem::path& p, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return !!out;
}

bool writeTextTo(const std::filesystem::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    return !!out;
}

std::string readTextFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool sameEvents(const std::vector<NoteEvent>& a, const std::vector<NoteEvent>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].tick != b[i].tick || a[i].duration != b[i].duration ||
            a[i].note != b[i].note || a[i].velocity != b[i].velocity ||
            a[i].track != b[i].track || a[i].channel != b[i].channel) return false;
    }
    return true;
}

void testSongLibrary() {
    namespace fs = std::filesystem;
    section("library/SongLibrary");

    // 临时目录刻意含中文名，验证 UTF-8 路径全程无碍
    std::u8string dirname = u8"曲库测试-";
    {
        const std::string num = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        dirname.insert(dirname.end(), num.begin(), num.end());
    }
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / dirname;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    check(!ec, "创建含中文名的临时目录");

    const std::vector<std::uint8_t> midiBytes = makeTestMidi();
    const MidiParseResult midiParsed = parseMidi(midiBytes.data(), midiBytes.size());

    // ---- a. 添加 → 重新 load → loadSong 事件逐项一致 ----------------------
    std::string idA;
    {
        const fs::path dir = base / "a";
        fs::create_directories(dir, ec);
        const fs::path src = dir / "样例.mid";
        check(writeBytesTo(src, midiBytes), "a: 写出测试 .mid");

        SongLibrary lib;
        check(lib.load(dir), "a: 空目录 load 成功");
        const SongLibrary::AddResult r = lib.add(src, std::nullopt);
        if (!r.ok()) std::printf("  [INFO] a: add code=%d message=%s\n", static_cast<int>(r.code), r.message.c_str());
        check(r.ok(), "a: add 成功");
        check(r.noteCount == 2, "a: noteCount == 2");
        idA = r.id;
        check(idA.rfind("u-", 0) == 0 && idA.size() == 18, "a: id 形如 u-+16hex");
        const SongLibrary::Entry* e = lib.find(idA);
        check(e && e->bpm == 120 && e->noteCount == 2 && e->melodyTrack == -1 &&
              e->hash == SongLibrary::fnv1a64(midiBytes.data(), midiBytes.size()),
              "a: 索引元数据正确");
        check(fs::exists(dir / (idA + ".mid"), ec), "a: 磁盘文件名只用 id");

        SongLibrary lib2;
        check(lib2.load(dir) && lib2.entries().size() == 1, "a: 重新 load 持久化生效");
        const Song loaded = lib2.loadSong(idA);
        check(sameEvents(loaded.events, midiParsed.song.events),
              "a: loadSong 事件与直接 parse 逐项一致");
        check(loaded.userImport && loaded.id == idA, "a: loadSong 标记 userImport 并保留 id");
    }

    // ---- b. 相同内容重复 add → Duplicate 且同一 id -------------------------
    {
        const fs::path dir = base / "a";
        const fs::path src = dir / "另一个名字.mid";   // 文件名不同、内容相同
        writeBytesTo(src, midiBytes);
        SongLibrary lib;
        lib.load(dir);
        const SongLibrary::AddResult dup = lib.add(src, std::nullopt);
        check(dup.code == SongLibrary::AddResult::Code::Duplicate && dup.id == idA,
              "b: 重复内容 → Duplicate 且指向已有 id");
        check(lib.entries().size() == 1, "b: 曲库仍只有 1 条");

        // 内容改一个字节 → 不同 hash → 正常入库
        std::vector<std::uint8_t> altered = midiBytes;
        altered[altered.size() - 10] = 0x51;   // D4 力度 80 → 81
        const fs::path src2 = dir / "改动版.mid";
        writeBytesTo(src2, altered);
        const SongLibrary::AddResult r2 = lib.add(src2, std::nullopt);
        check(r2.ok() && r2.id != idA, "b: 内容不同不判重、正常入库");
    }

    // ---- c. rename / setFavorite / remove 持久化 ----------------------------
    {
        const fs::path dir = base / "a";
        SongLibrary lib;
        lib.load(dir);
        check(lib.rename(idA, "新标题") && lib.setFavorite(idA, true), "c: rename/setFavorite 成功");
        SongLibrary lib2;
        lib2.load(dir);
        const SongLibrary::Entry* e = lib2.find(idA);
        check(e && e->title == "新标题" && e->favorite, "c: 重新 load 后改名与收藏生效");
        check(lib2.remove(idA), "c: remove 成功");
        check(!fs::exists(dir / (idA + ".mid"), ec), "c: remove 后 .mid 文件不存在");
        SongLibrary lib3;
        lib3.load(dir);
        check(lib3.find(idA) == nullptr, "c: remove 持久化生效");
    }

    // ---- d. 损坏行 / .bak 恢复 / 高版本只读 --------------------------------
    {
        const fs::path dir = base / "d";
        fs::create_directories(dir, ec);
        const fs::path src = dir / "s.mid";
        writeBytesTo(src, midiBytes);
        SongLibrary lib;
        lib.load(dir);
        lib.add(src, std::nullopt);
        const std::string goodIndex = readTextFile(dir / "index.txt");
        const std::string validLine = goodIndex.substr(goodIndex.find('\n') + 1);

        // 损坏行：字段数不对 ×1、垃圾行 ×1；有效行 1 条
        check(writeTextTo(dir / "index.txt",
                          "PIANO-LIBRARY 1\n" + validLine + "this is garbage\na|b|c\n"),
              "d: 写入含损坏行的索引");
        SongLibrary bad;
        bad.load(dir);
        check(bad.entries().size() == 1, "d: 损坏行被跳过，有效条目保留");
        check(bad.corruptLineCount() == 2, "d: corruptLineCount == 2");

        // .bak 恢复：再成功 add 一首让 .bak 存有旧索引，删掉 index.txt
        std::vector<std::uint8_t> alt = midiBytes;
        alt[alt.size() - 10] = 0x51;   // D4 力度 80 → 81，hash 变化
        writeBytesTo(dir / "s2.mid", alt);
        SongLibrary lib2;
        lib2.load(dir);
        lib2.add(dir / "s2.mid", std::nullopt);   // saveIndex 时把旧 index.txt 备份为 .bak
        fs::remove(dir / "index.txt", ec);
        SongLibrary lib3;
        lib3.load(dir);
        check(lib3.restoredFromBak(), "d: index.txt 缺失时从 index.bak 恢复");
        check(lib3.entries().size() == 1, "d: 恢复的是备份时点（1 条目）");

        // 版本号 > 1 → 只读，一切写操作失败
        writeTextTo(dir / "index.txt", "PIANO-LIBRARY 99\n" + validLine);
        SongLibrary ro;
        ro.load(dir);
        check(ro.readOnly(), "d: 版本 99 → readOnly");
        check(ro.entries().size() == 1, "d: readOnly 仍尽力解析条目");
        check(ro.add(src, std::nullopt).code == SongLibrary::AddResult::Code::ReadOnly,
              "d: readOnly 拒绝 add");
        const std::string anyId = ro.entries()[0].id;
        check(!ro.rename(anyId, "x") && !ro.remove(anyId) && !ro.setFavorite(anyId, true),
              "d: readOnly 拒绝 rename/remove/setFavorite");
    }

    // ---- e. 残留的 index.txt.tmp 不影响读写 --------------------------------
    {
        const fs::path dir = base / "e";
        fs::create_directories(dir, ec);
        writeTextTo(dir / "index.txt.tmp", "垃圾 tmp 内容\x00\x01");
        const fs::path src = dir / "s.mid";
        writeBytesTo(src, midiBytes);
        SongLibrary lib;
        check(lib.load(dir) && lib.entries().empty(), "e: 垃圾 tmp 不影响 load");
        check(lib.add(src, std::nullopt).ok(), "e: 下次 save 成功（tmp 被覆盖）");
        SongLibrary lib2;
        check(lib2.load(dir) && lib2.entries().size() == 1, "e: 重新 load 正常");
    }

    // ---- f. 标题净化与索引转义（中文/斜杠/../控制字符） ---------------------
    {
        const fs::path dir = base / "f";
        fs::create_directories(dir, ec);
        const std::string rawTitle = "  A/B..|\\\n\x01 中文 ";
        const std::vector<std::uint8_t> titled = makeTestMidiTitled(rawTitle);
        const fs::path src = dir / "t.mid";
        writeBytesTo(src, titled);
        SongLibrary lib;
        lib.load(dir);
        const SongLibrary::AddResult r = lib.add(src, std::nullopt);
        check(r.ok(), "f: 含脏标题的 MIDI 可入库");
        const SongLibrary::Entry* e = lib.find(r.id);
        // A2 起标题取文件名 stem 优先，轨名只在 stem 为空时兜底
        check(e && e->title == "t", "f: 标题取文件名 stem（A2 规则）");
        const std::string expected = SongLibrary::sanitizeTitle(rawTitle);
        check(expected.find('\n') == std::string::npos &&
              expected.find('\x01') == std::string::npos, "f: sanitizeTitle 去控制字符/trim");
        // 文件名只用 id：目录里除索引与源文件外只有 <id>.mid
        bool onlyIdFiles = true;
        for (const auto& de : fs::directory_iterator(dir, ec)) {
            const std::string fn = de.path().filename().string();
            if (fn != "index.txt" && fn != "index.bak" && fn != "t.mid" &&
                fn != r.id + ".mid") onlyIdFiles = false;
        }
        check(onlyIdFiles, "f: 曲库文件名只用 id（无标题命名的文件）");
        // 索引转义改走 rename 注入 | 与 \（stem 里不可能含这两个字符）
        check(lib.rename(r.id, "A|B\\C"), "f: rename 注入需转义的字符");
        const std::string idx = readTextFile(dir / "index.txt");
        check(idx.find("A\\|B\\\\C") != std::string::npos,
              "f: 索引中的 | 和 \\ 已转义（换行在净化阶段已去除，进不了索引）");
        SongLibrary lib2;
        lib2.load(dir);
        const SongLibrary::Entry* e2 = lib2.find(r.id);
        check(e2 && e2->title == "A|B\\C", "f: 转义往返后标题一致");

        // ---- k. collectTitleChars 覆盖条目标题字符 ----
        const std::string chars = lib2.collectTitleChars();
        bool covers = true;
        for (const char c : std::string("A|B\\C"))
            if (chars.find(c) == std::string::npos) { covers = false; break; }
        check(covers, "k: collectTitleChars 覆盖标题全部字符");
    }

    // ---- l. 标题裁决（A2）：stem 优先 / GBK 轨名拒绝 / isValidUtf8 ----------
    {
        check(SongLibrary::isValidUtf8("中文 en \xF0\x9F\x8E\xB9"), "l: 合法 UTF-8 通过");
        check(!SongLibrary::isValidUtf8("\x80"), "l: 孤立续字节拒绝");
        check(!SongLibrary::isValidUtf8("\xC0\x80"), "l: overlong 拒绝");
        check(!SongLibrary::isValidUtf8("\xED\xA0\x80"), "l: 代理区拒绝");
        check(!SongLibrary::isValidUtf8("\xE4\xB8"), "l: 截断序列拒绝");
        const std::string gbk = "\xC4\xE3\xBA\xC3";   // GBK「你好」，非法 UTF-8
        check(!SongLibrary::isValidUtf8(gbk), "l: GBK 字节序列被识别为非法 UTF-8");

        check(SongLibrary::pickTitle("歌名", "whatever") == "歌名", "l: stem 优先于轨名");
        check(SongLibrary::pickTitle("  ", "轨名") == "轨名", "l: stem 净化为空时退回轨名");
        check(SongLibrary::pickTitle("", gbk) == "imported", "l: GBK 轨名兜底 imported");
        check(SongLibrary::pickTitle("", "abc\xEF\xBF\xBD" "def") == "imported",
              "l: 含 U+FFFD 的轨名兜底 imported");

        // 真实 add：GBK 轨名 + 中文文件名 → 标题来自文件名，非法字节不进 index.txt
        const fs::path dir = base / "l";
        fs::create_directories(dir, ec);
        const std::vector<std::uint8_t> gbkMidi = makeTestMidiTitled(gbk);
        const fs::path src = dir / fs::path(std::u8string(u8"gbk曲.mid"));
        writeBytesTo(src, gbkMidi);
        SongLibrary lib;
        lib.load(dir);
        const SongLibrary::AddResult r = lib.add(src, std::nullopt);
        check(r.ok(), "l: GBK 轨名 MIDI 可入库");
        const SongLibrary::Entry* e = lib.find(r.id);
        check(e && e->title == "gbk曲", "l: 标题来自文件名 stem 而非 GBK 轨名");
        const std::string idx = readTextFile(dir / "index.txt");
        check(idx.find('\xC4') == std::string::npos && idx.find('\xBA') == std::string::npos,
              "l: GBK 字节未写入 index.txt");
    }

    // ---- g. 超限拒绝 --------------------------------------------------------
    {
        // > 1MB 文件
        const fs::path dir = base / "g1";
        fs::create_directories(dir, ec);
        std::vector<std::uint8_t> big(SongLibrary::kMaxFileBytes + 1, 0);
        std::memcpy(big.data(), "MThd", 4);
        const fs::path src = dir / "big.mid";
        writeBytesTo(src, big);
        SongLibrary lib;
        lib.load(dir);
        check(lib.add(src, std::nullopt).code == SongLibrary::AddResult::Code::TooLarge,
              "g: >1MB 文件被拒绝（读入内存前 file_size 拦截）");

        // 伪造 500 条索引 → add 拒绝
        const fs::path dir2 = base / "g2";
        fs::create_directories(dir2, ec);
        std::string fakeIndex = "PIANO-LIBRARY 1\n";
        for (int i = 0; i < SongLibrary::kMaxSongs; ++i) {
            char line[128];
            std::snprintf(line, sizeof(line),
                          "u-%016x|t%d|user-import|%016x|120|1.000|2|-1|1700000000|0\n",
                          i, i, i);
            fakeIndex += line;
        }
        writeTextTo(dir2 / "index.txt", fakeIndex);
        const fs::path src2 = dir2 / "s.mid";
        writeBytesTo(src2, midiBytes);
        SongLibrary lib2;
        lib2.load(dir2);
        check(lib2.entries().size() == static_cast<std::size_t>(SongLibrary::kMaxSongs),
              "g: 假索引载入 500 条");
        check(lib2.add(src2, std::nullopt).code == SongLibrary::AddResult::Code::TooManySongs,
              "g: 第 501 首被拒绝");

        // 总容量：把已入库 .mid resize 到 64MB（稀疏，瞬间完成），再 add 触发 LibraryFull
        const fs::path dir3 = base / "g3";
        fs::create_directories(dir3, ec);
        const fs::path src3 = dir3 / "s.mid";
        writeBytesTo(src3, midiBytes);
        SongLibrary lib3;
        lib3.load(dir3);
        const SongLibrary::AddResult r3 = lib3.add(src3, std::nullopt);
        check(r3.ok(), "g: 总容量用例先行入库");
        fs::resize_file(dir3 / (r3.id + ".mid"), SongLibrary::kMaxTotalBytes, ec);
        check(!ec, "g: resize_file 到 64MB");
        std::vector<std::uint8_t> alt = midiBytes;
        alt[alt.size() - 10] = 0x52;   // D4 力度 80 → 82
        writeBytesTo(dir3 / "s2.mid", alt);
        SongLibrary lib4;
        lib4.load(dir3);
        check(lib4.totalBytes() == SongLibrary::kMaxTotalBytes, "g: totalBytes 按文件实际大小统计");
        check(lib4.add(dir3 / "s2.mid", std::nullopt).code == SongLibrary::AddResult::Code::LibraryFull,
              "g: 总容量超限被拒绝");

        // 音符数上限是防御性常量：≤1MB 的合法 MIDI 装不下 200000 个音符（约 4.8B/音符），
        // add 路径必先命中 TooLarge；这里钉住常量取值本身
        check(SongLibrary::kMaxNotesPerSong == 200000, "g: 音符数上限常量 == 200000");
        check(SongLibrary::kMaxFileBytes == 1024 * 1024 && SongLibrary::kMaxSongs == 500 &&
              SongLibrary::kMaxTotalBytes == 64ull * 1024 * 1024, "g: 限制常量取值钉住");
    }

    // ---- h. FNV-1a 黄金值 ----------------------------------------------------
    {
        check(SongLibrary::fnv1a64("abc", 3) == 0xe71fa2190541574bull, "h: FNV-1a(\"abc\") 黄金值");
        check(SongLibrary::fnv1a64("a", 1) == 0xaf63dc4c8601ec8cull, "h: FNV-1a(\"a\") 黄金值");
        check(SongLibrary::fnv1a64("", 0) == 0xcbf29ce484222325ull, "h: FNV-1a(空) 黄金值");
    }

    // ---- i. MidiReader 回归：多轨黄金值 + trackStats/eventTracks/reassign ----
    {
        // 与 testMidi 相同的 Format 1 三轨样例
        auto makeTrack = [](std::vector<std::uint8_t> body) {
            const std::uint32_t n = static_cast<std::uint32_t>(body.size());
            std::vector<std::uint8_t> t{ 'M','T','r','k', static_cast<std::uint8_t>(n >> 24),
                                         static_cast<std::uint8_t>(n >> 16), static_cast<std::uint8_t>(n >> 8),
                                         static_cast<std::uint8_t>(n) };
            t.insert(t.end(), body.begin(), body.end()); return t;
        };
        const std::vector<std::uint8_t> f1a = makeTrack({ 0,0xFF,0x51,3,0x0F,0x42,0x40, 0,0xFF,0x2F,0 });
        const std::vector<std::uint8_t> f1b = makeTrack({ 0,0x90,60,80, 0x81,0x70,0x80,60,0, 0,0xFF,0x2F,0 });
        const std::vector<std::uint8_t> f1c = makeTrack({ 0,0x90,62,80, 0x81,0x70,0x80,62,0, 0,0x90,64,80,
                                                           0x81,0x70,0x80,64,0, 0,0xFF,0x2F,0 });
        std::vector<std::uint8_t> f1{ 'M','T','h','d',0,0,0,6,0,1,0,3,1,0xE0 };
        for (const auto& t : { f1a, f1b, f1c }) f1.insert(f1.end(), t.begin(), t.end());

        MidiParseResult r = parseMidi(f1.data(), f1.size());
        check(r.ok && r.song.events.size() == 3, "i: Format 1 解析 3 音符");
        if (r.song.events.size() == 3) {
            // 黄金值：排序键 (tick,track,note)。轨道 2 音符最多 → 旋律(0)，轨道 1 → 伴奏(1)
            const auto& e0 = r.song.events[0];
            const auto& e1 = r.song.events[1];
            const auto& e2 = r.song.events[2];
            check(e0.tick == 0 && e0.note == 62 && e0.duration == 240 && e0.velocity == 80 &&
                  e0.track == kTrackMelody && e0.channel == 0, "i: 事件[0] 黄金值 (tick0 note62 旋律)");
            check(e1.tick == 0 && e1.note == 60 && e1.duration == 240 &&
                  e1.track == kTrackAccomp, "i: 事件[1] 黄金值 (tick0 note60 伴奏)");
            check(e2.tick == 240 && e2.note == 64 && e2.duration == 240 &&
                  e2.track == kTrackMelody, "i: 事件[2] 黄金值 (tick240 note64 旋律)");
            check(r.eventTracks.size() == 3 && r.eventTracks[0] == 2 &&
                  r.eventTracks[1] == 1 && r.eventTracks[2] == 2,
                  "i: eventTracks 与排序后事件对齐 [2,1,2]");
        }
        check(r.trackStats.size() == 3, "i: trackStats 覆盖 3 轨");
        if (r.trackStats.size() == 3) {
            check(r.trackStats[0].noteCount == 0 && r.trackStats[0].lowest == -1,
                  "i: 轨道 0 无音符");
            check(r.trackStats[1].noteCount == 1 && r.trackStats[1].lowest == 60 &&
                  r.trackStats[1].highest == 60 && r.trackStats[1].channelMask == 1,
                  "i: 轨道 1 统计正确");
            check(r.trackStats[2].noteCount == 2 && r.trackStats[2].lowest == 62 &&
                  r.trackStats[2].highest == 64 && r.trackStats[2].channelMask == 1,
                  "i: 轨道 2 统计正确");
        }
        // reassignMelodyTrack：改选轨道 1 为旋律，事件顺序不变、仅标签变化
        reassignMelodyTrack(r, 1);
        if (r.song.events.size() == 3) {
            check(r.song.events[0].track == kTrackAccomp && r.song.events[1].track == kTrackMelody &&
                  r.song.events[2].track == kTrackAccomp, "i: reassign 后 0/1 标注正确");
            check(r.song.events[0].note == 62 && r.song.events[1].note == 60 &&
                  r.song.events[2].note == 64, "i: reassign 不改变事件顺序");
        }
        // Format 0 样例的 eventTracks / trackStats
        check(midiParsed.eventTracks.size() == 2 && midiParsed.eventTracks[0] == 0 &&
              midiParsed.eventTracks[1] == 0, "i: Format 0 eventTracks 全为轨 0");
        check(midiParsed.trackStats.size() == 1 && midiParsed.trackStats[0].noteCount == 2 &&
              midiParsed.trackStats[0].lowest == 60 && midiParsed.trackStats[0].highest == 62,
              "i: Format 0 trackStats 正确");
    }

    // ---- j. 畸形输入：不崩溃、明显非法的返回 ok=false ------------------------
    {
        bool truncationsOk = true;
        for (std::size_t n = 0; n <= midiBytes.size(); ++n) {
            const MidiParseResult r = parseMidi(midiBytes.data(), n);
            if (n < 14 && r.ok) truncationsOk = false;   // 头都读不全必须失败
        }
        check(truncationsOk, "j: 逐字节截断 0..全长 无崩溃、头截断必败");

        // 头部声明超长（headerLen=100 但文件只有 14 字节）→ 无音符 → ok=false
        std::vector<std::uint8_t> bad1{ 'M','T','h','d',0,0,0,100, 0,0,0,1,1,0xE0 };
        check(!parseMidi(bad1.data(), bad1.size()).ok, "j: MThd 长度越界拒绝");

        // MTrk 声明 0x7FFFFFFF 但实体极短 → 截断处理，不崩溃
        std::vector<std::uint8_t> bad2{ 'M','T','h','d',0,0,0,6,0,0,0,1,1,0xE0,
                                        'M','T','r','k',0x7F,0xFF,0xFF,0xFF, 0x00,0xFF,0x2F,0x00 };
        parseMidi(bad2.data(), bad2.size());
        check(true, "j: MTrk 长度越界不崩溃");

        // VLQ 连续位溢出（5 个 continuation 字节）→ 4 字节上限截断，不崩溃
        std::vector<std::uint8_t> track{ 0xFF,0xFF,0xFF,0xFF,0xFF, 0x90,60,80,
                                         0x81,0x70,0x80,60,0, 0,0xFF,0x2F,0 };
        std::vector<std::uint8_t> bad3{ 'M','T','h','d',0,0,0,6,0,0,0,1,1,0xE0, 'M','T','r','k' };
        const std::uint32_t tl = static_cast<std::uint32_t>(track.size());
        bad3.insert(bad3.end(), { static_cast<std::uint8_t>(tl >> 24), static_cast<std::uint8_t>(tl >> 16),
                                  static_cast<std::uint8_t>(tl >> 8), static_cast<std::uint8_t>(tl) });
        bad3.insert(bad3.end(), track.begin(), track.end());
        parseMidi(bad3.data(), bad3.size());
        check(true, "j: VLQ 溢出不崩溃");

        // 伪随机字节流 1KB
        std::vector<std::uint8_t> junk(1024);
        std::uint32_t seed = 12345;
        for (auto& b : junk) { seed = seed * 1664525u + 1013904223u; b = static_cast<std::uint8_t>(seed >> 24); }
        parseMidi(junk.data(), junk.size());
        check(true, "j: 随机字节流不崩溃");
    }

    // ---- l. UserData：路径形状 + legacy 迁移（临时 APPDATA/HOME + 临时 cwd） ----
    {
        check(userDataPath("x") == userDataDirectory() / "x", "l: userDataPath 拼接正确");

        // 备份并替换环境变量（仅本进程）。Windows 下 userDataDirectory 读 APPDATA，
        // POSIX 读 HOME；路径含中文，Windows 必须走宽字符 _wputenv_s。
        // 两个子场景各自独立目录，互不依赖执行顺序，也不会碰到 runner 的真实 HOME。
    #ifdef _WIN32
        const wchar_t* oldAppData = _wgetenv(L"APPDATA");
        const std::wstring oldEnvW = oldAppData ? oldAppData : L"";
        auto setEnv = [](const fs::path& p) { _wputenv_s(L"APPDATA", p.wstring().c_str()); };
        auto restoreEnv = [&]() { _wputenv_s(L"APPDATA", oldEnvW.c_str()); };
    #else
        const char* oldHome = std::getenv("HOME");
        const std::string oldEnv = oldHome ? oldHome : "";
        const char* oldXdg = std::getenv("XDG_DATA_HOME");
        const std::string oldXdgEnv = oldXdg ? oldXdg : "";
        const bool hadXdg = oldXdg != nullptr;
        auto setEnv = [](const fs::path& p) { setenv("HOME", p.string().c_str(), 1); };
        auto restoreEnv = [&]() {
            setenv("HOME", oldEnv.c_str(), 1);
            if (hadXdg) setenv("XDG_DATA_HOME", oldXdgEnv.c_str(), 1);
            else unsetenv("XDG_DATA_HOME");
        };
        // The test deliberately controls HOME so it never touches the runner's
        // real profile. XDG_DATA_HOME must be cleared for the same reason.
        unsetenv("XDG_DATA_HOME");
    #endif
        const fs::path oldCwd = fs::current_path(ec);

        // l1：cwd 有 favorites.txt 而用户数据目录没有 → 复制。
        //     macOS 下这里真实走 ~/Library/Application Support 分支（临时 HOME 隔离）。
        {
            const fs::path fakeHome = base / "l1" / "home";
            const fs::path fakeCwd  = base / "l1" / "cwd";
            fs::create_directories(fakeHome, ec);
            fs::create_directories(fakeCwd, ec);
            writeTextTo(fakeCwd / "favorites.txt", "song-1\n");
            setEnv(fakeHome);
            fs::current_path(fakeCwd, ec);
            migrateLegacyUserData();
            const fs::path target = userDataPath("favorites.txt");
            check(readTextFile(target) == "song-1\n", "l: cwd 旧配置复制到用户数据目录");
            // 确认迁移目标位于临时 HOME 之下（隔离生效；各平台布局不同，
            // 不断言具体子路径 —— Windows=%APPDATA%/…，macOS=Library/Application Support/…，
            // Linux=.local/share/…）
            const fs::path tn = target.lexically_normal();
            const fs::path hn = fakeHome.lexically_normal();
            check(std::equal(hn.begin(), hn.end(), tn.begin()), "l: 迁移目标位于临时 HOME 下");
            check(fs::exists(fakeCwd / "favorites.txt", ec), "l: 旧文件保留不删除");
        }

        // l2：目标已存在 → 不覆盖（独立目录，与 l1 无关）
        {
            const fs::path fakeHome = base / "l2" / "home";
            const fs::path fakeCwd  = base / "l2" / "cwd";
            fs::create_directories(fakeCwd, ec);
            writeTextTo(fakeCwd / "favorites.txt", "song-2\n");
            setEnv(fakeHome);
            const fs::path target = userDataPath("favorites.txt");
            fs::create_directories(target.parent_path(), ec);
            writeTextTo(target, "existing\n");
            fs::current_path(fakeCwd, ec);
            migrateLegacyUserData();
            check(readTextFile(target) == "existing\n", "l: 目标已存在则不覆盖（复制而非移动）");
        }

    #ifdef __APPLE__
        // l3：macOS 旧位置 ~/.local/share 存在且新位置不存在 → 一次性复制迁移
        {
            const fs::path fakeHome = base / "l3" / "home";
            const fs::path legacyDir = fakeHome / ".local" / "share" / "GrandStudioPiano";
            fs::create_directories(legacyDir, ec);
            writeTextTo(legacyDir / "favorites.txt", "legacy\n");
            setEnv(fakeHome);
            const fs::path dir = userDataDirectory();
            check(dir == fakeHome / "Library" / "Application Support" / "GrandStudioPiano",
                  "l: macOS 迁移后使用新位置");
            check(readTextFile(dir / "favorites.txt") == "legacy\n",
                  "l: macOS legacy 内容被复制迁移");
            check(fs::exists(legacyDir / "favorites.txt", ec), "l: macOS legacy 旧文件保留不删除");
        }
    #endif

        fs::current_path(oldCwd, ec);
        restoreEnv();
    }

    // ---- m. 收藏裁决：favorites.txt ∪ 索引 favorite=1，合并后写回两处 ----
    {
        const fs::path dir = base / "m";
        fs::create_directories(dir, ec);
        // 曲库两首：A（fav=1）、B（fav=0）
        const fs::path srcA = dir / "a.mid";
        writeBytesTo(srcA, midiBytes);
        std::vector<std::uint8_t> alt = midiBytes;
        alt[alt.size() - 10] = 0x51;   // D4 力度 80 → 81
        const fs::path srcB = dir / "b.mid";
        writeBytesTo(srcB, alt);
        SongLibrary lib;
        lib.load(dir);
        const std::string idA2 = lib.add(srcA, std::nullopt).id;
        const std::string idB2 = lib.add(srcB, std::nullopt).id;
        check(!idA2.empty() && !idB2.empty() && idA2 != idB2, "m: 两首入库");
        check(lib.setFavorite(idA2, true), "m: A 标记收藏");

        const fs::path favFile = dir / "favorites.txt";
        // favorites 文件的读写格式与 App::loadFavorites/saveFavorites 一致：每行一个 id
        auto readFavFile = [&]() {
            std::vector<std::string> ids;
            std::ifstream in(favFile);
            std::string line;
            while (std::getline(in, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
                    line.pop_back();
                if (!line.empty()) ids.push_back(line);
            }
            return ids;
        };
        auto writeFavFile = [&](const std::vector<std::string>& ids) {
            std::string text;
            for (const auto& id : ids) text += id + "\n";
            return writeTextTo(favFile, text);
        };
        // 与 App::init 完全相同的合并 + 写回逻辑
        auto mergeAndWriteBack = [&](SongLibrary& l, std::vector<std::string>& favs) {
            const std::vector<std::string> merged = mergeFavoriteIds(l.entries(), favs);
            for (const auto& id : merged) {
                const SongLibrary::Entry* e = l.find(id);
                if (e && !e->favorite) l.setFavorite(id, true);
            }
            if (merged != favs) { favs = merged; writeFavFile(favs); }
        };

        // 文件 {B}、索引 A fav=1 → 并集含 A 与 B，fileIds 顺序优先
        writeFavFile({ idB2 });
        std::vector<std::string> favs = readFavFile();
        const std::vector<std::string> merged = mergeFavoriteIds(lib.entries(), favs);
        check(merged.size() == 2 && merged[0] == idB2 && merged[1] == idA2,
              "m: 并集含 A 与 B，顺序 fileIds 优先");

        // 写回后重新 load 曲库 + 重读文件 → 两处一致
        mergeAndWriteBack(lib, favs);
        SongLibrary lib2;
        lib2.load(dir);
        const std::vector<std::string> file2 = readFavFile();
        const SongLibrary::Entry* ea = lib2.find(idA2);
        const SongLibrary::Entry* eb = lib2.find(idB2);
        check(ea && ea->favorite && eb && eb->favorite, "m: 写回后索引 A、B 均 fav");
        check(file2.size() == 2 && std::find(file2.begin(), file2.end(), idA2) != file2.end() &&
              std::find(file2.begin(), file2.end(), idB2) != file2.end(), "m: 写回后文件含 A、B");
        // 再次合并 = 幂等（两处已一致时不产生差异）
        check(mergeFavoriteIds(lib2.entries(), file2) == file2, "m: 合并幂等");

        // 反向：文件为空、索引 fav=1 → 合并后文件被写回该 id
        writeFavFile({});
        SongLibrary lib3;
        lib3.load(dir);
        std::vector<std::string> favs3 = readFavFile();
        mergeAndWriteBack(lib3, favs3);
        const std::vector<std::string> file3 = readFavFile();
        check(file3.size() == 2 && std::find(file3.begin(), file3.end(), idA2) != file3.end() &&
              std::find(file3.begin(), file3.end(), idB2) != file3.end(),
              "m: 空文件 + 索引 fav → 文件被写回索引收藏");

        // 空对空 → 空
        SongLibrary lib4;
        const fs::path dir2 = base / "m2";
        fs::create_directories(dir2, ec);
        lib4.load(dir2);
        check(mergeFavoriteIds(lib4.entries(), {}).empty(), "m: 空对空 → 空");
    }

    fs::remove_all(base, ec);
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
    testFxSettings();
    testMixer();
    testPlayer();
    testJudge();
    testSongs();
    testNoteFx();
    testSongLibrary();

    std::printf("\n----------------------------------------\n");
    std::printf("通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    std::printf("----------------------------------------\n");
    return g_fail == 0 ? 0 : 1;
}

} // namespace piano
