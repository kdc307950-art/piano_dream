#include "ui/FontBook.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <fstream>
#include <iterator>
#include <set>
#include <vector>

namespace piano {
namespace {

constexpr float kTitleSize = 30.0f;
constexpr float kBodySize  = 16.0f;
constexpr float kSmallSize = 12.5f;
constexpr float kTitleSpacing = 0.4f;
constexpr float kBodySpacing  = 0.3f;
constexpr float kSmallSpacing = 0.2f;

/// 候选字体，按优先级排列。中文字体优先，其次各平台的常见 CJK 字体。
const char* const kFontCandidates[] = {
    "C:/Windows/Fonts/msyh.ttc",     // 微软雅黑（Win7+，最通用也最好看）
    "C:/Windows/Fonts/Deng.ttf",     // 等线（Win10+）
    "C:/Windows/Fonts/simhei.ttf",   // 黑体
    "C:/Windows/Fonts/msjh.ttc",     // 微软正黑（繁体环境）
    "C:/Windows/Fonts/simkai.ttf",   // 楷体
    "C:/Windows/Fonts/simsun.ttc",   // 宋体
    "/System/Library/Fonts/PingFang.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
};

// ---- 大端读写 ----
std::uint16_t readBE16(const unsigned char* p) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}
std::uint32_t readBE32(const unsigned char* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8)  |
            static_cast<std::uint32_t>(p[3]);
}
void writeBE32(std::vector<unsigned char>& v, std::size_t at, std::uint32_t x) {
    v[at + 0] = static_cast<unsigned char>((x >> 24) & 0xFF);
    v[at + 1] = static_cast<unsigned char>((x >> 16) & 0xFF);
    v[at + 2] = static_cast<unsigned char>((x >> 8) & 0xFF);
    v[at + 3] = static_cast<unsigned char>(x & 0xFF);
}

/// 把 TrueType Collection（.ttc）里的第一个字体抽出成一个独立的 sfnt。
///
/// 为什么需要这个：raylib 的 LoadFontFromMemory 内部固定调用
/// `stbtt_InitFont(&info, fileData, 0)`，即永远从文件偏移 0 开始解析。
/// 而 .ttc 的文件头是 'ttcf'，真正的字体目录在别处 —— 于是解析失败。
/// 更麻烦的是**它不报错**：字体对象看起来有效，只是所有非 ASCII 字形
/// 全部退化成 '?'（中文界面直接变成一排问号）。
///
/// 这里按 sfnt 规范重新布局：12 字节头 + 16×numTables 表记录 + 各表数据（4 字节对齐），
/// 表偏移全部重算。返回空表示不是 TTC 或结构异常。
std::vector<unsigned char> unwrapTtcFirstFont(const std::vector<unsigned char>& in) {
    if (in.size() < 16) return {};
    if (!(in[0] == 't' && in[1] == 't' && in[2] == 'c' && in[3] == 'f')) return {};

    const std::uint32_t numFonts = readBE32(in.data() + 8);
    if (numFonts == 0) return {};
    const std::uint32_t dirOff = readBE32(in.data() + 12);
    if (static_cast<std::size_t>(dirOff) + 12 > in.size()) return {};

    const std::uint16_t numTables = readBE16(in.data() + dirOff + 4);
    if (numTables == 0) return {};
    if (static_cast<std::size_t>(dirOff) + 12 + static_cast<std::size_t>(numTables) * 16 > in.size())
        return {};

    struct Rec { std::uint32_t tag, checkSum, offset, length; };
    std::vector<Rec> recs(numTables);
    for (std::uint16_t i = 0; i < numTables; ++i) {
        const unsigned char* p = in.data() + dirOff + 12 + static_cast<std::size_t>(i) * 16;
        recs[i].tag      = readBE32(p);
        recs[i].checkSum = readBE32(p + 4);
        recs[i].offset   = readBE32(p + 8);
        recs[i].length   = readBE32(p + 12);
        if (static_cast<std::size_t>(recs[i].offset) + recs[i].length > in.size()) return {};
    }

    const std::uint32_t headerSize = 12u + static_cast<std::uint32_t>(numTables) * 16u;
    std::uint32_t cursor = headerSize;
    std::vector<std::uint32_t> newOffset(numTables);
    for (std::uint16_t i = 0; i < numTables; ++i) {
        newOffset[i] = cursor;
        cursor += recs[i].length;
        cursor = (cursor + 3u) & ~3u;      // 表数据 4 字节对齐
    }

    std::vector<unsigned char> out(cursor, 0);
    std::memcpy(out.data(), in.data() + dirOff, 12);   // sfntVersion / numTables / searchRange 等
    for (std::uint16_t i = 0; i < numTables; ++i) {
        const std::size_t at = 12 + static_cast<std::size_t>(i) * 16;
        writeBE32(out, at,      recs[i].tag);
        writeBE32(out, at + 4,  recs[i].checkSum);
        writeBE32(out, at + 8,  newOffset[i]);
        writeBE32(out, at + 12, recs[i].length);
        std::memcpy(out.data() + newOffset[i], in.data() + recs[i].offset, recs[i].length);
    }
    return out;
}

