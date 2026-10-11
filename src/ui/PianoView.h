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
};

} // namespace piano
