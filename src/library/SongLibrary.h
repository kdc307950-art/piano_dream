#pragma once
//
// library/SongLibrary.h —— 用户曲库（导入 MIDI 的持久化存储）
//
// 目录：<userDataPath("library")>，每条曲目一个 <id>.mid，索引 index.txt：
//   首行 "PIANO-LIBRARY 1"，之后每行一条，字段 '|' 分隔、'\' 转义（\\ \n \|）：
//   id | title | source | hash(FNV-1a 64, 16hex) | bpm | durationSeconds |
//   noteCount | melodyTrack | importTimeUnix | favorite
//
// 写入安全：index.txt.tmp → rename；覆盖前旧 index.txt 复制为 index.bak；
// 磁盘文件名只用 id。raylib-free，可链进 selftest。
//
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/Song.h"

namespace piano {

class SongLibrary {
public:
    // ---- 限制常量 ----
    static constexpr std::uintmax_t kMaxFileBytes    = 1024 * 1024;          // 单文件 1MB
    static constexpr int            kMaxSongs        = 500;
    static constexpr std::size_t    kMaxNotesPerSong = 200000;
    static constexpr std::uintmax_t kMaxTotalBytes   = 64ull * 1024 * 1024;  // 曲库总容量 64MB
    static constexpr std::size_t    kMaxTitleLen     = 64;                   // 标题字符数（码点）

    struct Entry {
        std::string   id;
        std::string   title;
        std::string   source;             // "user-import"
        std::uint64_t hash = 0;
        int           bpm = kDefaultBpm;
        double        durationSeconds = 0.0;
        int           noteCount = 0;
        int           melodyTrack = -1;   // 手动指定的旋律轨（原始轨号），-1 = 自动
        std::int64_t  importTimeUnix = 0;
        bool          favorite = false;
        std::uintmax_t fileBytes = 0;     // 内存统计用，不写入索引
    };

    struct AddResult {
        enum class Code {
            Ok, Duplicate, TooLarge, TooManySongs, TooManyNotes,
            LibraryFull, ParseFailed, IoError, ReadOnly
        };
        Code        code = Code::Ok;
        std::string id;          // Ok / Duplicate 时有效
        std::string message;     // 补充说明（如解析错误详情）
        int         noteCount = 0;
        bool ok() const { return code == Code::Ok; }
    };

    /// 加载索引（不解析任何 .mid）。目录不存在视为空曲库。
    bool load(const std::filesystem::path& dir);

    const std::vector<Entry>& entries() const { return entries_; }
    const Entry* find(const std::string& id) const;

    AddResult add(const std::filesystem::path& srcFile, std::optional<int> melodyTrack);
    bool rename(const std::string& id, const std::string& newTitle);
    bool remove(const std::string& id);            // 连 <id>.mid 一起删
    bool setFavorite(const std::string& id, bool favorite);

    /// 惰性解析：只在选中/播放时调用。失败返回 empty() 的 Song。
    Song loadSong(const std::string& id);

    bool readOnly() const { return readOnly_; }
    int  corruptLineCount() const { return corruptLines_; }
    bool restoredFromBak() const { return restoredFromBak_; }
    std::uintmax_t totalBytes() const { return totalBytes_; }

    static std::uint64_t fnv1a64(const void* data, std::size_t size);

    /// 全部条目标题拼接（供字体图集收集码点）。
    std::string collectTitleChars() const;

    /// 标题净化：去控制字符、trim、按码点限长。公开以便测试。
    static std::string sanitizeTitle(const std::string& raw);

private:
    bool saveIndex();
    bool readEntryBytes(const std::string& id, std::vector<std::uint8_t>& out) const;
    std::filesystem::path midiPathFor(const std::string& id) const;

    std::filesystem::path dir_;
    std::vector<Entry> entries_;
    std::uintmax_t totalBytes_ = 0;
    int  corruptLines_ = 0;
    bool readOnly_ = false;
    bool restoredFromBak_ = false;
};

/// 收藏裁决（A1.5）：fileIds ∪ 索引中 favorite=1 的条目 id。
/// 顺序：先按 fileIds 原顺序，再按索引顺序追加文件里没有的收藏条目；结果去重。
/// 并集不丢收藏 —— 取消收藏的正常路径会同时写两处，所以不会因合并而复活。
inline std::vector<std::string> mergeFavoriteIds(const std::vector<SongLibrary::Entry>& entries,
                                                 const std::vector<std::string>& fileIds) {
    std::vector<std::string> out;
    for (const auto& id : fileIds)
        if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    for (const auto& e : entries) {
        if (!e.favorite) continue;
        if (std::find(out.begin(), out.end(), e.id) == out.end()) out.push_back(e.id);
    }
    return out;
}

} // namespace piano
