#include "audio/Mixer.h"

#include <algorithm>
#include <cmath>

#include "core/Note.h"

namespace piano {
namespace {

/// 采样音色允许的最大变调距离（半音）。超过这个范围，变调带来的共振峰偏移会明显失真。
constexpr int kSampleMaxDistance = 12;

/// 硬复用时的去咔哒淡出长度（样本数）。64 样本 @48kHz ≈ 1.3ms，足够消除咔哒。
constexpr int kDeclickFrames = 64;

} // namespace

// ---------------------------------------------------------------------------
// 内部小组件
// ---------------------------------------------------------------------------

void Mixer::DeclickTail::start(float sl, float sr) {
    l = sl;
    r = sr;
    remain = kDeclickFrames;
    stepL = -sl / static_cast<float>(kDeclickFrames);
    stepR = -sr / static_cast<float>(kDeclickFrames);
    active = true;
}

void Mixer::DeclickTail::renderAdd(float* out, std::uint32_t frames) {
    if (!active) return;
    const std::uint32_t n = std::min<std::uint32_t>(frames, static_cast<std::uint32_t>(remain));
    for (std::uint32_t i = 0; i < n; ++i) {
        l += stepL;
        r += stepR;
        out[i * 2]     += l;
        out[i * 2 + 1] += r;
    }
    remain -= static_cast<int>(n);
    if (remain <= 0) active = false;
}

void Mixer::ClickVoice::renderAdd(float* out, std::uint32_t frames, float sampleRate) {
    if (!active) return;
    (void)sampleRate;
    for (std::uint32_t i = 0; i < frames; ++i) {
        const float s = std::sin(2.0f * kPi * static_cast<float>(phase)) * env * amp;
        out[i * 2]     += s * 0.7f;
        out[i * 2 + 1] += s * 0.7f;
        phase += inc;
        if (phase >= 1.0) phase -= 1.0;
        inc += (incEnd - inc) * incCoef;
        env *= coef;
        if (env < 1e-4f) { active = false; return; }
    }
}

void Mixer::Compressor::prepare(float sr) {
    const float atk = 0.004f, rel = 0.19f;
    atkCoef = 1.0f - std::exp(-1.0f / (atk * sr));
    relCoef = 1.0f - std::exp(-1.0f / (rel * sr));
    env = 0.0f;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

void Mixer::prepare(double sampleRate) {
    sampleRate_ = sampleRate > 1000.0 ? sampleRate : 48000.0;
    wavetables_.build(sampleRate_);
    reverb_.prepare(sampleRate_);
    reverb_.setDamping(0.22f);
    reverb_.setMix(reverb_.mix());
    comp_.prepare(static_cast<float>(sampleRate_));
    for (auto& c : visual_) c.store(0, std::memory_order_relaxed);
    reset();
}

void Mixer::reset() {
    for (auto& v : voices_) v.hardReset();
    for (auto& t : tails_) t.active = false;
    for (auto& c : clicks_) c.active = false;
    reverb_.reset();
    comp_.reset();
    frameCounter_.store(0, std::memory_order_relaxed);
    activeVoices_.store(0, std::memory_order_relaxed);
    peak_.store(0.0f, std::memory_order_relaxed);
    sustainPrev_ = sustainOn_.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// 声部分配与偷声
// ---------------------------------------------------------------------------

Voice* Mixer::allocateSlot() {
    for (auto& v : voices_)
        if (!v.active) return &v;
    return nullptr;
}

Voice* Mixer::pickVictim() {
    // 偷声优先级（数值越小越先被偷）：
    //   0  已在释放阶段，且不是用户实时弹奏的
    //   1  已在释放阶段的用户音
    //   2  被延音踏板挂住的音
    //   3  自动播放/回放中仍按住的音
    //   4  用户仍按住的音（最不该偷）
    auto tierOf = [](const Voice& v) -> int {
        if (v.released) return (v.source == Source::User) ? 1 : 0;
        if (v.deferred) return 2;
        if (v.source == Source::User) return 4;
        return 3;
    };

    Voice* best = nullptr;
    int   bestTier = 99;
    float bestLevel = 1e9f;

    for (auto& v : voices_) {
        if (!v.active) continue;
        const int   tier  = tierOf(v);
        const float level = v.env.level;
        if (best == nullptr || tier < bestTier || (tier == bestTier && level < bestLevel)) {
            best = &v;
            bestTier = tier;
            bestLevel = level;
        }
    }
    return best;
}

void Mixer::stealOne() {
    Voice* victim = pickVictim();
    if (!victim) return;

    // 关键：必须真正「释放槽位」，而不是把被偷的声部留在快速释放状态。
    // 否则 pickVictim 下一次还会挑中它（已释放的声部优先级最高），
    // 复音上限就会被架空 —— 实测会累积到 99 个活跃声部。
    //
    // 去咔哒由 64 样本的淡出补丁负责：把被偷声部最后输出的样本交给补丁，
    // 线性降到 0，同时新声部开始起音，两者叠加听不出接缝。
    for (auto& t : tails_) {
        if (!t.active) { t.start(victim->lastL, victim->lastR); break; }
    }
    victim->hardReset();
}

int Mixer::liveVoiceCount() const {
    int n = 0;
    for (const auto& v : voices_) if (v.active) ++n;
    return n;
}

int Mixer::heldVoiceCount() const {
    int n = 0;
    for (const auto& v : voices_) if (v.active && !v.released) ++n;
    return n;
}

Voice* Mixer::startVoice(int midi, float vel, Source source) {
    Voice* v = allocateSlot();
    if (!v) {
        Voice* victim = pickVictim();
        if (!victim) return nullptr;
        // 所有 96 个槽位都在使用：硬复用最低音量的那个，
        // 并把它的残余输出交给一个 64 样本的淡出补丁，消除切换咔哒。
        for (auto& t : tails_) {
            if (!t.active) { t.start(victim->lastL, victim->lastR); break; }
        }
        victim->hardReset();
        v = victim;
        hardReuse_.fetch_add(1, std::memory_order_relaxed);
    }

    const std::uint64_t frame = frameCounter_.load(std::memory_order_relaxed);
    const bool wantSample = (engineMode_.load(std::memory_order_relaxed) != EngineMode::SynthOnly) && bank_ && !bank_->empty();
    const SampleBuffer* buf = wantSample ? bank_->nearest(midi, kSampleMaxDistance) : nullptr;

    if (buf) v->startSample(buf, sampleRate_, midi, vel, source, frame);
    else     v->startSynth(wavetables_, sampleRate_, midi, vel, source, frame);
    return v;
}

// ---------------------------------------------------------------------------
// 音符事件
// ---------------------------------------------------------------------------

Voice* Mixer::noteOn(int midi, float velocity, Source source) {
    if (midi < 0 || midi > 127) return nullptr;
    velocity = clampf(velocity, 0.05f, 1.0f);

    // 复音上限只看「仍在按住 / 被踏板挂住」的声部；已经在释放尾音里的不计入，
    // 这样尾音不会挤占复音预算。
    if (heldVoiceCount() >= kPolyphonyLimit) stealOne();

    Voice* v = startVoice(midi, velocity, source);
    visual_[static_cast<std::size_t>(midi)].fetch_add(1, std::memory_order_relaxed);

    // 纯视觉副作用：声部真正启动后才通知 UI 飘音符；节拍器不飘。
    // 队列满时直接丢弃，不统计不重试，绝不影响发声。
    if (v && visualQueue_ && source != Source::Metronome) {
        VisualEvent ev;
        ev.audioFrame = frameCounter_.load(std::memory_order_relaxed);
        ev.velocity   = velocity;
        ev.midi       = static_cast<std::uint8_t>(midi);
        ev.source     = source;
        visualQueue_->push(ev);
    }
    return v;
}

void Mixer::noteOff(int midi, Source source) {
    if (midi < 0 || midi > 127) return;

    std::atomic<int>& counter = visual_[static_cast<std::size_t>(midi)];
    if (counter.load(std::memory_order_relaxed) > 0)
        counter.fetch_sub(1, std::memory_order_relaxed);

    const bool sustain = sustainOn_.load(std::memory_order_relaxed);

    // 选最早开始、且尚未进入释放的匹配声部
    Voice* target = nullptr;
    for (auto& v : voices_) {
        if (!v.active || v.released || v.deferred) continue;
        if (v.note != midi || v.source != source) continue;
        if (!target || v.startFrame < target->startFrame) target = &v;
    }
    if (!target) return;

    target->held = false;
    if (sustain) {
        target->deferred = true;   // 踏板仍踩下：挂住，等踏板抬起
    } else {
        target->deferred = false;
        target->released = true;
        target->env.noteOff();
    }
}

void Mixer::flushDeferred() {
    for (auto& v : voices_) {
        if (!v.active || !v.deferred) continue;
        v.deferred = false;
        v.released = true;
        v.env.noteOff();
    }
}

void Mixer::allNotesOff(bool hard) {
    for (auto& v : voices_) {
        if (!v.active) continue;
        if (hard) {
            v.hardReset();
        } else {
            v.held = false;
            v.deferred = false;
            v.released = true;
            v.env.forceRelease(static_cast<float>(sampleRate_), 0.08f);
        }
    }
    for (auto& c : visual_) c.store(0, std::memory_order_relaxed);
}

void Mixer::releaseSource(Source source, bool hard) {
    for (auto& v : voices_) {
        if (!v.active || v.source != source) continue;
        if (hard) {
            v.hardReset();
        } else {
            v.held = false;
            v.deferred = false;
            v.released = true;
            v.env.forceRelease(static_cast<float>(sampleRate_), 0.08f);
        }
    }
}

void Mixer::triggerClick(float freq, float amp, float decaySeconds) {
    ClickVoice& c = clicks_[static_cast<std::size_t>(clickCursor_)];
    clickCursor_ = (clickCursor_ + 1) % kClicks;
    c.active  = true;
    c.phase   = 0.0;
    c.inc     = static_cast<double>(freq) / sampleRate_;
    c.incEnd  = static_cast<double>(freq * 0.62f) / sampleRate_;
    c.incCoef = 0.0025;
    c.amp     = amp;
    c.env     = 1.0f;
    c.coef    = std::pow(0.0001f, 1.0f / (decaySeconds * static_cast<float>(sampleRate_)));
}

// ---------------------------------------------------------------------------
// 渲染
// ---------------------------------------------------------------------------

void Mixer::render(float* out, std::uint32_t frames) {
    if (frames == 0) return;
    std::fill(out, out + static_cast<std::size_t>(frames) * 2, 0.0f);

    // 延音踏板的下降沿：释放所有被挂住的声部
    const bool sus = sustainOn_.load(std::memory_order_relaxed);
    if (sustainPrev_ && !sus) flushDeferred();
    sustainPrev_ = sus;

    const float srF = static_cast<float>(sampleRate_);

    for (auto& v : voices_) if (v.active) v.renderAdd(out, frames, srF);
    for (auto& t : tails_)  if (t.active) t.renderAdd(out, frames);
    for (auto& c : clicks_) if (c.active) c.renderAdd(out, frames, srF);

    float peak = 0.0f;
    for (std::uint32_t i = 0; i < frames; ++i) {
        float l = out[i * 2];
        float r = out[i * 2 + 1];

        reverb_.process(l, r);
    const float master = masterVol_.load(std::memory_order_relaxed);
    l *= master * 0.72f;
    r *= master * 0.72f;
        l = comp_.process(l);
        r = comp_.process(r);
        l = softClip(l);
        r = softClip(r);

        out[i * 2]     = l;
        out[i * 2 + 1] = r;

        const float al = std::fabs(l);
        const float ar = std::fabs(r);
        if (al > peak) peak = al;
        if (ar > peak) peak = ar;
    }
    peak_.store(peak, std::memory_order_relaxed);
    frameCounter_.fetch_add(frames, std::memory_order_relaxed);
    activeVoices_.store(liveVoiceCount(), std::memory_order_relaxed);
}

} // namespace piano
