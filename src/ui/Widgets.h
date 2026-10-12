#pragma once
//
// ui/Widgets.h —— 极简即时模式控件
//
// 不引入 GUI 库，用最少的代码画出按钮 / 开关 / 滑杆，并且让「悬停 / 按下 /
// 拖拽」的状态由 UiContext 统一管理（滑杆拖出矩形范围也不会断）。
//
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "raylib.h"

#include "ui/FontBook.h"
#include "ui/Theme.h"

namespace piano {

struct UiContext {
    Vector2 mouse{};
    bool mouseDown = false;
    bool mousePressed = false;
    bool mouseReleased = false;
    const FontBook* fonts = nullptr;
    float scale = 1.0f;
    Vector2 offset{};
    float logicalHeight = 1000.0f;
    int  draggingSlider = 0;      // 0 = 无
    bool consumedClick = false;   // 本帧点击是否已被某个控件吃掉
    struct DeferredDropdown {
        Rectangle list{};
        std::vector<std::string> items;
        int index = -1;
    };
    std::vector<DeferredDropdown> deferredDropdowns;

    void beginFrame() {
        const Vector2 rawMouse = GetMousePosition();
        mouse = { (rawMouse.x - offset.x) / std::max(1.0f, scale),
                  (rawMouse.y - offset.y) / std::max(1.0f, scale) };
        mousePressed  = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        mouseReleased = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
        mouseDown     = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
        consumedClick = false;
        deferredDropdowns.clear();
        if (mouseReleased) draggingSlider = 0;
    }