bool readFileBytes(const char* path, std::vector<unsigned char>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !out.empty();
}

} // namespace

void FontBook::collectCodepoints(const std::string& utf8, std::vector<int>& out) {
    const char* p = utf8.c_str();
    int size = 0;
    for (;;) {
        const int cp = GetCodepointNext(p, &size);
        if (cp == 0 || size <= 0) break;
        // 控制字符（尤其是文案里的换行）不需要进入字体图集，也不应被
        // 统计为“字体缺少的码点”。真正绘制时换行由调用方拆分处理。
        if (cp >= 0x20) out.push_back(cp);
        p += size;
    }
}

bool FontBook::load(const std::vector<std::string>& samples, float rasterScale) {
    unload();

    // 1) 收集界面实际用到的全部码点
    std::set<int> unique;
    for (const auto& s : samples) {
        std::vector<int> cps;
        collectCodepoints(s, cps);
        unique.insert(cps.begin(), cps.end());
    }
    for (int c = 32; c < 127; ++c) unique.insert(c);   // ASCII 可见字符
    // 补充符号：只列「界面真的会画」且「实测微软雅黑确实含有」的。
    // 不要往这里加 ▶ ♪ ♫ ⌨ ✦ 之类的符号 —— 中文字体普遍没有，加了只会让
    // 启动日志里多出一串「不存在的码点」，渲染时也仍然是 '?'。
    for (int c : { 0x00B7,  // ·
                   0x00D7,  // ×
                   0x2014,  // —
                   0x2192,  // →
                   0x2193,  // ↓
                   0x2212,  // −
                   0x25A0,  // ■
                   0x25B2,  // ▲
                   0x25BC,  // ▼
                   0x25CE,  // ◎
                   0x25CF,  // ●
                   0x2605,  // ★
                   0x2606 } // ☆
    ) {
        unique.insert(c);
    }

    std::vector<int> codepoints(unique.begin(), unique.end());
    loadedCodepoints_ = codepoints;

    // 2) 依次尝试候选字体
    for (const char* path : kFontCandidates) {
        if (!FileExists(path)) continue;

        std::vector<unsigned char> raw;
        if (!readFileBytes(path, raw)) continue;

        // .ttc 先拆包；拆完一律按 .ttf 交给 raylib
        std::vector<unsigned char> unwrapped = unwrapTtcFirstFont(raw);
        const bool isTtc = !unwrapped.empty();
        const unsigned char* data = isTtc ? unwrapped.data() : raw.data();
        const int dataSize = static_cast<int>(isTtc ? unwrapped.size() : raw.size());
        if (dataSize <= 0) continue;

        const Font t = LoadFontFromMemory(".ttf", data, dataSize,
                                          static_cast<int>(std::lround(kTitleSize * rasterScale)),
                                          codepoints.data(), static_cast<int>(codepoints.size()));

        // 关键校验：raylib 只会为「字体里确实存在」的码点生成字形，其余静默丢弃
        // （渲染时退化成 '?'）。更坑的是：加载失败时 raylib 会**悄悄返回内置默认字体**
        // （baseSize 10 / 95 个 ASCII 字形），IsFontValid 依然为真 ——
        // 所以不能只看 IsFontValid，必须校验字形覆盖率。
        const Font def = GetFontDefault();
        const int wanted = static_cast<int>(codepoints.size());
        const bool isDefaultFallback = (t.texture.id == def.texture.id);
        const int found = IsFontValid(t) ? t.glyphCount : 0;

        std::printf("[font] 尝试 %s -> 有效=%d 字形=%d 覆盖率=%d%%\n",
                    path, IsFontValid(t) ? 1 : 0, found,
                    wanted > 0 ? found * 100 / wanted : 0);

        if (!IsFontValid(t) || isDefaultFallback || found * 10 < wanted * 9) {
            // 默认字体是 raylib 的全局资源，绝对不能 Unload
            if (!isDefaultFallback && IsFontValid(t)) UnloadFont(t);
            continue;
        }

        title_ = t;
        body_  = LoadFontFromMemory(".ttf", data, dataSize, static_cast<int>(std::lround(kBodySize * rasterScale)),
                                    codepoints.data(), static_cast<int>(codepoints.size()));
        small_ = LoadFontFromMemory(".ttf", data, dataSize, static_cast<int>(std::lround(kSmallSize * rasterScale)),
                                    codepoints.data(), static_cast<int>(codepoints.size()));
        if (!IsFontValid(body_) || !IsFontValid(small_) ||
            body_.texture.id == def.texture.id || small_.texture.id == def.texture.id) {
            if (IsFontValid(title_) && title_.texture.id != def.texture.id) UnloadFont(title_);
            if (IsFontValid(body_) && body_.texture.id != def.texture.id)   UnloadFont(body_);
            if (IsFontValid(small_) && small_.texture.id != def.texture.id) UnloadFont(small_);
            title_ = body_ = small_ = {};
            continue;
        }

        SetTextureFilter(title_.texture, TEXTURE_FILTER_BILINEAR);
        SetTextureFilter(body_.texture, TEXTURE_FILTER_BILINEAR);
        SetTextureFilter(small_.texture, TEXTURE_FILTER_BILINEAR);

        sourcePath_ = path;
        usingSystemFont_ = true;
        loaded_ = true;
        glyphCount_ = wanted;
        resolvedGlyphs_ = found;
        return true;
    }

    // 3) 兜底：raylib 默认字体（中文会显示为 '?'）
    title_ = GetFontDefault();
    body_  = GetFontDefault();
    small_ = GetFontDefault();
    sourcePath_ = "(raylib 默认字体)";
    usingSystemFont_ = false;
    loaded_ = true;
    glyphCount_ = 224;
    resolvedGlyphs_ = 224;
    return true;
}

