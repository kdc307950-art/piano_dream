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
    int  draggingSlider = 0;      // 0 = 无
    bool consumedClick = false;   // 本帧点击是否已被某个控件吃掉

    void beginFrame() {
        const Vector2 rawMouse = GetMousePosition();
        mouse = { (rawMouse.x - offset.x) / std::max(1.0f, scale),
                  (rawMouse.y - offset.y) / std::max(1.0f, scale) };
        mousePressed  = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        mouseReleased = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
        mouseDown     = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
        consumedClick = false;
        if (mouseReleased) draggingSlider = 0;
    }

    bool hovered(Rectangle r) const { return CheckCollisionPointRec(mouse, r); }
};

namespace widget {

inline bool pointIn(Rectangle r, Vector2 p) { return CheckCollisionPointRec(p, r); }

inline void panel(Rectangle r, Color fill, Color border, float roundness = 0.14f) {
    DrawRectangleRounded(r, roundness, 8, fill);
    if (border.a > 0) DrawRectangleRoundedLinesEx(r, roundness, 8, 1.0f, border);
}

/// 通用按钮。返回 true 表示本帧被点击。
inline bool button(UiContext& ui, int id, Rectangle r, const char* label,
                   bool primary = false, bool enabled = true, bool active = false) {
    (void)id;
    const bool hover = enabled && ui.hovered(r);

    Color fill, border, text;
    if (!enabled) {
        fill = Color{ 26, 35, 48, 255 };
        border = theme::kLine;
        text = Color{ 110, 122, 140, 140 };
    } else if (primary) {
        fill = hover ? Color{ 241, 204, 146, 255 } : Color{ 232, 191, 130, 255 };
        border = Color{ 222, 192, 139, 255 };
        text = Color{ 25, 32, 43, 255 };
    } else if (active) {
        fill = Color{ 121, 90, 47, 120 };
        border = Color{ 184, 141, 83, 255 };
        text = theme::kGoldLight;
    } else {
        fill = hover ? Color{ 36, 50, 71, 255 } : Color{ 24, 35, 51, 255 };
        border = theme::kLineSoft;
        text = Color{ 219, 227, 236, 255 };
    }

    panel(r, fill, border, 0.22f);

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
    Color fill = active ? Color{ 121, 90, 47, 120 } : (hover ? Color{ 36, 50, 71, 255 } : Color{ 22, 33, 48, 255 });
    Color border = active ? Color{ 184, 141, 83, 255 } : theme::kLineSoft;
    Color text = active ? theme::kGoldLight : Color{ 222, 231, 241, 255 };
    panel(r, fill, border, 0.3f);
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

    panel(boxRect, value ? Color{ 215, 173, 104, 255 } : Color{ 21, 31, 46, 255 },
          value ? Color{ 246, 210, 150, 255 } : theme::kLineSoft, 0.3f);
    if (value) {
        const Vector2 a{ boxRect.x + 3.5f, boxRect.y + 7.5f };
        const Vector2 b{ boxRect.x + 6.5f, boxRect.y + 10.5f };
        const Vector2 c{ boxRect.x + 11.5f, boxRect.y + 4.0f };
        DrawLineEx(a, b, 2.0f, Color{ 25, 32, 43, 255 });
        DrawLineEx(b, c, 2.0f, Color{ 25, 32, 43, 255 });
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
    DrawRectangleRounded(track, 1.0f, 6, Color{ 52, 67, 87, 255 });
    if (t > 0.0f) {
        Rectangle filled{ track.x, track.y, track.width * t, track.height };
        DrawRectangleRounded(filled, 1.0f, 6, theme::kGold);
    }
    const float knobX = track.x + track.width * t;
    DrawCircleV({ knobX, trackY + 2.0f }, hover || dragging ? 7.0f : 6.0f,
                (hover || dragging) ? theme::kGoldLight : Color{ 240, 208, 160, 255 });
    DrawCircleV({ knobX, trackY + 2.0f }, 2.4f, Color{ 23, 29, 39, 255 });
    return changed;
}

/// 下拉选择器。返回 true 表示选择发生了变化。
inline bool dropdown(UiContext& ui, int id, Rectangle r, const std::vector<std::string>& items,
                     int& index, bool& open, int& openId) {
    bool changed = false;
    const bool hover = ui.hovered(r);
    panel(r, hover ? Color{ 25, 37, 53, 255 } : Color{ 21, 31, 46, 255 }, theme::kLineSoft, 0.28f);

    const std::string label = (index >= 0 && index < static_cast<int>(items.size())) ? items[index] : "";
    if (ui.fonts) {
        ui.fonts->drawBody(label.c_str(), r.x + 10.0f, r.y + (r.height - 16.0f) * 0.5f + 1.0f, theme::kInk);
        ui.fonts->drawSmall(open ? "▲" : "▼", r.x + r.width - 18.0f, r.y + (r.height - 13.0f) * 0.5f, theme::kMuted);
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
        if (list.y + list.height > static_cast<float>(GetScreenHeight()) - 8.0f)
            list.y = r.y - listH - 4.0f;

        DrawRectangleRounded(list, 0.08f, 6, Color{ 17, 26, 39, 252 });
        DrawRectangleRoundedLinesEx(list, 0.08f, 6, 1.0f, theme::kLineSoft);

        for (std::size_t i = 0; i < items.size(); ++i) {
            const Rectangle row{ list.x + 4.0f, list.y + 4.0f + rowH * static_cast<float>(i),
                                 list.width - 8.0f, rowH };
            const bool rowHover = ui.hovered(row);
            if (rowHover) DrawRectangleRounded(row, 0.2f, 4, Color{ 36, 50, 71, 255 });
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
    }
    return changed;
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

inline void progressBar(Rectangle r, float t, Color fill, Color back = Color{ 255, 255, 255, 18 }) {
    t = std::clamp(t, 0.0f, 1.0f);
    DrawRectangleRounded(r, 1.0f, 4, back);
    if (t > 0.0f) {
        Rectangle f{ r.x, r.y, r.width * t, r.height };
        DrawRectangleRounded(f, 1.0f, 4, fill);
    }
}

inline std::string fmt(const char* pattern, double value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), pattern, value);
    return std::string(buf);
}

} // namespace widget
} // namespace piano
