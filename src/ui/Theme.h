#pragma once
//
// ui/Theme.h —— 配色与布局
//
// 配色取自 HTML 原版的「Grand Studio」暗色金调，保证 C++ 版与网页版视觉一致。
// 布局集中在这里计算，UI 绘制与输入命中测试共用同一份几何，避免两处各算一遍。
//
#include <algorithm>

#include "raylib.h"

namespace piano {

namespace theme {

// ---- 基础色 ----
inline constexpr Color kBg        = {  11,  16,  25, 255 };
inline constexpr Color kBgDeep    = {   7,  11,  18, 255 };
inline constexpr Color kPanel     = {  18,  27,  41, 255 };
inline constexpr Color kPanelTop  = {  27,  38,  55, 255 };
inline constexpr Color kPanelMid  = {  16,  25,  36, 255 };
inline constexpr Color kPanelBot  = {  14,  21,  32, 255 };
inline constexpr Color kLine      = {  43,  54,  71, 255 };
inline constexpr Color kLineSoft  = {  53,  66,  83, 255 };
inline constexpr Color kInk       = { 233, 237, 244, 255 };
inline constexpr Color kMuted     = { 140, 154, 173, 255 };
inline constexpr Color kMutedDim  = { 104, 118, 138, 255 };
inline constexpr Color kGold      = { 215, 173, 104, 255 };
inline constexpr Color kGoldLight = { 246, 210, 150, 255 };
inline constexpr Color kGoldDeep  = { 201, 155,  89, 255 };
inline constexpr Color kGood      = { 152, 187, 150, 255 };
inline constexpr Color kWarn      = { 231, 177, 111, 255 };
inline constexpr Color kBad       = { 238, 119, 119, 255 };

// ---- 琴键 ----
inline constexpr Color kWhiteKeyTop   = { 184, 187, 193, 255 };
inline constexpr Color kWhiteKeyMid   = { 255, 255, 255, 255 };
inline constexpr Color kWhiteKeyBot   = { 191, 196, 202, 255 };
inline constexpr Color kWhiteKeyEdge  = { 159, 163, 170, 255 };
inline constexpr Color kWhiteKeyActive= { 235, 200, 140, 255 };
inline constexpr Color kBlackKeyTop   = {   6,   8,  11, 255 };
inline constexpr Color kBlackKeyMid   = {  45,  51,  62, 255 };
inline constexpr Color kBlackKeyBot   = {   0,   0,   0, 255 };
inline constexpr Color kBlackKeyActive= { 187, 143,  81, 255 };

} // namespace theme

/// 所有面板的矩形区域。UI 绘制与输入命中测试都从这里取坐标。
struct UiLayout {
    float w = 1520.0f;
    float h = 1000.0f;

    Rectangle topbar{};
    Rectangle hero{};
    Rectangle console{};

    Rectangle header{};
    Rectangle controls{};
    Rectangle rail{};
    Rectangle keys{};
    Rectangle bottom{};
    Rectangle transport{};
    Rectangle timeline{};
    Rectangle song{};

    Rectangle guide{};
    Rectangle footer{};

    // 琴键尺寸
    float keyW   = 38.0f;
    float keyH   = 220.0f;
    float blackW = 24.0f;
    float blackH = 139.0f;

    void compute(float width, float height) {
        w = width < 1100.0f ? 1100.0f : width;
        h = height < 880.0f ? 880.0f : height;

        const float margin = 24.0f;
        topbar = { margin, 12.0f, w - margin * 2.0f, 44.0f };
        hero   = { margin, 60.0f, w - margin * 2.0f, 58.0f };

        const float consoleTop    = 140.0f;
        const float contentH      = std::min(h, 1000.0f);
        const float consoleBottom = contentH - 112.0f;
        console = { margin, consoleTop, w - margin * 2.0f, consoleBottom - consoleTop };

        const float pad = 20.0f;
        const float cw  = std::min(console.width - pad * 2.0f, 1440.0f);
        const float x   = console.x + (console.width - cw) * 0.5f;
        float y = console.y + 14.0f;

        header   = { x, y, cw, 38.0f }; y += 46.0f;
        controls = { x, y, cw, 64.0f }; y += 72.0f;
        rail     = { x, y, cw, 26.0f }; y += 30.0f;

        const float below = 34.0f + 50.0f + 12.0f + 24.0f + 150.0f; // 为曲目面板预留空间
        const float avail = console.y + console.height - 14.0f - y - below;
        keyH   = std::clamp(avail, 150.0f, 300.0f);
        blackH = keyH * 0.633f;
        keyW   = 38.0f;
        blackW = keyW * 0.636f;

        keys   = { x, y, cw, keyH };  y += keyH + 8.0f;
        bottom = { x, y, cw, 26.0f }; y += 34.0f;
        transport = { x, y, cw, 42.0f }; y += 50.0f;
        timeline  = { x, y, cw, 4.0f };  y += 12.0f;
        song      = { x, y, cw, std::max(60.0f, console.y + console.height - 14.0f - y) };

        const float lowerW = std::min(w - margin * 2.0f, 1440.0f);
        const float lowerX = margin + (w - margin * 2.0f - lowerW) * 0.5f;
        guide  = { lowerX, consoleBottom + 10.0f, lowerW, 84.0f };
        footer = { lowerX, guide.y + guide.height + 2.0f, lowerW, 16.0f };
    }
};

} // namespace piano
