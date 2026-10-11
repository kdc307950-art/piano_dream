#include "ui/PianoView.h"

#include <algorithm>
#include <cmath>

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

    // 键床背景
    DrawRectangleRounded({ vp.x - 4.0f, vp.y - 4.0f, vp.width + 8.0f, vp.height + 8.0f },
                         0.03f, 6, Color{ 8, 11, 15, 255 });

    BeginScissorMode(static_cast<int>(vp.x), static_cast<int>(vp.y),
                     static_cast<int>(vp.width), static_cast<int>(vp.height));

    const int w0 = static_cast<int>(std::floor(scrollX_ / layout_.keyW));
    const int w1 = std::min(kWhiteKeyCount - 1,
                            static_cast<int>(std::ceil((scrollX_ + vp.width) / layout_.keyW)) + 1);

    // ---- 白键 ----
    for (int i = w0; i <= w1 && i < static_cast<int>(whites_.size()); ++i) {
        const KeyGeom& k = whites_[static_cast<std::size_t>(i)];
        const float sx = vp.x + k.x - scrollX_;
        const bool active = mixer.visualCount(k.midi) > 0;

        Rectangle r{ sx, vp.y, layout_.keyW, layout_.keyH };
        if (active) {
            DrawRectangleGradientV(static_cast<int>(r.x), static_cast<int>(r.y),
                                   static_cast<int>(r.width), static_cast<int>(r.height),
                                   Color{ 245, 223, 175, 255 }, Color{ 229, 196, 139, 255 });
            DrawRectangleRoundedLinesEx(r, 0.0f, 2, 1.0f, Color{ 190, 150, 90, 255 });
            // 按下时键面下沉，用一层顶部阴影表现
            DrawRectangleGradientV(static_cast<int>(r.x), static_cast<int>(r.y),
                                   static_cast<int>(r.width), 10,
                                   Color{ 150, 110, 60, 60 }, Color{ 0, 0, 0, 0 });
        } else {
            DrawRectangleGradientV(static_cast<int>(r.x), static_cast<int>(r.y),
                                   static_cast<int>(r.width), static_cast<int>(r.height),
                                   theme::kWhiteKeyMid, Color{ 238, 238, 237, 255 });
            DrawRectangleGradientH(static_cast<int>(r.x), static_cast<int>(r.y),
                                   static_cast<int>(r.width), static_cast<int>(r.height),
                                   Color{ 184, 187, 193, 70 }, Color{ 191, 196, 202, 60 });
            DrawLineEx({ r.x, r.y }, { r.x, r.y + r.height }, 1.0f, theme::kWhiteKeyEdge);
            DrawLineEx({ r.x, r.y + r.height - 3.0f }, { r.x + r.width, r.y + r.height - 3.0f },
                       3.0f, Color{ 169, 174, 182, 255 });
        }

        // 每个 C 键标注音名
        if (k.midi % 12 == 0) {
            const std::string name = noteName(k.midi);
            const float tw = fonts.smallWidth(name.c_str());
            fonts.drawSmall(name.c_str(), r.x + (r.width - tw) * 0.5f, r.y + r.height - 44.0f,
                            active ? Color{ 90, 66, 30, 255 } : Color{ 100, 112, 131, 255 });
        }
        // 电脑键盘快捷键标签
        if (map_) {
            if (const char* lb = map_->labelForNote(k.midi)) {
                const float tw = fonts.smallWidth(lb);
                const float bx = r.x + (r.width - std::max(19.0f, tw + 8.0f)) * 0.5f;
                const Rectangle badge{ bx, r.y + r.height - 20.0f, std::max(19.0f, tw + 8.0f), 14.0f };
                DrawRectangleRounded(badge, 0.35f, 4,
                                     active ? Color{ 120, 92, 50, 120 } : Color{ 120, 128, 140, 40 });
                fonts.drawSmall(lb, bx + (badge.width - tw) * 0.5f, badge.y + 1.0f,
                                active ? Color{ 70, 50, 20, 255 } : Color{ 69, 80, 93, 255 });
            }
        }
    }

    // ---- 黑键 ----
    for (const auto& k : blacks_) {
        const float sx = vp.x + k.x - scrollX_;
        if (sx + layout_.blackW < vp.x - 4.0f || sx > vp.x + vp.width + 4.0f) continue;

        const bool active = mixer.visualCount(k.midi) > 0;
        const Rectangle r{ sx, vp.y, layout_.blackW, layout_.blackH };

        DrawRectangle(static_cast<int>(r.x + 2.0f), static_cast<int>(r.y + 4.0f),
                      static_cast<int>(r.width), static_cast<int>(r.height), Color{ 0, 0, 0, 110 });

        if (active) {
            DrawRectangleGradientH(static_cast<int>(r.x), static_cast<int>(r.y),
                                   static_cast<int>(r.width), static_cast<int>(r.height),
                                   Color{ 92, 67, 36, 255 }, theme::kBlackKeyActive);
            DrawRectangleGradientV(static_cast<int>(r.x), static_cast<int>(r.y),
                                   static_cast<int>(r.width), static_cast<int>(r.height),
                                   Color{ 222, 175, 100, 60 }, Color{ 0, 0, 0, 0 });
        } else {
            DrawRectangleGradientV(static_cast<int>(r.x), static_cast<int>(r.y),
                                   static_cast<int>(r.width), static_cast<int>(r.height),
                                   theme::kBlackKeyTop, Color{ 16, 20, 28, 255 });
            DrawRectangleGradientH(static_cast<int>(r.x), static_cast<int>(r.y),
                                   static_cast<int>(r.width), static_cast<int>(r.height),
                                   Color{ 6, 8, 11, 255 }, Color{ 45, 51, 62, 255 });
        }
        DrawRectangleRoundedLinesEx(r, 0.05f, 4, 1.0f, Color{ 5, 7, 11, 255 });

        if (map_) {
            if (const char* lb = map_->labelForNote(k.midi)) {
                const float tw = fonts.smallWidth(lb);
                fonts.drawSmall(lb, r.x + (r.width - tw) * 0.5f, r.y + r.height - 20.0f,
                                active ? Color{ 255, 232, 190, 255 } : Color{ 207, 218, 234, 190 });
            }
        }
    }

    EndScissorMode();

    // 边框
    DrawRectangleRoundedLinesEx({ vp.x - 4.0f, vp.y - 4.0f, vp.width + 8.0f, vp.height + 8.0f },
                                0.03f, 6, 2.0f, Color{ 8, 11, 16, 255 });

    // 滚动条
    if (maxScroll() > 0.5f) {
        const float trackY = vp.y + vp.height + 4.0f;
        const float visibleRatio = vp.width / contentWidth();
        const float barW = std::max(40.0f, vp.width * visibleRatio);
        const float barX = vp.x + (vp.width - barW) * (scrollX_ / maxScroll());
        DrawRectangleRounded({ vp.x, trackY, vp.width, 4.0f }, 1.0f, 4, Color{ 17, 25, 36, 255 });
        DrawRectangleRounded({ barX, trackY, barW, 4.0f }, 1.0f, 4, Color{ 112, 91, 65, 255 });
    }
}

} // namespace piano