    bool hovered(Rectangle r) const { return CheckCollisionPointRec(mouse, r); }
};

namespace widget {

inline bool pointIn(Rectangle r, Vector2 p) { return CheckCollisionPointRec(p, r); }

/// 柔和投影：三层圆角矩形叠出弥散阴影，让面板「浮」在背景上。
inline void softShadow(Rectangle r, float roundness = 0.14f, float offsetY = 5.0f,
                       unsigned char alpha = 42) {
    const float grow[3] = { 2.0f, 5.0f, 9.0f };
    const float fade[3] = { 0.60f, 0.32f, 0.15f };
    for (int i = 2; i >= 0; --i) {
        const float g = grow[i];
        const Rectangle layer{ r.x - g, r.y + offsetY + g * 0.35f, r.width + g * 2.0f, r.height };
        DrawRectangleRounded(layer, roundness, 8,
                             Color{ 0, 0, 0, static_cast<unsigned char>(alpha * fade[i]) });
    }
}

/// 顶部一道高光 + 底部一道内阴影，给平面控件一点细腻的体积感。
inline void bevel(Rectangle r, unsigned char topAlpha = 24, unsigned char bottomAlpha = 28) {
    if (topAlpha > 0)
        DrawRectangleRounded({ r.x + 2.0f, r.y + 1.0f, r.width - 4.0f, 1.0f }, 1.0f, 2,
                             Color{ 255, 255, 255, topAlpha });
    if (bottomAlpha > 0)
        DrawRectangleRounded({ r.x + 2.0f, r.y + r.height - 2.0f, r.width - 4.0f, 1.0f }, 1.0f, 2,
                             Color{ 0, 0, 0, bottomAlpha });
}

inline void panel(Rectangle r, Color fill, Color border, float roundness = 0.14f) {
    DrawRectangleRounded(r, roundness, 8, fill);
    if (border.a > 0) DrawRectangleRoundedLinesEx(r, roundness, 8, 1.0f, border);
}

/// 通用按钮。返回 true 表示本帧被点击。
inline bool button(UiContext& ui, int id, Rectangle r, const char* label,
                   bool primary = false, bool enabled = true, bool active = false) {
    (void)id;
    const bool hover = enabled && ui.hovered(r);
    const bool press = hover && ui.mouseDown;

    Color fill, border, text;
    if (!enabled) {
        fill = Color{ 23, 31, 43, 255 };
        border = Color{ 38, 48, 62, 255 };
        text = Color{ 105, 116, 132, 130 };
    } else if (primary) {
        fill = press ? Color{ 210, 168, 106, 255 }
             : hover ? Color{ 242, 203, 142, 255 } : Color{ 230, 188, 124, 255 };
        border = Color{ 198, 158, 96, 255 };
        text = Color{ 32, 34, 38, 255 };
        // 主按钮下方垫一层暖金光晕，像是被灯光托起来
        DrawRectangleRounded({ r.x, r.y + 3.0f, r.width, r.height }, 0.22f, 8,
                             Color{ 198, 152, 88, static_cast<unsigned char>(hover ? 44 : 26) });
    } else if (active) {
        fill = press ? Color{ 150, 112, 58, 150 } : Color{ 150, 112, 58, 104 };
        border = Color{ 206, 162, 96, 255 };
        text = theme::kGoldLight;
    } else {
        fill = press ? Color{ 19, 28, 41, 255 }
             : hover ? Color{ 37, 52, 74, 255 } : Color{ 23, 33, 48, 255 };
        border = hover ? Color{ 88, 104, 128, 255 } : theme::kLineSoft;
        text = Color{ 222, 230, 240, 255 };
    }

    panel(r, fill, border, 0.22f);
    if (enabled) bevel(r, primary ? 70 : 14, primary ? 30 : 22);
    if (active && enabled) {
        // 激活态：上半部一层薄金色光泽
        DrawRectangleRounded({ r.x + 1.5f, r.y + 1.5f, r.width - 3.0f, r.height * 0.5f }, 0.2f, 6,
                             Color{ 255, 226, 170, 18 });
    }

    const float tw = ui.fonts ? ui.fonts->bodyWidth(label) : static_cast<float>(MeasureText(label, 10));
    const float th = 16.0f;
    const float tx = r.x + (r.width - tw) * 0.5f;
    const float ty = r.y + (r.height - th) * 0.5f + 1.0f;
    if (ui.fonts) ui.fonts->drawBody(label, tx, ty, text);
    else DrawText(label, static_cast<int>(tx), static_cast<int>(ty), 10, text);

    if (!enabled) return false;
    if (hover && ui.mousePressed && !ui.consumedClick) {
        ui.consumedClick = true;
        return true;
    }
    return false;
}

/// 小尺寸按钮（用于八度 ±、视图缩放等）。
inline bool smallButton(UiContext& ui, Rectangle r, const char* label, bool active = false) {
    const bool hover = ui.hovered(r);
    const bool press = hover && ui.mouseDown;
    Color fill = active ? (press ? Color{ 150, 112, 58, 150 } : Color{ 150, 112, 58, 104 })
                        : (press ? Color{ 18, 27, 40, 255 }
                           : hover ? Color{ 35, 50, 72, 255 } : Color{ 20, 30, 44, 255 });
    Color border = active ? Color{ 206, 162, 96, 255 }
                          : (hover ? Color{ 88, 104, 128, 255 } : theme::kLineSoft);
    Color text = active ? theme::kGoldLight : Color{ 224, 232, 242, 255 };
    panel(r, fill, border, 0.3f);
    bevel(r, 12, 20);
    const float tw = ui.fonts ? ui.fonts->bodyWidth(label) : 8.0f;
    const float tx = r.x + (r.width - tw) * 0.5f;
    const float ty = r.y + (r.height - 16.0f) * 0.5f + 1.0f;
    if (ui.fonts) ui.fonts->drawBody(label, tx, ty, text);
    if (hover && ui.mousePressed && !ui.consumedClick) {
        ui.consumedClick = true;
        return true;
    }
    return false;
}

/// 复选开关（画成小方块 + 文字）。
inline bool checkbox(UiContext& ui, Rectangle r, const char* label, bool value) {
    const float box = 15.0f;
    const Rectangle boxRect{ r.x, r.y + (r.height - box) * 0.5f, box, box };
    const bool hover = ui.hovered(r);

    panel(boxRect, value ? Color{ 224, 184, 118, 255 } : Color{ 18, 28, 42, 255 },
          value ? Color{ 246, 210, 150, 255 }
                : (hover ? Color{ 150, 128, 88, 255 } : theme::kLineSoft), 0.3f);
    if (value) {
        bevel(boxRect, 60, 26);
        const Vector2 a{ boxRect.x + 3.5f, boxRect.y + 7.5f };
        const Vector2 b{ boxRect.x + 6.5f, boxRect.y + 10.5f };
        const Vector2 c{ boxRect.x + 11.5f, boxRect.y + 4.0f };
        DrawLineEx(a, b, 2.2f, Color{ 32, 34, 38, 255 });
        DrawLineEx(b, c, 2.2f, Color{ 32, 34, 38, 255 });
    }
    if (ui.fonts) ui.fonts->drawSmall(label, boxRect.x + box + 7.0f,
                                      r.y + (r.height - 13.0f) * 0.5f,
                                      hover ? theme::kInk : theme::kMuted);
    if (hover && ui.mousePressed && !ui.consumedClick) {
        ui.consumedClick = true;
        return true;
    }
    return false;
}

/// 水平滑杆。value 会被就地修改；返回 true 表示本帧值有变化。
inline bool slider(UiContext& ui, int id, Rectangle r, const char* label, const char* valueText,
                   float& value, float lo, float hi) {
    const float labelH = 16.0f;
    const float trackY = r.y + labelH + 10.0f;

    if (ui.fonts && label && *label) {
        ui.fonts->drawSmall(label, r.x, r.y, theme::kMutedDim);
    }
    if (ui.fonts && valueText && *valueText) {
        const float vw = ui.fonts->smallWidth(valueText);
        ui.fonts->drawSmall(valueText, r.x + r.width - vw, r.y, theme::kGold);
    }

    const Rectangle track{ r.x, trackY, r.width, 4.0f };
    const Rectangle hit{ r.x, trackY - 9.0f, r.width, 22.0f };
    const bool hover = ui.hovered(hit);

    // 拖拽中：即使鼠标移出也继续跟随
    if (ui.mousePressed && hover && !ui.consumedClick) {
        ui.draggingSlider = id;
        ui.consumedClick = true;
    }
    const bool dragging = (ui.draggingSlider == id);

    bool changed = false;
    if (dragging) {
        const float t = std::clamp((ui.mouse.x - track.x) / (track.width > 1.0f ? track.width : 1.0f), 0.0f, 1.0f);
        const float nv = lo + t * (hi - lo);
        if (nv != value) { value = nv; changed = true; }
    }

    const float t = (hi > lo) ? std::clamp((value - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f;
    // 内凹轨道：深色底 + 底部一道细高光，像嵌进面板里
    DrawRectangleRounded(track, 1.0f, 6, Color{ 36, 46, 60, 255 });
    DrawRectangleRounded({ track.x + 1.0f, track.y + track.height, track.width - 2.0f, 1.0f },
                         1.0f, 2, Color{ 255, 255, 255, 10 });
    if (t > 0.0f) {
        Rectangle filled{ track.x, track.y, track.width * t, track.height };
        DrawRectangleRounded(filled, 1.0f, 6, theme::kGold);
        DrawRectangleRounded({ filled.x, filled.y, filled.width, 1.5f }, 1.0f, 2,
                             Color{ 255, 230, 180, 110 });
    }
    const float knobX = track.x + track.width * t;
    const float knobY = trackY + 2.0f;
    if (hover || dragging) {
        DrawCircleGradient({ knobX, knobY }, 15.0f,
                           Color{ 215, 173, 104, 42 }, Color{ 215, 173, 104, 0 });
    }
    DrawCircleV({ knobX, knobY + 1.4f }, 6.2f, Color{ 0, 0, 0, 80 });
    DrawCircleV({ knobX, knobY }, (hover || dragging) ? 6.6f : 6.0f,
                (hover || dragging) ? theme::kGoldLight : Color{ 236, 202, 146, 255 });
    DrawCircleV({ knobX, knobY }, 3.6f, Color{ 33, 39, 49, 255 });
    DrawCircleV({ knobX - 1.2f, knobY - 1.4f }, 1.0f, Color{ 255, 255, 255, 110 });
    return changed;
}

/// 下拉选择器。返回 true 表示选择发生了变化。
inline bool dropdown(UiContext& ui, int id, Rectangle r, const std::vector<std::string>& items,
                     int& index, bool& open, int& openId) {
    bool changed = false;
    const bool hover = ui.hovered(r);
    panel(r, hover ? Color{ 26, 38, 55, 255 } : Color{ 19, 29, 43, 255 },
          (open && openId == id) ? theme::kGold
                                 : (hover ? Color{ 88, 104, 128, 255 } : theme::kLineSoft), 0.28f);
    bevel(r, 12, 20);

    const std::string label = (index >= 0 && index < static_cast<int>(items.size())) ? items[index] : "";
    if (ui.fonts) {
        ui.fonts->drawBody(label.c_str(), r.x + 10.0f, r.y + (r.height - 16.0f) * 0.5f + 1.0f, theme::kInk);
        ui.fonts->drawSmall(open ? "▲" : "▼", r.x + r.width - 18.0f, r.y + (r.height - 13.0f) * 0.5f,
                            open ? theme::kGold : theme::kMuted);
    }

    if (hover && ui.mousePressed && !ui.consumedClick) {
        ui.consumedClick = true;
        open = !open || openId != id;
        openId = open ? id : 0;
    }

    if (open && openId == id) {
        const float rowH = 26.0f;
        const float listH = rowH * static_cast<float>(items.size()) + 8.0f;
        Rectangle list{ r.x, r.y + r.height + 4.0f, r.width, listH };
        if (list.y + list.height > ui.logicalHeight - 8.0f)
            list.y = r.y - listH - 4.0f;

        DrawRectangleRounded(list, 0.08f, 6, Color{ 15, 23, 35, 250 });
        DrawRectangleRoundedLinesEx(list, 0.08f, 6, 1.0f, Color{ 62, 76, 96, 255 });

        for (std::size_t i = 0; i < items.size(); ++i) {
            const Rectangle row{ list.x + 4.0f, list.y + 4.0f + rowH * static_cast<float>(i),
                                 list.width - 8.0f, rowH };
            const bool rowHover = ui.hovered(row);
            if (rowHover) {
                DrawRectangleRounded(row, 0.2f, 4, Color{ 213, 173, 106, 22 });
                DrawRectangleRounded({ row.x, row.y + 4.0f, 2.5f, row.height - 8.0f }, 1.0f, 2,
                                     Color{ 213, 173, 106, 190 });
            }
            if (ui.fonts) {
                const bool sel = (static_cast<int>(i) == index);
                ui.fonts->drawSmall(items[i].c_str(), row.x + 8.0f, row.y + 7.0f,
                                    sel ? theme::kGoldLight : theme::kInk);
            }
            if (rowHover && ui.mousePressed) {
                index = static_cast<int>(i);
                changed = true;
                open = false;
                ui.consumedClick = true;
            }
        }
        // 点击列表外关闭
        if (ui.mousePressed && !ui.consumedClick && !pointIn(list, ui.mouse) && !pointIn(r, ui.mouse)) {
            open = false;
            ui.consumedClick = true;
        }
        // 记录一份副本，在整帧其它面板绘制完后再次绘制，确保菜单浮在琴键
        // 和说明卡之上。交互仍在这里处理，避免改变即时模式控件的时序。
        ui.deferredDropdowns.push_back({ list, items, index });
    }
    return changed;
}

inline void drawDeferredDropdowns(UiContext& ui) {
    for (const auto& dd : ui.deferredDropdowns) {
        softShadow(dd.list, 0.08f, 4.0f, 64);
        DrawRectangleRounded(dd.list, 0.08f, 6, Color{ 15, 23, 35, 250 });
        DrawRectangleRoundedLinesEx(dd.list, 0.08f, 6, 1.0f, Color{ 62, 76, 96, 255 });
        constexpr float rowH = 26.0f;
        for (std::size_t i = 0; i < dd.items.size(); ++i) {
            const Rectangle row{ dd.list.x + 4.0f, dd.list.y + 4.0f + rowH * static_cast<float>(i),
                                 dd.list.width - 8.0f, rowH };
            const bool rowHover = ui.hovered(row);
            if (rowHover) {
                DrawRectangleRounded(row, 0.2f, 4, Color{ 213, 173, 106, 22 });
                DrawRectangleRounded({ row.x, row.y + 4.0f, 2.5f, row.height - 8.0f }, 1.0f, 2,
                                     Color{ 213, 173, 106, 190 });
            }
            if (ui.fonts) {
                const bool sel = static_cast<int>(i) == dd.index;
                ui.fonts->drawSmall(dd.items[i].c_str(), row.x + 8.0f, row.y + 7.0f,
                                    sel ? theme::kGoldLight : theme::kInk);
            }
        }
    }
}

inline void text(UiContext& ui, const char* s, float x, float y, Color c, bool small = true) {
    if (!ui.fonts) return;
    if (small) ui.fonts->drawSmall(s, x, y, c);
    else ui.fonts->drawBody(s, x, y, c);
}

inline void textCentered(UiContext& ui, const char* s, Rectangle r, Color c, bool small = true) {
    if (!ui.fonts) return;
    const float w = small ? ui.fonts->smallWidth(s) : ui.fonts->bodyWidth(s);
    const float h = small ? 13.0f : 16.0f;
    if (small) ui.fonts->drawSmall(s, r.x + (r.width - w) * 0.5f, r.y + (r.height - h) * 0.5f, c);
    else       ui.fonts->drawBody(s, r.x + (r.width - w) * 0.5f, r.y + (r.height - h) * 0.5f, c);
}

inline void progressBar(Rectangle r, float t, Color fill, Color back = Color{ 255, 255, 255, 14 }) {
    t = std::clamp(t, 0.0f, 1.0f);
    DrawRectangleRounded(r, 1.0f, 4, back);
    if (t > 0.0f) {
        Rectangle f{ r.x, r.y, r.width * t, r.height };
        DrawRectangleRounded(f, 1.0f, 4, fill);
        DrawRectangleRounded({ f.x, f.y, f.width, 1.0f }, 1.0f, 2, Color{ 255, 255, 255, 50 });
    }
}

inline std::string fmt(const char* pattern, double value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), pattern, value);
    return std::string(buf);
}

} // namespace widget
} // namespace piano
