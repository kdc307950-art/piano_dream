#pragma once
//
// fx/FxLayer.h —— 「按键飘出音符」渲染层（raylib 绘制，只画不测）
//
// 每帧从 NoteFx collect 到栈上固定数组，用 PianoView::keyTopCenter 解析键位 X，
// 在舞台带矩形内裁剪绘制。先画全部柔光、再画全部音符，普通 alpha 混合。
//
#include "fx/NoteAtlas.h"
#include "fx/NoteParticles.h"
#include "ui/PianoView.h"
#include "ui/Theme.h"

namespace piano {
namespace fx {

/// 逻辑矩形 → 屏幕像素的裁剪框换算（与 PianoView::draw 里
/// viewOffset_ + rect × viewScale_ 的手动换算同模式，高 DPI 下不能直接用逻辑坐标）。
inline void beginScissorLogical(Rectangle r, float scale, Vector2 offset) {
    BeginScissorMode(static_cast<int>(offset.x + r.x * scale),
                     static_cast<int>(offset.y + r.y * scale),
                     static_cast<int>(r.width * scale),
                     static_cast<int>(r.height * scale));
}

class FxLayer {
public:
    /// rasterScale 变化的重烘焙防抖由调用方负责（仿 App 字体重载）。
    bool bake(float rasterScale) { return atlas_.bake(rasterScale); }
    void unload() { atlas_.unload(); }
    bool ready() const { return atlas_.ready(); }

    void setViewTransform(float scale, Vector2 offset) noexcept {
        viewScale_ = scale; viewOffset_ = offset;
    }

    /// 在 drawConsole()（琴键+控制行）之后、下拉菜单之前调用。不碰 consumedClick。
    void draw(const NoteFx& noteFx, const PianoView& view, const UiLayout& layout) const;

private:
    void drawSprite(int shape, float x, float y, float edge, float rotRad, Color tint) const;

    NoteAtlas atlas_;
    float viewScale_ = 1.0f;
    Vector2 viewOffset_{};
};

} // namespace fx
} // namespace piano
