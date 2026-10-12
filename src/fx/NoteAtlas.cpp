#include "fx/NoteAtlas.h"

#include <algorithm>
#include <cmath>

#include "audio/Dsp.h"   // clampf
#include "fx/FxTokens.h"

namespace piano {
namespace fx {

namespace tk = theme::fx;

namespace {

// ---------------------------------------------------------------------------
// SDF 基元（归一化单元格坐标 [0,1]×[0,1]，y 向下；返回值 < 0 表示在形内）
// ---------------------------------------------------------------------------

float sdSegment(float px, float py, float ax, float ay, float bx, float by) {
    const float dx = bx - ax, dy = by - ay;
    const float len2 = dx * dx + dy * dy;
    float t = len2 > 0.0f ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0f;
    t = clampf(t, 0.0f, 1.0f);
    const float qx = px - (ax + t * dx), qy = py - (ay + t * dy);
    return std::sqrt(qx * qx + qy * qy);
}

/// 倾斜椭圆的近似 SDF（圆域变换 + 距离补偿，用于覆盖率采样足够精确）。
float sdEllipse(float px, float py, float cx, float cy, float rx, float ry, float tiltDeg) {
    const float a = tiltDeg * (kPi / 180.0f);
    const float ca = std::cos(a), sa = std::sin(a);
    const float dx = px - cx, dy = py - cy;
    const float lx = (dx * ca + dy * sa) / rx;
    const float ly = (-dx * sa + dy * ca) / ry;
    return (std::sqrt(lx * lx + ly * ly) - 1.0f) * std::min(rx, ry);
}

float sdBox(float px, float py, float cx, float cy, float hx, float hy) {
    const float qx = std::fabs(px - cx) - hx;
    const float qy = std::fabs(py - cy) - hy;
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f);
}

/// 二次贝塞尔折线化的最小距离（采样成 12 段胶囊）。
float sdQuadratic(float px, float py, float ax, float ay, float cx, float cy,
                  float bx, float by, float thickness) {
    float best = 1e9f;
    float lx = ax, ly = ay;
    for (int i = 1; i <= 12; ++i) {
        const float t = static_cast<float>(i) / 12.0f;
        const float u = 1.0f - t;
        const float x = u * u * ax + 2.0f * u * t * cx + t * t * bx;
        const float y = u * u * ay + 2.0f * u * t * cy + t * t * by;
        best = std::min(best, sdSegment(px, py, lx, ly, x, y));
        lx = x; ly = y;
    }
    return best - thickness;
}

// ---------------------------------------------------------------------------
// 字形：返回该采样点的覆盖率 [0,1]
// ---------------------------------------------------------------------------

float headD(float px, float py, float cx, float cy) {
    return sdEllipse(px, py, cx, cy, tk::kGlyphHeadRx, tk::kGlyphHeadRy, tk::kGlyphHeadTiltDeg);
}

/// 符杆：贴符头右侧的竖条。
float stemD(float px, float py, float headCx, float headCy) {
    const float sx = headCx + tk::kGlyphHeadRx * 0.86f - tk::kGlyphStemWidth * 0.5f;
    const float top = tk::kGlyphStemTop;
    const float bot = headCy - tk::kGlyphHeadRy * 0.15f;
    return sdBox(px, py, sx, (top + bot) * 0.5f, tk::kGlyphStemWidth * 0.5f, (bot - top) * 0.5f);
}

float stemX(float headCx) {
    return headCx + tk::kGlyphHeadRx * 0.86f - tk::kGlyphStemWidth * 0.5f;
}

float covQuarter(float px, float py) {    // ♩
    return (std::min(headD(px, py, tk::kGlyphHeadCx, tk::kGlyphHeadCy),
                     stemD(px, py, tk::kGlyphHeadCx, tk::kGlyphHeadCy)) < 0.0f) ? 1.0f : 0.0f;
}

float covEighth(float px, float py) {     // ♪ = ♩ + 符尾
    float d = std::min(headD(px, py, tk::kGlyphHeadCx, tk::kGlyphHeadCy),
                       stemD(px, py, tk::kGlyphHeadCx, tk::kGlyphHeadCy));
    const float sx = stemX(tk::kGlyphHeadCx);
    const float top = tk::kGlyphStemTop;
    d = std::min(d, sdQuadratic(px, py,
                                sx, top,
                                sx + tk::kGlyphFlagSweep, top + tk::kGlyphFlagDrop * 0.25f,
                                sx + tk::kGlyphFlagSweep * 0.55f, top + tk::kGlyphFlagDrop,
                                tk::kGlyphStemWidth * 0.9f));
    return d < 0.0f ? 1.0f : 0.0f;
}

float covBeamed(float px, float py) {     // ♫：双符头共梁
    float d = std::min(headD(px, py, tk::kGlyphHeadCx, tk::kGlyphHeadCy),
                       headD(px, py, tk::kGlyphHead2Cx, tk::kGlyphHead2Cy));
    d = std::min(d, stemD(px, py, tk::kGlyphHeadCx, tk::kGlyphHeadCy));
    d = std::min(d, stemD(px, py, tk::kGlyphHead2Cx, tk::kGlyphHead2Cy));
    // 横梁：连接两根符杆顶端的加厚线段，右端略下沉
    d = std::min(d, sdSegment(px, py,
                              stemX(tk::kGlyphHeadCx), tk::kGlyphStemTop,
                              stemX(tk::kGlyphHead2Cx), tk::kGlyphStemTop + tk::kGlyphBeamSlant)
                        - tk::kGlyphBeamThickness * 0.5f);
    return d < 0.0f ? 1.0f : 0.0f;
}

float covGlow(float px, float py) {       // 径向柔光斑（连续 alpha）
    const float dx = px - 0.5f, dy = py - 0.5f;
    const float r = std::sqrt(dx * dx + dy * dy) / tk::kGlowRadius;
    return std::pow(clampf(1.0f - r, 0.0f, 1.0f), tk::kGlowFalloff);
}

using CellFn = float (*)(float, float);
constexpr CellFn kCells[4] = { covQuarter, covEighth, covBeamed, covGlow };

} // namespace

// ---------------------------------------------------------------------------
// 烘焙
// ---------------------------------------------------------------------------

float NoteAtlas::maxDrawSize() {
    return tk::kBaseDrawSizePx * tk::kScaleEnd * tk::kSizeLowPitchMul *
           (tk::kSizeVelocityBase + tk::kSizeVelocityRange) * tk::kGlowScaleMul;
}

NoteAtlas::~NoteAtlas() { unload(); }

void NoteAtlas::unload() {
    if (ready_) {
        UnloadTexture(tex_);
        tex_ = Texture2D{};
        ready_ = false;
    }
    bakedScale_ = 0.0f;
    cellPx_ = 0;
}

float NoteAtlas::cellLogical() const {
    return bakedScale_ > 0.0f ? static_cast<float>(cellPx_) / bakedScale_ : 0.0f;
}

Rectangle NoteAtlas::cellRect(int shape) const {
    const int s = std::clamp(shape, 0, 3);
    return Rectangle{ static_cast<float>(s * cellPx_), 0.0f,
                      static_cast<float>(cellPx_), static_cast<float>(cellPx_) };
}

bool NoteAtlas::bake(float rasterScale) {
    const float rs = rasterScale > 0.01f ? rasterScale : 1.0f;
    if (ready_ && std::fabs(rs - bakedScale_) <= 0.001f) return false;   // 幂等

    const int cell = std::clamp(static_cast<int>(std::ceil(maxDrawSize() * rs)),
                                tk::kAtlasCellMin, tk::kAtlasCellMax);
    const int w = cell * 4;
    const int h = cell;

    Image img = GenImageColor(w, h, BLANK);   // R8G8B8A8，全透明
    Color* px = static_cast<Color*>(img.data);

    // 4×4 超采样：每个像素 16 个采样点取平均覆盖率 → 边缘约 1.5px 的抗锯齿过渡
    constexpr int SS = tk::kAtlasSupersample;
    constexpr float inv = 1.0f / (static_cast<float>(SS) * static_cast<float>(SS));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int shape = x / cell;
            const CellFn fn = kCells[shape];
            float acc = 0.0f;
            for (int sy = 0; sy < SS; ++sy) {
                for (int sx = 0; sx < SS; ++sx) {
                    const float u = (static_cast<float>(x % cell) +
                                     (static_cast<float>(sx) + 0.5f) / SS) / static_cast<float>(cell);
                    const float v = (static_cast<float>(y) +
                                     (static_cast<float>(sy) + 0.5f) / SS) / static_cast<float>(cell);
                    acc += fn(u, v);
                }
            }
            const float cov = clampf(acc * inv, 0.0f, 1.0f);
            px[y * w + x] = Color{ 255, 255, 255,
                                   static_cast<unsigned char>(std::lround(cov * 255.0f)) };
        }
    }

    Texture2D tex = LoadTextureFromImage(img);
    GenTextureMipmaps(&tex);
    // raylib 的 TEXTURE_FILTER_TRILINEAR 即「双线性采样 + mipmap 链」的表达，
    // 缩小绘制时既平滑又不闪烁。
    SetTextureFilter(tex, TEXTURE_FILTER_TRILINEAR);
    UnloadImage(img);

    unload();            // 先卸旧纹理（幂等路径已在函数开头返回）
    tex_ = tex;
    cellPx_ = cell;
    bakedScale_ = rs;
    ready_ = true;
    return true;
}

} // namespace fx
} // namespace piano
