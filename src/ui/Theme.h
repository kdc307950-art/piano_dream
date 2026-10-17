#pragma once
//
// ui/Theme.h —— 配色与布局
//
// 配色取自 HTML 原版的「Grand Studio」暗色金调，保证 C++ 版与网页版视觉一致。
// 布局集中在这里计算，UI 绘制与输入命中测试共用同一份几何，避免两处各算一遍。
//
#include <algorithm>

#include "raylib.h"

#include "fx/FxTokens.h"

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
inline constexpr Color kMuted     = { 154, 165, 181, 255 };
inline constexpr Color kMutedDim  = { 154, 165, 181, 255 };
inline constexpr Color kGold      = { 215, 173, 104, 255 };
inline constexpr Color kGoldLight = { 246, 210, 150, 255 };
inline constexpr Color kGoldDeep  = { 201, 155,  89, 255 };
inline constexpr Color kGood      = { 152, 187, 150, 255 };
inline constexpr Color kWarn      = { 231, 177, 111, 255 };
inline constexpr Color kBad       = { 238, 119, 119, 255 };

// ---- 琴键 ----
// 白键取象牙暖白（真琴键不是冷白），黑键取漆面深蓝黑并留出高光余地。
inline constexpr Color kWhiteKeyTop   = { 253, 251, 246, 255 };
inline constexpr Color kWhiteKeyMid   = { 246, 243, 237, 255 };
inline constexpr Color kWhiteKeyBot   = { 230, 226, 218, 255 };
inline constexpr Color kWhiteKeyEdge  = { 172, 166, 156, 255 };
inline constexpr Color kWhiteKeyActive= { 250, 228, 180, 255 };
inline constexpr Color kBlackKeyTop   = {  62,  68,  80, 255 };
inline constexpr Color kBlackKeyMid   = {  30,  34,  42, 255 };
inline constexpr Color kBlackKeyBot   = {   8,  10,  14, 255 };
inline constexpr Color kBlackKeyActive= { 178, 130,  68, 255 };

} // namespace theme

/// 所有面板的矩形区域。UI 绘制与输入命中测试都从这里取坐标。
struct UiLayout {
    float w = 1520.0f;
    float h = 1000.0f;

    Rectangle topbar{};
    Rectangle hero{};
    Rectangle console{};

    Rectangle header{};
    Rectangle library{};    // 左侧曲库栏
    Rectangle heroPanel{};  // 中央品牌舞台
    Rectangle controls{};
    Rectangle rail{};
    Rectangle keys{};
    Rectangle fxStage{};   // 「按键飘出音符」舞台带（键盘上方），fxEnabled=false 时为空
    Rectangle bottom{};
    Rectangle transport{};
    Rectangle timeline{};
    Rectangle song{};

    Rectangle guide{};
    Rectangle footer{};

    // 琴键尺寸
    float keyW   = 26.0f;
    float keyH   = 220.0f;
    float blackW = 24.0f;
    float blackH = 139.0f;

    bool fxEnabled = false;   // 特效舞台带开关；关闭时布局与引入本特性前逐像素一致

    void compute(float width, float height) {
        w = width < 1100.0f ? 1100.0f : width;
        h = height < 880.0f ? 880.0f : height;

        // 逻辑画布保持 1520×1000，方便高 DPI 与截图模式复用；布局结构
        // 按设计稿重排为：顶部品牌栏 → 曲库/舞台/控制三栏 → 键盘 → 运输栏 → 谱面。
        const float margin = 14.0f;
        topbar = { margin, 10.0f, w - margin * 2.0f, 58.0f };
        hero   = { margin, 72.0f, w - margin * 2.0f, 8.0f };

        const float consoleTop    = 84.0f;
        const float consoleBottom = h - 52.0f;
        console = { margin, consoleTop, w - margin * 2.0f, consoleBottom - consoleTop };

        const float x = console.x;
        const float cw = console.width;
        const float topRowH = 356.0f;
        const float gap = 8.0f;
        const float leftW = std::clamp(cw * 0.30f, 300.0f, 460.0f);
        const float rightW = std::clamp(cw * 0.255f, 300.0f, 410.0f);
        const float middleW = cw - leftW - rightW - gap * 2.0f;

        library   = { x, consoleTop, leftW, topRowH };
        heroPanel = { x + leftW + gap, consoleTop, middleW, topRowH };
        controls  = { heroPanel.x + heroPanel.width + gap, consoleTop, rightW, topRowH };
        header    = heroPanel;

        float y = consoleTop + topRowH + 8.0f;
        rail = { x, y, cw, 20.0f };
        y += 28.0f;

        // 特效在设计稿中表现为舞台底部的金色粒子带，不再额外挤占键盘高度。
        // 这样键盘、运输栏和谱面在不同窗口尺寸下保持稳定位置。
        fxStage = fxEnabled ? Rectangle{ x, y - 26.0f, cw, 26.0f } : Rectangle{};

        keyH   = 154.0f;
        blackH = keyH * 0.633f;
        keyW   = std::clamp(cw / 52.0f, 14.0f, 32.0f);
        blackW = keyW * 0.636f;
        keys   = { x, y, cw, keyH };  y += keyH + 8.0f;
        bottom = { x, y, cw, 24.0f }; y += 30.0f;
        transport = { x, y, cw, 88.0f }; y += 96.0f;
        timeline  = { x, y, cw, 4.0f };  y += 12.0f;
        song      = { x, y, cw, std::max(150.0f, consoleBottom - y) };

        guide  = { x, consoleBottom + 8.0f, cw, 36.0f };
        footer = { x, guide.y + guide.height + 2.0f, cw, 12.0f };
    }
};

} // namespace piano
