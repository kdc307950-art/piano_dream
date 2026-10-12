#pragma once
//
// fx/FxTokens.h —— 「按键飘出音符」特效的全部数值令牌
//
// 纯 constexpr，不依赖 raylib / miniaudio，可被离线自测直接引用。
// 命名空间挂在 theme 之下，与 ui/Theme.h 的 theme 汇合；
// 特效代码中不得出现裸数字。
//
namespace piano {
namespace theme {
namespace fx {

// ---- 寿命（秒）：音高越低飘得越久 ----
inline constexpr float kLifetimeBase         = 1.8f;
inline constexpr float kLifetimeLowPitchMul  = 1.25f;
inline constexpr float kLifetimeHighPitchMul = 0.85f;

// ---- 淡入淡出（u = 生命周期比例）----
inline constexpr float kFadeInSeconds = 0.10f;   // 不短于 100ms
inline constexpr float kFadeOutStartU = 0.55f;   // 从此比例开始淡出
inline constexpr float kFadeZeroU     = 0.90f;   // 此比例前完全透明（保证消失在舞台顶部之前）

// ---- 缩放：先回弹生长到 1.0，再缓涨到 1.35 ----
inline constexpr float kScaleStart = 0.25f;
inline constexpr float kScaleMid   = 1.0f;
inline constexpr float kScaleEnd   = 1.35f;
inline constexpr float kGrowEndU   = 0.30f;    // 回弹生长段结束的生命周期比例
inline constexpr float kBackEaseC  = 1.2f;     // backOut 回弹系数

// ---- 上升 ----
inline constexpr float kRiseDistanceStageMul = 0.95f;  // 上升总距离 = 舞台高度 × 此值

// ---- 摇摆 / 旋转 ----
inline constexpr float kSwayAmplitudePx = 6.0f;
inline constexpr float kSwayCycles      = 1.5f;
inline constexpr float kRotationMaxRad  = 0.35f;

// ---- 双音符：力度足够时追加第二个（延迟队列）----
inline constexpr float kSecondDelaySeconds = 0.060f;
inline constexpr float kSecondScaleMul     = 0.70f;
// 键盘固定力度 0.79 必须够不到双音符，阈值从 0.75 提到 0.85
inline constexpr float kTwoNoteVelocity    = 0.85f;

// ---- 限流 ----
inline constexpr float kCooldownSeconds  = 0.080f;
inline constexpr int   kPerFrameSpawnMax = 6;
inline constexpr int   kAliveCapLow      = 48;
inline constexpr int   kAliveCapStandard = 96;
inline constexpr int   kAliveCapHigh     = 160;
inline constexpr int   kPoolCapacity     = 160;   // 池物理容量，恒等于最高档位
inline constexpr int   kPendingCapacity  = 32;

// ---- 和弦 / 滑音负载：窗口内生成数超标后按概率丢弃 ----
inline constexpr float kChordWindowSeconds = 0.10f;
inline constexpr int   kChordLoadCount     = 3;
inline constexpr float kChordProbability   = 0.7f;   // 高负载时的保留概率
inline constexpr float kChordSizeMul       = 0.85f;

// ---- 映射：力度↑更大更亮，音高↓更大 ----
inline constexpr float kSizeVelocityBase   = 0.75f;
inline constexpr float kSizeVelocityRange  = 0.50f;
inline constexpr float kSizeLowPitchMul    = 1.30f;
inline constexpr float kSizeHighPitchMul   = 0.80f;
inline constexpr float kAlphaBase          = 0.60f;
inline constexpr float kAlphaVelocityRange = 0.40f;

// ---- 减少动态效果：柔光点，无位移、无旋转、短寿命 ----
inline constexpr float kReducedLifetimeSeconds = 0.25f;
inline constexpr float kReducedScale           = 0.60f;
inline constexpr float kReducedFadeInSeconds   = 0.05f;

// ---- 柔光斑 ----
inline constexpr float kGlowScaleMul = 2.2f;    // 柔光绘制尺寸相对音符的倍数
inline constexpr float kGlowAlphaMul = 0.35f;   // 柔光透明度衰减

// ---- 图集字形比例（全部为单元格边长的分数，y 向下）----
inline constexpr float kGlyphCellFill      = 0.70f;   // 音符字形设计高度占单元格比例
inline constexpr float kGlyphHeadRx        = 0.150f;  // 符头椭圆半径
inline constexpr float kGlyphHeadRy        = 0.105f;
inline constexpr float kGlyphHeadTiltDeg   = -18.0f;  // 符头略倾斜
inline constexpr float kGlyphHeadCx        = 0.36f;   // 单音符符头中心
inline constexpr float kGlyphHeadCy        = 0.72f;
inline constexpr float kGlyphStemWidth     = 0.030f;  // 符杆贴符头右侧
inline constexpr float kGlyphStemTop       = 0.14f;   // 符杆顶端
inline constexpr float kGlyphFlagSweep     = 0.30f;   // ♪ 符尾横向甩出
inline constexpr float kGlyphFlagDrop      = 0.34f;   // ♪ 符尾下落
inline constexpr float kGlyphHead2Cx       = 0.64f;   // ♫ 第二个符头中心
inline constexpr float kGlyphHead2Cy       = 0.70f;
inline constexpr float kGlyphBeamThickness = 0.085f;  // ♫ 横梁厚度
inline constexpr float kGlyphBeamSlant     = 0.06f;   // ♫ 横梁右端下沉
inline constexpr float kGlowRadius         = 0.50f;   // 柔光半径（直径铺满单元格）
inline constexpr float kGlowFalloff        = 1.8f;    // 柔光径向衰减指数
inline constexpr int   kAtlasSupersample   = 4;       // 每像素 4×4 超采样抗锯齿（≈1.5px 过渡）

// ---- 事件时效 / 步长上限 ----
inline constexpr float  kStaleEventSeconds = 0.150f;
inline constexpr double kDtMaxSeconds      = 0.05;   // 1/20s

// ---- 预留（阶段 2/3 使用，本期只定义不引用）----
inline constexpr float kBudgetMs            = 0.4f;
inline constexpr int   kDegradeFrames       = 120;
inline constexpr int   kDegradeIgnoreFrames = 30;
inline constexpr float kStageMinHeight      = 80.f;
inline constexpr float kStageMaxHeight      = 160.f;
inline constexpr float kBaseDrawSizePx      = 28.f;
inline constexpr float kControlRowAlphaMul  = 0.7f;
inline constexpr int   kAtlasCellMin        = 64;
inline constexpr int   kAtlasCellMax        = 256;

} // namespace fx
} // namespace theme
} // namespace piano
