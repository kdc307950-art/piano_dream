#pragma once
//
// ui/PianoView.h —— 88 键键盘视图
//
// 负责：琴键几何计算、绘制、滚动、命中测试（含按 Y 位置取力度）。
// 不直接发声，只读取 Mixer 的高亮计数 —— UI 与音频彻底解耦。
//
#include <vector>

#include "raylib.h"

#include "audio/Mixer.h"
#include "core/Note.h"
#include "input/InputRouter.h"
#include "input/KeyboardMap.h"
#include "ui/FontBook.h"
#include "ui/Theme.h"

namespace piano {

class PianoView {
public:
    struct KeyGeom {
        int   midi = 0;
        float x = 0.0f;
        bool  black = false;
    };

    void setLayout(const UiLayout& layout);
    void setViewTransform(float scale, Vector2 offset) noexcept { viewScale_ = scale; viewOffset_ = offset; }
    void setKeyboardMap(const KeyboardMap* map) { map_ = map; }

    float scroll() const noexcept { return scrollX_; }
    float contentWidth() const noexcept;
    float maxScroll() const noexcept;
    void  scrollBy(float dx);
    void  centerOn(int midi, bool smooth = true);
    void  updateScrollAnimation(float dt);

    /// 屏幕坐标 → 音高 + 力度。力度按「越靠近琴键下沿越强」计算，与 HTML 一致。
    bool hitTest(float x, float y, int& midiOut, float& velocityOut) const;

    /// 视口正中间最接近的白键音高。
    int visibleCenterNote() const;

    /// 某个音高在内容坐标系里的左边界 X。
    float xOfNote(int midi) const;

    /// 键顶中心（内容/逻辑坐标系，含滚动换算）：x=键中心（白键 x+keyW/2，黑键 x+blackW/2），
    /// y=键顶（layout_.keys.y）。返回键是否在可视视口内（供特效层跳过屏外粒子）。
    bool keyTopCenter(int midi, float& x, float& y) const;

    void draw(const Mixer& mixer, const InputRouter& input, const FontBook& fonts) const;

private:
    void rebuildGeometry();

    UiLayout layout_{};
    const KeyboardMap* map_ = nullptr;

    std::vector<KeyGeom> whites_;
    std::vector<KeyGeom> blacks_;

    float scrollX_ = 0.0f;
    float targetScroll_ = 0.0f;
    bool  animating_ = false;
    float geomKeyW_ = 0.0f;
    float viewScale_ = 1.0f;
    Vector2 viewOffset_{};
};

} // namespace piano
