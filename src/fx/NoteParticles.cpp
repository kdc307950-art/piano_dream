#include "fx/NoteParticles.h"

#include <cmath>

#include "core/Note.h"

namespace piano {
namespace fx {

namespace tk = theme::fx;

namespace {
constexpr double kDistantPast = -1.0e9;   // 冷却/负载窗口的「从未发生」初值
}

NoteFx::NoteFx() {
    lastSpawn_.fill(kDistantPast);
    recentSpawns_.fill(kDistantPast);
}

// ---------------------------------------------------------------------------
// 参数
// ---------------------------------------------------------------------------

void NoteFx::setLevel(Level level) { level_ = level; }
void NoteFx::setReducedMotion(bool on) { reducedMotion_ = on; }
void NoteFx::setStageHeight(float px) { stageHeight_ = px > 0.0f ? px : 0.0f; }
void NoteFx::seedRng(std::uint32_t seed) { rng_.seed(seed); }
void NoteFx::setAliveCapScale(float s) { aliveCapScale_ = clampf(s, 0.0f, 1.0f); }

void NoteFx::clear() {
    for (auto& p : pool_) p.active = false;
    aliveCount_ = 0;
    pendingHead_ = 0;
    pendingCount_ = 0;
    spawnedThisFrame_ = 0;
    lastSpawn_.fill(kDistantPast);
    recentSpawns_.fill(kDistantPast);
}

int NoteFx::levelCap() const {
    switch (level_) {
    case Level::Low:      return tk::kAliveCapLow;
    case Level::Standard: return tk::kAliveCapStandard;
    case Level::High:     return tk::kAliveCapHigh;
    case Level::Off:      break;
    }
    return 0;
}

int NoteFx::effectiveAliveCap() const {
    const int cap = levelCap();
    const int scaled = static_cast<int>(std::lround(static_cast<double>(cap) * aliveCapScale_));
    return cap < scaled ? cap : scaled;
}

// ---------------------------------------------------------------------------
// 纯函数
// ---------------------------------------------------------------------------

float NoteFx::lerp(float a, float b, float t) { return a + (b - a) * t; }

float NoteFx::backOut(float t) {
    const float c = tk::kBackEaseC;
    const float u = t - 1.0f;
    return 1.0f + (c + 1.0f) * u * u * u + c * u * u;
}

float NoteFx::smoothstep(float e0, float e1, float x) {
    const float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float NoteFx::pitchNorm(int midi) {
    const int m = clampNote(midi);
    return static_cast<float>(m - kMidiLow) / static_cast<float>(kMidiHigh - kMidiLow);
}

float NoteFx::baseScaleFor(float velocity, int midi) {
    const float pitchMul = lerp(tk::kSizeLowPitchMul, tk::kSizeHighPitchMul, pitchNorm(midi));
    return pitchMul * (tk::kSizeVelocityBase + tk::kSizeVelocityRange * clampf(velocity, 0.0f, 1.0f));
}

float NoteFx::baseLifetimeFor(int midi) {
    return tk::kLifetimeBase * lerp(tk::kLifetimeLowPitchMul, tk::kLifetimeHighPitchMul, pitchNorm(midi));
}

float NoteFx::baseAlphaFor(float velocity) {
    return tk::kAlphaBase + tk::kAlphaVelocityRange * clampf(velocity, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// 生成与限流（deterministic 顺序：冷却 → 每帧上限 → 存活上限 → 负载丢弃 → 生成）
// ---------------------------------------------------------------------------

bool NoteFx::trySpawn(std::uint8_t midi, float velocity, std::uint8_t source,
                      double birth, float scaleMul, bool honorCooldown) {
    // ① 同 midi 冷却（延迟的第二音符属于同一次按键，跳过此检查）
    if (honorCooldown && birth - lastSpawn_[midi] < tk::kCooldownSeconds) return false;
    // ② 本帧生成上限
    if (spawnedThisFrame_ >= tk::kPerFrameSpawnMax) return false;
    // ③ 存活上限（含降级缩放）
    if (aliveCount_ >= effectiveAliveCap()) return false;

    // ④ 负载降低：最近窗口内生成数超标 → 一次 RNG draw 决定丢弃
    int recent = 0;
    for (double t : recentSpawns_)
        if (t > now_ - tk::kChordWindowSeconds) ++recent;
    if (recent >= tk::kChordLoadCount) {
        const float keep = rng_.next() * 0.5f + 0.5f;   // 均匀 [0,1)
        if (keep >= tk::kChordProbability) return false;
        scaleMul *= tk::kChordSizeMul;
    }

    Particle* slot = nullptr;
    for (auto& p : pool_)
        if (!p.active) { slot = &p; break; }
    if (!slot) return false;

    // ⑤ 每个粒子只抽一次 32-bit 种子，形状/相位/旋转方向全部由该种子位混合推出
    rng_.next();
    const std::uint32_t s = rng_.s;

    slot->active    = true;
    slot->midi      = midi;
    slot->source    = source;
    slot->velocity  = velocity;
    slot->shape     = reducedMotion_ ? std::uint8_t{3} : static_cast<std::uint8_t>(s % 3u);
    slot->phase     = static_cast<float>((s >> 3) & 0xFFFFu) * (2.0f * kPi / 65536.0f);
    slot->swayAmp   = (level_ == Level::Low) ? 0.0f : tk::kSwayAmplitudePx;   // 「少」档无摇摆
    slot->rotDir    = ((s >> 19) & 1u) ? 1.0f : -1.0f;
    slot->baseScale = reducedMotion_ ? tk::kReducedScale : baseScaleFor(velocity, midi);
    slot->scaleMul  = scaleMul;
    slot->lifetime  = reducedMotion_ ? tk::kReducedLifetimeSeconds : baseLifetimeFor(midi);
    slot->riseDist  = stageHeight_ * tk::kRiseDistanceStageMul;
    slot->birth     = birth;

    lastSpawn_[midi] = birth;
    recentSpawns_[static_cast<std::size_t>(recentCursor_)] = now_;
    recentCursor_ = (recentCursor_ + 1) % static_cast<int>(recentSpawns_.size());
    ++spawnedThisFrame_;
    ++aliveCount_;
    return true;
}

void NoteFx::ingest(std::uint8_t midi, float velocity, std::uint8_t source, double eventTime) {
    if (level_ == Level::Off) return;
    if (eventTime > now_) eventTime = now_;                 // 未来事件按当前时刻处理
    if (eventTime < now_ - tk::kStaleEventSeconds) return;  // 过期丢弃

    if (trySpawn(midi, velocity, source, eventTime, 1.0f, true) &&
        velocity >= tk::kTwoNoteVelocity) {
        // 追加第二个音符：固定容量延迟队列，满则丢
        if (pendingCount_ < tk::kPendingCapacity) {
            PendingNote& p = pending_[static_cast<std::size_t>(
                (pendingHead_ + pendingCount_) % tk::kPendingCapacity)];
            p.midi     = midi;
            p.source   = source;
            p.velocity = velocity;
            p.scaleMul = tk::kSecondScaleMul;
            p.due      = eventTime + tk::kSecondDelaySeconds;
            ++pendingCount_;
        }
    }
}

void NoteFx::update(double dt) {
    if (dt < 0.0) dt = 0.0;
    if (dt > tk::kDtMaxSeconds) dt = tk::kDtMaxSeconds;
    now_ += dt;
    spawnedThisFrame_ = 0;

    // 延迟队列：到期的第二音符（保持相对顺序，未到期的压缩回队首）
    std::array<PendingNote, tk::kPendingCapacity> keep{};
    int kept = 0;
    for (int i = 0; i < pendingCount_; ++i) {
        const PendingNote& p = pending_[static_cast<std::size_t>(
            (pendingHead_ + i) % tk::kPendingCapacity)];
        if (p.due <= now_) {
            trySpawn(p.midi, p.velocity, p.source, p.due, p.scaleMul, false);
        } else {
            keep[static_cast<std::size_t>(kept++)] = p;
        }
    }
    for (int i = 0; i < kept; ++i) pending_[static_cast<std::size_t>(i)] = keep[static_cast<std::size_t>(i)];
    pendingHead_ = 0;
    pendingCount_ = kept;

    // 回收寿命结束的粒子
    for (auto& p : pool_) {
        if (p.active && now_ - p.birth >= static_cast<double>(p.lifetime)) {
            p.active = false;
            --aliveCount_;
        }
    }
}

// ---------------------------------------------------------------------------
// 收集渲染项（全部解析式，仅依赖 u = age/lifetime）
// ---------------------------------------------------------------------------

int NoteFx::collect(RenderItem* out, int maxOut) const {
    int n = 0;
    for (const Particle& p : pool_) {
        if (!p.active || n >= maxOut) continue;
        const double ageD = now_ - p.birth;
        if (ageD < 0.0) continue;
        const float u = clampf(static_cast<float>(ageD / static_cast<double>(p.lifetime)), 0.0f, 1.0f);

        RenderItem item{};
        item.midi   = p.midi;
        item.shape  = p.shape;
        item.source = p.source;

        if (reducedMotion_) {
            // 柔光点：上升/摇摆/旋转整体旁路
            item.offsetX  = 0.0f;
            item.offsetY  = 0.0f;
            item.rotation = 0.0f;
            item.scale    = tk::kReducedScale;
            const float fadeIn = clampf(static_cast<float>(ageD) / tk::kReducedFadeInSeconds, 0.0f, 1.0f);
            item.alpha = baseAlphaFor(p.velocity) * fadeIn *
                         (1.0f - smoothstep(tk::kFadeOutStartU, tk::kFadeZeroU, u));
        } else {
            float anim;
            if (u < tk::kGrowEndU) {
                anim = lerp(tk::kScaleStart, tk::kScaleMid, backOut(u / tk::kGrowEndU));
            } else {
                anim = lerp(tk::kScaleMid, tk::kScaleEnd,
                            (u - tk::kGrowEndU) / (1.0f - tk::kGrowEndU));
            }
            item.scale = anim * p.baseScale * p.scaleMul;

            const float rise = 1.0f - (1.0f - u) * (1.0f - u);   // 减速上升
            item.offsetY = p.riseDist * rise;
            item.offsetX = p.swayAmp * std::sin(2.0f * kPi * tk::kSwayCycles * u + p.phase);
            item.rotation = p.rotDir * tk::kRotationMaxRad * u;

            const float fadeIn = clampf(static_cast<float>(ageD) / tk::kFadeInSeconds, 0.0f, 1.0f);
            item.alpha = baseAlphaFor(p.velocity) * fadeIn *
                         (1.0f - smoothstep(tk::kFadeOutStartU, tk::kFadeZeroU, u));
        }
        out[n++] = item;
    }
    return n;
}

} // namespace fx
} // namespace piano
