#pragma once
//
// fx/NoteParticles.h —— 「按键飘出音符」粒子逻辑（纯逻辑，严禁依赖 raylib）
//
// 设计要点：
//   - 固定容量对象池（std::array，运行期零分配）
//   - 位置/缩放/透明度/旋转全部是生命周期比例 u 的解析函数，不做数值积分，
//     因此与帧率无关、同种子逐位可复现
//   - RNG 复用 audio/Dsp.h 的 NoiseGen（xorshift32，带种子）；
//     每个粒子只抽一次 32-bit 种子，形状/相位/旋转方向全部由它位混合推出
//   - 模块自带时钟：ingest 的 eventTime 与 update(dt) 同属一个秒级时钟域
//
#include <array>
#include <cstdint>

#include "audio/Dsp.h"
#include "fx/FxTokens.h"

namespace piano {
namespace fx {

enum class Level : std::uint8_t { Off = 0, Low, Standard, High };

/// update 后由 UI 收集绘制，坐标为逻辑像素。
struct RenderItem {
    std::uint8_t midi   = 0;   // 键身份（UI 用 keyTopCenter 解析 X）
    std::uint8_t shape  = 0;   // 0=♩ 1=♪ 2=♫；3=柔光点（减少动态效果专用）
    std::uint8_t source = 0;   // Source 枚举值（阶段 3 配色用）
    float offsetX  = 0.0f;     // 摇摆横向偏移（px）
    float offsetY  = 0.0f;     // 上升距离（px，≥0，向上）
    float scale    = 0.0f;     // 乘基础绘制尺寸
    float alpha    = 0.0f;     // 0..1
    float rotation = 0.0f;     // 弧度
};

class NoteFx {
public:
    NoteFx();

    void setLevel(Level level);
    void setReducedMotion(bool on);
    void setStageHeight(float px);         // 上升距离 = px × kRiseDistanceStageMul（出生时快照）
    void seedRng(std::uint32_t seed);
    void setAliveCapScale(float s);        // 自适应降级用（默认 1.0，阶段 3 用 0.5）

    /// 清空粒子池 / 延迟队列 / 计数器，保留种子与配置（窗口尺寸或缩放变化时调用）。
    void clear();

    Level level() const { return level_; }
    bool reducedMotion() const { return reducedMotion_; }

    /// eventTime 为模块时钟域的秒（UI 由 audioFrame 换算）。
    /// 晚于 now() 的按 now() 处理；早于 now() − kStaleEventSeconds 的直接丢弃。
    void ingest(std::uint8_t midi, float velocity, std::uint8_t source, double eventTime);

    /// dt 钳到 [0, kDtMaxSeconds]；推进时钟、处理延迟队列、回收死亡粒子。
    void update(double dt);

    double now() const { return now_; }
    int aliveCount() const { return aliveCount_; }
    int pendingCount() const { return pendingCount_; }
    int capacity() const { return theme::fx::kPoolCapacity; }
    int effectiveAliveCap() const;

    /// 填充数组并返回数量，零分配。out 可容纳 maxOut 个。
    int collect(RenderItem* out, int maxOut) const;

    // ---- 纯映射函数（令牌驱动，供测试与内部共用）----
    static float baseScaleFor(float velocity, int midi);   // 力度↑大、音高↓大
    static float baseLifetimeFor(int midi);                // 音高↓久
    static float baseAlphaFor(float velocity);             // 力度↑亮

private:
    struct Particle {
        bool active = false;
        std::uint8_t midi = 0, shape = 0, source = 0;
        float velocity  = 0.0f;
        float baseScale = 1.0f;   // 力度/音高映射出的基准缩放（减少动态效果时为 kReducedScale）
        float scaleMul  = 1.0f;   // 双音符 / 和弦负载的额外缩放
        float lifetime  = 1.0f;
        float riseDist  = 0.0f;   // 出生时的 stageHeight 快照 × 0.95
        float phase     = 0.0f;   // 摇摆相位（来自粒子种子）
        float swayAmp   = theme::fx::kSwayAmplitudePx;  // 摇摆振幅（「少」档为 0）
        float rotDir    = 1.0f;   // 旋转方向 ±1（来自粒子种子）
        double birth    = 0.0;
    };

    struct PendingNote {
        std::uint8_t midi = 0, source = 0;
        float velocity = 0.0f;
        float scaleMul = 1.0f;
        double due = 0.0;
    };

    static float lerp(float a, float b, float t);
    static float backOut(float t);                 // 轻微回弹缓出
    static float smoothstep(float e0, float e1, float x);
    static float pitchNorm(int midi);              // 21→0，108→1

    int  levelCap() const;
    bool trySpawn(std::uint8_t midi, float velocity, std::uint8_t source,
                  double birth, float scaleMul, bool honorCooldown);

    Level level_ = Level::Standard;
    bool  reducedMotion_ = false;
    float stageHeight_ = 100.0f;
    float aliveCapScale_ = 1.0f;

    NoiseGen rng_;
    double now_ = 0.0;

    std::array<Particle, theme::fx::kPoolCapacity> pool_{};
    int aliveCount_ = 0;

    std::array<PendingNote, theme::fx::kPendingCapacity> pending_{};
    int pendingHead_ = 0;
    int pendingCount_ = 0;

    std::array<double, 128> lastSpawn_{};          // 同 midi 冷却
    std::array<double, 8> recentSpawns_{};         // 和弦/滑音负载窗口
    int recentCursor_ = 0;
    int spawnedThisFrame_ = 0;                     // 两次 update 之间的生成数
};

} // namespace fx
} // namespace piano