void FontBook::unload() {
    if (!loaded_) return;
    if (usingSystemFont_) {
        UnloadFont(title_);
        UnloadFont(body_);
        UnloadFont(small_);
    }
    title_ = body_ = small_ = {};
    loaded_ = false;
    usingSystemFont_ = false;
    sourcePath_.clear();
    glyphCount_ = 0;
    resolvedGlyphs_ = 0;
}

void FontBook::drawTitle(const char* text, float x, float y, Color color) const {
    DrawTextEx(title_, text, { x, y }, kTitleSize, kTitleSpacing, color);
}
void FontBook::drawBody(const char* text, float x, float y, Color color) const {
    DrawTextEx(body_, text, { x, y }, kBodySize, kBodySpacing, color);
}
void FontBook::drawSmall(const char* text, float x, float y, Color color) const {
    DrawTextEx(small_, text, { x, y }, kSmallSize, kSmallSpacing, color);
}

float FontBook::titleWidth(const char* text) const {
    return MeasureTextEx(title_, text, kTitleSize, kTitleSpacing).x;
}
float FontBook::bodyWidth(const char* text) const {
    return MeasureTextEx(body_, text, kBodySize, kBodySpacing).x;
}
float FontBook::smallWidth(const char* text) const {
    return MeasureTextEx(small_, text, kSmallSize, kSmallSpacing).x;
}

bool FontBook::canRender(const std::string& utf8) const {
    if (!usingSystemFont_) return true;   // 默认字体不做校验
    std::vector<int> cps;
    collectCodepoints(utf8, cps);
    for (int cp : cps) {
        if (cp < 0x20) continue;
        if (!std::binary_search(loadedCodepoints_.begin(), loadedCodepoints_.end(), cp))
            return false;
    }
    return true;
}

std::vector<int> FontBook::missingCodepoints() const {
    std::vector<int> missing;
    if (!usingSystemFont_ || title_.glyphs == nullptr || title_.glyphCount <= 0) return missing;

    std::set<int> have;
    for (int i = 0; i < title_.glyphCount; ++i) have.insert(title_.glyphs[i].value);
    for (int cp : loadedCodepoints_)
        if (have.find(cp) == have.end()) missing.push_back(cp);
    return missing;
}

} // namespace piano
