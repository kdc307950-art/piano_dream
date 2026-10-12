#include "ui/PianoView.h"

#include <algorithm>
#include <cmath>

#include "ui/Widgets.h"

namespace piano {

void PianoView::setLayout(const UiLayout& layout) {
    // 几何只与键宽有关；每帧重建 88 个键是浪费，这里做一次廉价判断
    const bool needGeom = whites_.empty() ||
                          std::fabs(layout.keyW - geomKeyW_) > 0.01f ||
                          std::fabs(layout.blackW - (geomKeyW_ * 0.636f)) > 0.01f;
    layout_ = layout;
    if (needGeom) {
        rebuildGeometry();
    } else {
        scrollX_ = std::clamp(scrollX_, 0.0f, maxScroll());
        targetScroll_ = std::clamp(targetScroll_, 0.0f, maxScroll());
    }
}

void PianoView::rebuildGeometry() {
    whites_.clear();
    blacks_.clear();
    whites_.reserve(kWhiteKeyCount);
    blacks_.reserve(36);

    int whiteIndex = 0;
    for (int midi = kMidiLow; midi <= kMidiHigh; ++midi) {
        if (isBlackKey(midi)) {
            // 黑键骑在相邻两个白键的接缝上，与 HTML 原版的排版一致
            blacks_.push_back(KeyGeom{ midi, static_cast<float>(whiteIndex) * layout_.keyW - layout_.blackW * 0.5f, true });
        } else {
            whites_.push_back(KeyGeom{ midi, static_cast<float>(whiteIndex) * layout_.keyW, false });
            ++whiteIndex;
        }
    }
    geomKeyW_ = layout_.keyW;
    scrollX_ = std::clamp(scrollX_, 0.0f, maxScroll());
    targetScroll_ = scrollX_;
}

float PianoView::contentWidth() const noexcept {
    return static_cast<float>(kWhiteKeyCount) * layout_.keyW;
}

float PianoView::maxScroll() const noexcept {
    return std::max(0.0f, contentWidth() - layout_.keys.width);
}

void PianoView::scrollBy(float dx) {
    scrollX_ = std::clamp(scrollX_ + dx, 0.0f, maxScroll());
    targetScroll_ = scrollX_;
    animating_ = false;
}

void PianoView::centerOn(int midi, bool smooth) {
    const int clamped = clampNote(midi);
    const float x = xOfNote(clamped);
    targetScroll_ = std::clamp(x - layout_.keys.width * 0.5f + layout_.keyW * 0.5f, 0.0f, maxScroll());
    if (!smooth) {
        scrollX_ = targetScroll_;
        animating_ = false;
    } else {
        animating_ = true;
    }
}

void PianoView::updateScrollAnimation(float dt) {
    if (!animating_) return;
    const float diff = targetScroll_ - scrollX_;
    if (std::fabs(diff) < 0.5f) {
        scrollX_ = targetScroll_;
        animating_ = false;
        return;
    }
    const float k = 1.0f - std::exp(-dt * 12.0f);   // 帧率无关的指数趋近
    scrollX_ += diff * k;
}

float PianoView::xOfNote(int midi) const {
    if (midi < kMidiLow || midi > kMidiHigh) return 0.0f;
    for (const auto& k : whites_) if (k.midi == midi) return k.x;
    for (const auto& k : blacks_) if (k.midi == midi) return k.x + layout_.blackW * 0.5f;
    return 0.0f;
}

bool PianoView::keyTopCenter(int midi, float& x, float& y) const {
    x = 0.0f;
    y = layout_.keys.y;
    if (midi < kMidiLow || midi > kMidiHigh) return false;

    float cx = 0.0f;
    bool found = false;
    for (const auto& k : whites_) {
        if (k.midi == midi) { cx = k.x + layout_.keyW * 0.5f; found = true; break; }
    }
    if (!found) {
        for (const auto& k : blacks_) {
            if (k.midi == midi) { cx = k.x + layout_.blackW * 0.5f; found = true; break; }
        }
    }
    if (!found) return false;

    x = layout_.keys.x + cx - scrollX_;
    return x >= layout_.keys.x && x <= layout_.keys.x + layout_.keys.width;
}

int PianoView::visibleCenterNote() const {
    const float centerX = scrollX_ + layout_.keys.width * 0.5f;
    int best = 60;
    float bestDist = 1e9f;
    for (const auto& k : whites_) {
        const float d = std::fabs(k.x + layout_.keyW * 0.5f - centerX);
        if (d < bestDist) { bestDist = d; best = k.midi; }
    }
    return best;
}

bool PianoView::hitTest(float x, float y, int& midiOut, float& velocityOut) const {
    const Rectangle& vp = layout_.keys;
    if (!CheckCollisionPointRec({ x, y }, vp)) return false;

    const float cx = x - vp.x + scrollX_;   // 内容坐标
    const float cy = y - vp.y;

    // 黑键在上层，优先命中
    for (const auto& k : blacks_) {
        if (cx >= k.x && cx < k.x + layout_.blackW && cy >= 0.0f && cy < layout_.blackH) {
            midiOut = k.midi;
            velocityOut = std::clamp(0.46f + (cy / layout_.blackH) * 0.52f, 0.33f, 1.0f);
            return true;
        }
    }
    for (const auto& k : whites_) {
        if (cx >= k.x && cx < k.x + layout_.keyW && cy >= 0.0f && cy < layout_.keyH) {
            midiOut = k.midi;
            velocityOut = std::clamp(0.46f + (cy / layout_.keyH) * 0.52f, 0.33f, 1.0f);
            return true;
        }
    }
    return false;
}

void PianoView::draw(const Mixer& mixer, const InputRouter& input, const FontBook& fonts) const {
    const Rectangle& vp = layout_.keys;

    // 键床：深色琴身 + 弥散投影 + 沿口一丝漆光
    const Rectangle bed{ vp.x - 6.0f, vp.y - 6.0f, vp.width + 12.0f, vp.height + 10.0f };
    widget::softShadow(bed, 0.05f, 6.0f, 58);
    DrawRectangleRounded(bed, 0.05f, 8, Color{ 9, 12, 17, 255 });
    DrawRectangleRounded({ bed.x + 10.0f, bed.y + 1.0f, bed.width - 20.0f, 1.0f }, 1.0f, 2,
                         Color{ 255, 255, 255, 14 });

    BeginScissorMode(static_cast<int>(viewOffset_.x + vp.x * viewScale_),
                     static_cast<int>(viewOffset_.y + vp.y * viewScale_),
                     static_cast<int>(vp.width * viewScale_),
                     static_cast<int>(vp.height * viewScale_));

    const int w0 = static_cast<int>(std::floor(scrollX_ / layout_.keyW));
    const int w1 = std::min(kWhiteKeyCount - 1,
                            static_cast<int>(std::ceil((scrollX_ + vp.width) / layout_.keyW)) + 1);

    // ---- 白键 ----
    // 象牙暖白渐变 + 底部圆角 + 前沿阴影；键间留一丝缝隙露出键床，像真琴的键缝。
    for (int i = w0; i <= w1 && i < static_cast<int>(whites_.size()); ++i) {
        const KeyGeom& k = whites_[static_cast<std::size_t>(i)];
        const float sx = vp.x + k.x - scrollX_;
        const bool active = mixer.visualCount(k.midi) > 0;

        Rectangle r{ sx + 0.5f, vp.y, layout_.keyW - 1.0f, layout_.keyH };
        const float radius = std::min(4.0f, layout_.keyW * 0.16f);
        const float roundness = std::min(1.0f, radius * 2.0f / r.width);

        if (active) {
            DrawRectangleRounded(r, roundness, 6, Color{ 226, 192, 134, 255 });
            DrawRectangleGradientV(static_cast<int>(r.x), static_cast<int>(r.y),
                                   static_cast<int>(r.width), static_cast<int>(r.height - radius),
                                   theme::kWhiteKeyActive, Color{ 226, 192, 134, 255 });
            // 按下时键面下沉：顶部压一道内阴影
            DrawRectangleGradientV(static_cast<int>(r.x), static_cast<int>(r.y),
                                   static_cast<int>(r.width), 12,
                                   Color{ 110, 76, 30, 85 }, Color{ 110, 76, 30, 0 });
            // 键面下段泛起一层暖金光
            DrawRectangleGradientV(static_cast<int>(r.x + 2.0f), static_cast<int>(r.y + r.height * 0.45f),
                                   static_cast<int>(r.width - 4.0f),
                                   static_cast<int>(r.height * 0.55f - radius),
                                   Color{ 255, 216, 144, 0 }, Color{ 255, 216, 144, 80 });
            DrawRectangleRoundedLinesEx(r, roundness, 6, 1.0f, Color{ 198, 156, 92, 255 });
        } else {
            DrawRectangleRounded(r, roundness, 6, theme::kWhiteKeyBot);
            DrawRectangleGradientV(static_cast<int>(r.x), static_cast<int>(r.y),
                                   static_cast<int>(r.width), static_cast<int>(r.height - radius),
                                   theme::kWhiteKeyTop, theme::kWhiteKeyBot);
            // 键前沿（下端）一道落影，让每根键有厚度
            DrawRectangleRounded({ r.x + radius * 0.6f, r.y + r.height - 3.2f,
                                   r.width - radius * 1.2f, 1.6f }, 1.0f, 2, theme::kWhiteKeyEdge);
        }

        // 每个 C 键标注音名
        if (k.midi % 12 == 0) {
            const std::string name = noteName(k.midi);
            const float tw = fonts.smallWidth(name.c_str());
            fonts.drawSmall(name.c_str(), r.x + (r.width - tw) * 0.5f, r.y + r.height - 44.0f,
                            active ? Color{ 96, 68, 30, 255 } : Color{ 116, 126, 142, 255 });
        }
        // 电脑键盘快捷键标签
        if (map_) {
            if (const char* lb = map_->labelForNote(k.midi)) {
                const float tw = fonts.smallWidth(lb);
                const float bx = r.x + (r.width - std::max(19.0f, tw + 8.0f)) * 0.5f;
                const Rectangle badge{ bx, r.y + r.height - 20.0f, std::max(19.0f, tw + 8.0f), 14.0f };
                DrawRectangleRounded(badge, 0.35f, 4,
                                     active ? Color{ 120, 84, 36, 110 } : Color{ 30, 40, 55, 20 });
                fonts.drawSmall(lb, bx + (badge.width - tw) * 0.5f, badge.y + 1.0f,
                                active ? Color{ 70, 48, 18, 255 } : Color{ 96, 108, 124, 255 });
            }
        }
    }

    // ---- 黑键 ----
    // 漆面质感：顶部亮、底部沉，上段一道竖向反光带，左侧一条棱线高光。
    for (const auto& k : blacks_) {
        const float sx = vp.x + k.x - scrollX_;
        if (sx + layout_.blackW < vp.x - 4.0f || sx > vp.x + vp.width + 4.0f) continue;

        const bool active = mixer.visualCount(k.midi) > 0;
        const Rectangle r{ sx + 0.5f, vp.y, layout_.blackW - 1.0f, layout_.blackH };
        const float radius = std::min(3.5f, layout_.blackW * 0.22f);
        const float roundness = std::min(1.0f, radius * 2.0f / r.width);

        // 双层投影，比单层更柔
        DrawRectangleRounded({ r.x + 1.6f, r.y + 3.2f, r.width, r.height }, roundness, 6,
                             Color{ 0, 0, 0, 60 });
        DrawRectangleRounded({ r.x + 0.8f, r.y + 1.6f, r.width, r.height }, roundness, 6,
                             Color{ 0, 0, 0, 110 });

        if (active) {
            DrawRectangleRounded(r, roundness, 6, Color{ 96, 64, 30, 255 });
            DrawRectangleGradientV(static_cast<int>(r.x), static_cast<int>(r.y),
                                   static_cast<int>(r.width), static_cast<int>(r.height - radius),
                                   theme::kBlackKeyActive, Color{ 96, 64, 30, 255 });
            DrawRectangleGradientV(static_cast<int>(r.x), static_cast<int>(r.y),
                                   static_cast<int>(r.width), 10,
                                   Color{ 80, 50, 16, 95 }, Color{ 80, 50, 16, 0 });
            DrawRectangleRoundedLinesEx(r, roundness, 6, 1.0f, Color{ 214, 170, 104, 255 });
        } else {
            DrawRectangleRounded(r, roundness, 6, theme::kBlackKeyBot);
            DrawRectangleGradientV(static_cast<int>(r.x), static_cast<int>(r.y),
                                   static_cast<int>(r.width), static_cast<int>(r.height - radius),
                                   theme::kBlackKeyTop, theme::kBlackKeyBot);
            // 漆皮反光
            DrawRectangleGradientV(static_cast<int>(r.x + 2.0f), static_cast<int>(r.y + 3.0f),
                                   static_cast<int>(r.width - 4.0f), static_cast<int>(r.height * 0.42f),
                                   Color{ 255, 255, 255, 30 }, Color{ 255, 255, 255, 0 });
            DrawLineEx({ r.x + 1.2f, r.y + 3.0f }, { r.x + 1.2f, r.y + r.height - radius },
                       1.0f, Color{ 255, 255, 255, 22 });
        }

        if (map_) {
            if (const char* lb = map_->labelForNote(k.midi)) {
                const float tw = fonts.smallWidth(lb);
                fonts.drawSmall(lb, r.x + (r.width - tw) * 0.5f, r.y + r.height - 20.0f,
                                active ? Color{ 255, 234, 196, 255 } : Color{ 198, 210, 226, 160 });
            }
        }
    }

    // 呢毡条：三角钢琴琴键没入琴盖下的那道深红，是整个键盘的点睛之笔
    DrawRectangleGradientV(static_cast<int>(vp.x), static_cast<int>(vp.y),
                           static_cast<int>(vp.width), 5,
                           Color{ 150, 36, 42, 255 }, Color{ 102, 20, 26, 255 });
    DrawRectangleGradientV(static_cast<int>(vp.x), static_cast<int>(vp.y + 5.0f),
                           static_cast<int>(vp.width), 7,
                           Color{ 0, 0, 0, 70 }, Color{ 0, 0, 0, 0 });

    EndScissorMode();

    // 边框：外层深描边 + 内侧一丝暖金细线
    DrawRectangleRoundedLinesEx(bed, 0.05f, 8, 2.0f, Color{ 4, 6, 10, 255 });
    DrawRectangleRoundedLinesEx({ bed.x + 2.0f, bed.y + 2.0f, bed.width - 4.0f, bed.height - 4.0f },
                                0.05f, 8, 1.0f, Color{ 213, 173, 106, 20 });

    // 滚动条
    if (maxScroll() > 0.5f) {
        const float trackY = vp.y + vp.height + 5.0f;
        const float visibleRatio = vp.width / contentWidth();
        const float barW = std::max(40.0f, vp.width * visibleRatio);
        const float barX = vp.x + (vp.width - barW) * (scrollX_ / maxScroll());
        DrawRectangleRounded({ vp.x, trackY, vp.width, 4.0f }, 1.0f, 4, Color{ 255, 255, 255, 14 });
        DrawRectangleRounded({ barX, trackY, barW, 4.0f }, 1.0f, 4, Color{ 172, 136, 84, 255 });
        DrawRectangleRounded({ barX, trackY, barW, 1.5f }, 1.0f, 2, Color{ 255, 226, 178, 80 });
    }
}

} // namespace piano
