#include "fx/FxLayer.h"

#include <cmath>

#include "core/Event.h"
#include "fx/FxTokens.h"

namespace piano {
namespace fx {

namespace tk = theme::fx;

void FxLayer::drawSprite(int shape, float x, float y, float edge, float rotRad, Color tint) const {
    const Texture2D& tex = atlas_.texture();
    const Rectangle dst{ x, y, edge, edge };
    const Vector2 origin{ edge * 0.5f, edge * 0.5f };
    DrawTexturePro(tex, atlas_.cellRect(shape), dst, origin,
                   rotRad * (180.0f / kPi), tint);
}

/// 来源配色（不新增颜色令牌）：用户弹奏金色，曲目/回放用主文字色。
static Color sourceTint(std::uint8_t source, float alpha) {
    const Color base = source == static_cast<std::uint8_t>(Source::User) ? theme::kGold
                                                                         : theme::kInk;
    return Color{ base.r, base.g, base.b,
                  static_cast<unsigned char>(clampf(alpha, 0.0f, 1.0f) * 255.0f + 0.5f) };
}

void FxLayer::draw(const NoteFx& noteFx, const PianoView& view, const UiLayout& layout) const {
    if (!atlas_.ready()) return;
    if (layout.fxStage.height <= 0.0f) return;

    RenderItem items[tk::kPoolCapacity];
    const int n = noteFx.collect(items, tk::kPoolCapacity);
    if (n <= 0) return;

    // 裁剪 = 舞台带矩形，下缘延伸到键顶：音符不得覆盖白键表面。
    const Rectangle clip{ layout.fxStage.x, layout.fxStage.y, layout.fxStage.width,
                          layout.keys.y - layout.fxStage.y };
    beginScissorLogical(clip, viewScale_, viewOffset_);

    // 「少」档与减少动态效果不画柔光；减少动态效果的粒子（shape==3）本身就是柔光点。
    const bool drawGlow = noteFx.level() != Level::Low && !noteFx.reducedMotion();

    // 先画全部柔光（普通 alpha 混合，不加色）
    if (drawGlow) {
        for (int i = 0; i < n; ++i) {
            const RenderItem& it = items[static_cast<std::size_t>(i)];
            if (it.shape > 2 || it.alpha <= 0.0f) continue;
            float kx = 0.0f, ky = 0.0f;
            view.keyTopCenter(it.midi, kx, ky);
            // 柔光斑直径铺满单元格（fill=1.0），不除 kGlyphCellFill
            const float edge = tk::kBaseDrawSizePx * it.scale * tk::kGlowScaleMul;
            drawSprite(3, kx + it.offsetX, ky - it.offsetY, edge, 0.0f,
                       sourceTint(it.source, it.alpha * tk::kGlowAlphaMul));
        }
    }

    // 再画全部音符
    for (int i = 0; i < n; ++i) {
        const RenderItem& it = items[static_cast<std::size_t>(i)];
        if (it.alpha <= 0.0f) continue;
        float kx = 0.0f, ky = 0.0f;
        view.keyTopCenter(it.midi, kx, ky);
        const float px = kx + it.offsetX;
        const float py = ky - it.offsetY;

        float alpha = it.alpha;
        // 减少动态效果（shape==3）的粒子画图集第 4 格柔光点本身（直径铺满单元格）；
        // 音符字形按设计高度占比 kGlyphCellFill 换算目标尺寸。
        const int shape = it.shape == 3 ? 3 : it.shape;
        const float edge = it.shape == 3
            ? tk::kBaseDrawSizePx * it.scale
            : tk::kBaseDrawSizePx * it.scale / tk::kGlyphCellFill;

        // 防御性保留：包围盒与控制行相交则衰减（正常被 scissor 挡住不会触发）
        const Rectangle bbox{ px - edge * 0.5f, py - edge * 0.5f, edge, edge };
        if (CheckCollisionRecs(bbox, layout.controls)) alpha *= tk::kControlRowAlphaMul;

        drawSprite(shape, px, py, edge, it.rotation, sourceTint(it.source, alpha));
    }

    EndScissorMode();
}

} // namespace fx
} // namespace piano
