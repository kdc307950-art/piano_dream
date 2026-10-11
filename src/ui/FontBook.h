#pragma once
//
// ui/FontBook.h —— 中文字体加载
//
// raylib 自带的默认字体只有 ASCII，中文会显示成 '?'。
// 这里从系统字体目录加载一款中文字体，并且只把「界面上真正用到的字符」
// 收集成码点表交给 LoadFontEx —— 否则整个 CJK 区（两万多字）会把字形图集撑爆。
//
#include <string>
#include <vector>

#include "raylib.h"

namespace piano {

class FontBook {
public:
    /// 依据传入的所有界面字符串收集码点并加载字体。
    bool load(const std::vector<std::string>& samples, float rasterScale = 1.0f);
    void unload();

    const Font& title() const noexcept { return title_; }
    const Font& body() const noexcept { return body_; }
    const Font& small() const noexcept { return small_; }

    bool usingSystemFont() const noexcept { return usingSystemFont_; }
    const std::string& sourcePath() const noexcept { return sourcePath_; }
    /// 请求的字形数（界面用到的码点总数）
    int glyphCount() const noexcept { return glyphCount_; }
    /// 字体里真正找到的字形数 —— 明显小于 glyphCount 说明这个字体不含所需的中文字形
    int resolvedGlyphs() const noexcept { return resolvedGlyphs_; }

    /// 判断一段文本能否被当前字形图集完整渲染（用于动态来源的标题，例如导入的 MIDI）。
    bool canRender(const std::string& utf8) const;

    /// 请求了但字体里找不到的码点（渲染时会退化成 '?'）。用于排查界面上的问号。
    std::vector<int> missingCodepoints() const;

    /// 带字间距的绘制封装。
    void drawTitle(const char* text, float x, float y, Color color) const;
    void drawBody(const char* text, float x, float y, Color color) const;
    void drawSmall(const char* text, float x, float y, Color color) const;

    float titleWidth(const char* text) const;
    float bodyWidth(const char* text) const;
    float smallWidth(const char* text) const;

private:
    static void collectCodepoints(const std::string& utf8, std::vector<int>& out);

    Font title_{};
    Font body_{};
    Font small_{};
    bool loaded_ = false;
    bool usingSystemFont_ = false;
    std::string sourcePath_;
    int glyphCount_ = 0;
    int resolvedGlyphs_ = 0;
    std::vector<int> loadedCodepoints_;   // 升序，用于 canRender 二分查找
};

} // namespace piano
