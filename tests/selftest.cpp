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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "audio/Mixer.h"
#include "core/Note.h"
#include "core/SpscQueue.h"
#include "core/VisualEvent.h"
#include "fx/FxSettings.h"
#include "fx/NoteParticles.h"
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
        check(na == nb && na > 0 &&
              std::memcmp(a, b, sizeof(fx::RenderItem) * static_cast<std::size_t>(na)) == 0,
              "同种子同 dt 序列：collect 逐位一致");
        check(nc != na ||
              std::memcmp(a, c, sizeof(fx::RenderItem) * static_cast<std::size_t>(na)) != 0,
              "换种子后至少一个字段不同");
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

    // 布局兼容性：fxEnabled=false 时输出与引入舞台带之前逐像素一致
    {
        UiLayout off;
        off.fxEnabled = false;
        off.compute(1520.0f, 1000.0f);
        check(off.fxStage.height == 0.0f && off.fxStage.width == 0.0f,
              "fx 关闭：fxStage 为空矩形");
        check(off.keys.y == off.rail.y + 30.0f && std::fabs(off.keyH - 300.0f) < 1e-4f,
              "fx 关闭：琴键位置/高度与旧布局一致");

        UiLayout on;
        on.fxEnabled = true;
        on.compute(1520.0f, 1000.0f);
        check(std::fabs(on.fxStage.height - tk::kStageMaxHeight) < 1e-4f &&
              on.fxStage.y == on.rail.y + 30.0f,
              "fx 开启：舞台带位于键盘上方、高度取上限 160");
        check(std::fabs(on.keys.y - (on.fxStage.y + on.fxStage.height + 8.0f)) < 1e-4f,
              "fx 开启：琴键下移到舞台带之下");
        // 关闭态其余几何钉住绝对数值（与引入舞台带之前的公式逐项核对）
        check(std::fabs(off.controls.y - 156.0f) < 1e-3f && std::fabs(off.rail.y - 228.0f) < 1e-3f &&
              std::fabs(off.keys.y - 258.0f) < 1e-3f && std::fabs(off.transport.y - 600.0f) < 1e-3f &&
              std::fabs(off.song.y - 662.0f) < 1e-3f,
              "fx 关闭：控制行/琴盖/琴键/运输栏/曲目面板位置与旧版逐项一致");
    }
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

    std::printf("\n----------------------------------------\n");
    std::printf("通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    std::printf("----------------------------------------\n");
    return g_fail == 0 ? 0 : 1;
}

} // namespace piano
