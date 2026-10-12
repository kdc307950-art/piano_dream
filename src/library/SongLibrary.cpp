#include "library/SongLibrary.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <system_error>

#include "midi/MidiReader.h"

namespace piano {

namespace {

constexpr const char* kIndexHeader  = "PIANO-LIBRARY";
constexpr int         kIndexVersion = 1;

// ---------------------------------------------------------------------------
// 转义：'\' → '\\'，'|' → '\|'，换行 → '\n'
// ---------------------------------------------------------------------------
std::string escapeField(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '|') out += "\\|";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

/// 按未转义的 '|' 切分并就地反转义。字段数不足 10 返回 false。
bool splitLine(const std::string& line, std::vector<std::string>& fields) {
    fields.clear();
    std::string cur;
    bool esc = false;
    for (const char c : line) {
        if (esc) {
            if (c == 'n') cur += '\n';
            else if (c == '\\' || c == '|') cur += c;
            else { cur += '\\'; cur += c; }   // 未知转义原样保留
            esc = false;
        } else if (c == '\\') {
            esc = true;
        } else if (c == '|') {
            fields.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (esc) cur += '\\';
    fields.push_back(cur);
    return fields.size() == 10;
}

bool parseU64Hex(const std::string& s, std::uint64_t& out) {
    if (s.size() != 16) return false;
    std::uint64_t v = 0;
    for (const char c : s) {
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<std::uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint64_t>(c - 'A' + 10);
        else return false;
    }
    out = v;
    return true;
}

std::string toHex64(std::uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

bool parseInt(const std::string& s, long long& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    out = v;
    return true;
}

/// id 只允许 [a-z0-9-]，杜绝路径穿越（标题再脏也进不了文件名）。
bool validId(const std::string& id) {
    if (id.empty() || id.size() > 32) return false;
    for (const char c : id) {
        const bool okc = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!okc) return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// 哈希（FNV-1a 64，字节序无关）
// ---------------------------------------------------------------------------

std::uint64_t SongLibrary::fnv1a64(const void* data, std::size_t size) {
    const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
    std::uint64_t h = 14695981039346656037ull;
    for (std::size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

std::string SongLibrary::sanitizeTitle(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    std::size_t chars = 0;
    for (std::size_t i = 0; i < raw.size() && chars < kMaxTitleLen;) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c < 0x20 || c == 0x7F) { ++i; continue; }        // 去控制字符
        // UTF-8 序列整段保留（按码点计数字符数）
        std::size_t len = 1;
        if (c >= 0xC0 && c < 0xE0) len = 2;
        else if (c >= 0xE0 && c < 0xF0) len = 3;
        else if (c >= 0xF0) len = 4;
        if (i + len > raw.size()) break;                     // 截断的序列丢弃
        out.append(raw, i, len);
        i += len;
        ++chars;
    }
    // trim 两端空白
    const std::size_t b = out.find_first_not_of(' ');
    const std::size_t e = out.find_last_not_of(' ');
    return (b == std::string::npos) ? std::string() : out.substr(b, e - b + 1);
}

bool SongLibrary::isValidUtf8(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }
        std::size_t len;
        unsigned long minCp;
        if (c >= 0xC2 && c <= 0xDF)      { len = 2; minCp = 0x80; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; minCp = 0x800; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; minCp = 0x10000; }
        else return false;                       // 孤立续字节 / 非法起始字节
        if (i + len > s.size()) return false;    // 截断的序列
        unsigned long cp = c & (0x7Fu >> len);
        for (std::size_t k = 1; k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (cp < minCp) return false;                  // overlong
        if (cp >= 0xD800 && cp <= 0xDFFF) return false; // 代理区
        if (cp > 0x10FFFF) return false;
        i += len;
    }
    return true;
}

std::string SongLibrary::pickTitle(const std::string& stemUtf8, const std::string& trackTitle) {
    std::string t = sanitizeTitle(stemUtf8);
    if (!isValidUtf8(t)) t.clear();
    if (!t.empty()) return t;
    t = sanitizeTitle(trackTitle);
    if (!t.empty() && isValidUtf8(t) && t.find("\xEF\xBF\xBD") == std::string::npos)
        return t;
    return "imported";
}

// ---------------------------------------------------------------------------
// 加载
// ---------------------------------------------------------------------------

const SongLibrary::Entry* SongLibrary::find(const std::string& id) const {
    for (const auto& e : entries_)
        if (e.id == id) return &e;
    return nullptr;
}

std::filesystem::path SongLibrary::midiPathFor(const std::string& id) const {
    return dir_ / (id + ".mid");
}

bool SongLibrary::load(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    dir_ = dir;
    entries_.clear();
    totalBytes_ = 0;
    corruptLines_ = 0;
    readOnly_ = false;
    restoredFromBak_ = false;

    std::error_code ec;
    fs::path indexPath = dir_ / "index.txt";
    const fs::path bakPath = dir_ / "index.bak";

    // index.txt 缺失或头损坏而 .bak 存在 → 从 .bak 恢复
    auto readWhole = [](const fs::path& p, std::string& out) {
        std::ifstream in(p, std::ios::binary);
        if (!in) return false;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    };

    std::string text;
    bool haveIndex = readWhole(indexPath, text);
    bool headerOk = haveIndex && text.rfind(kIndexHeader, 0) == 0;
    if (!headerOk && fs::exists(bakPath, ec)) {
        std::string bak;
        if (readWhole(bakPath, bak) && bak.rfind(kIndexHeader, 0) == 0) {
            fs::copy_file(bakPath, indexPath, fs::copy_options::overwrite_existing, ec);
            if (!ec) {
                text = std::move(bak);
                haveIndex = true;
                headerOk = true;
                restoredFromBak_ = true;
            }
        }
    }
    if (!haveIndex) return true;   // 空曲库
    if (!headerOk) {               // 头损坏且无 .bak：视为空（不崩溃）
        ++corruptLines_;
        return true;
    }

    // 头行：版本号 > 1 → 只读（尽力解析条目，一切写操作失败）
    const std::size_t eol = text.find('\n');
    const std::string header = text.substr(0, eol);
    long long version = 0;
    {
        const std::size_t sp = header.find(' ');
        if (sp == std::string::npos || !parseInt(header.substr(sp + 1), version)) {
            ++corruptLines_;
            return true;
        }
    }
    if (version > kIndexVersion) readOnly_ = true;

    std::size_t pos = (eol == std::string::npos) ? text.size() : eol + 1;
    std::vector<std::string> f;
    while (pos < text.size()) {
        const std::size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? nl : nl - pos);
        pos = (nl == std::string::npos) ? text.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        if (!splitLine(line, f)) { ++corruptLines_; continue; }

        Entry e;
        long long v = 0;
        e.id = f[0];
        e.title = f[1];
        e.source = f[2];
        if (!validId(e.id) || !parseU64Hex(f[3], e.hash)) { ++corruptLines_; continue; }
        if (!parseInt(f[4], v)) { ++corruptLines_; continue; }
        e.bpm = static_cast<int>(v);
        e.durationSeconds = std::strtod(f[5].c_str(), nullptr);
        if (!parseInt(f[6], v)) { ++corruptLines_; continue; }
        e.noteCount = static_cast<int>(v);
        if (!parseInt(f[7], v)) { ++corruptLines_; continue; }
        e.melodyTrack = static_cast<int>(v);
        if (!parseInt(f[8], v)) { ++corruptLines_; continue; }
        e.importTimeUnix = v;
        e.favorite = (f[9] == "1");
        e.fileBytes = fs::file_size(midiPathFor(e.id), ec);
        if (ec) e.fileBytes = 0;
        totalBytes_ += e.fileBytes;
        entries_.push_back(std::move(e));
    }
    return true;
}

// ---------------------------------------------------------------------------
// 写入（原子：tmp → rename；覆盖前备份 .bak）
// ---------------------------------------------------------------------------

bool SongLibrary::saveIndex() {
    namespace fs = std::filesystem;
    if (readOnly_) return false;
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec) return false;

    std::string text = std::string(kIndexHeader) + " " + std::to_string(kIndexVersion) + "\n";
    for (const auto& e : entries_) {
        char num[64];
        text += e.id;
        text += '|' + escapeField(e.title);
        text += '|' + escapeField(e.source);
        text += '|' + toHex64(e.hash);
        std::snprintf(num, sizeof(num), "%d", e.bpm);                 text += '|'; text += num;
        std::snprintf(num, sizeof(num), "%.3f", e.durationSeconds);   text += '|'; text += num;
        std::snprintf(num, sizeof(num), "%d", e.noteCount);           text += '|'; text += num;
        std::snprintf(num, sizeof(num), "%d", e.melodyTrack);         text += '|'; text += num;
        std::snprintf(num, sizeof(num), "%lld", static_cast<long long>(e.importTimeUnix));
        text += '|'; text += num;
        text += e.favorite ? "|1\n" : "|0\n";
    }

    const fs::path tmp = dir_ / "index.txt.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << text;
        if (!out) return false;
    }
    const fs::path indexPath = dir_ / "index.txt";
    if (fs::exists(indexPath, ec))
        fs::copy_file(indexPath, dir_ / "index.bak", fs::copy_options::overwrite_existing, ec);
    fs::rename(tmp, indexPath, ec);
    if (ec) {   // Windows：目标存在时 rename 失败，先删再改名（.bak 已兜底）
        ec.clear();
        fs::remove(indexPath, ec);
        fs::rename(tmp, indexPath, ec);
    }
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// 增删改
// ---------------------------------------------------------------------------

bool SongLibrary::readEntryBytes(const std::string& id, std::vector<std::uint8_t>& out) const {
    std::ifstream in(midiPathFor(id), std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

SongLibrary::AddResult SongLibrary::add(const std::filesystem::path& srcFile,
                                        std::optional<int> melodyTrack) {
    namespace fs = std::filesystem;
    AddResult res;
    if (readOnly_) { res.code = AddResult::Code::ReadOnly; return res; }

    std::error_code ec;
    const std::uintmax_t size = fs::file_size(srcFile, ec);
    if (ec) { res.code = AddResult::Code::IoError; res.message = ec.message(); return res; }
    if (size > kMaxFileBytes) { res.code = AddResult::Code::TooLarge; return res; }
    if (static_cast<int>(entries_.size()) >= kMaxSongs) {
        res.code = AddResult::Code::TooManySongs; return res;
    }

    std::vector<std::uint8_t> bytes;
    {
        std::ifstream in(srcFile, std::ios::binary);
        if (!in) { res.code = AddResult::Code::IoError; return res; }
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    MidiParseResult parsed = parseMidi(bytes.data(), bytes.size());
    if (!parsed.ok) {
        res.code = AddResult::Code::ParseFailed;
        res.message = parsed.error;
        return res;
    }
    if (melodyTrack && *melodyTrack >= 0)
        reassignMelodyTrack(parsed, static_cast<std::size_t>(*melodyTrack));
    if (parsed.song.events.size() > kMaxNotesPerSong) {
        res.code = AddResult::Code::TooManyNotes; return res;
    }

    const std::uint64_t hash = fnv1a64(bytes.data(), bytes.size());
    // 同 hash 去重：逐字节比对内容
    for (const auto& e : entries_) {
        if (e.hash != hash) continue;
        std::vector<std::uint8_t> existing;
        if (readEntryBytes(e.id, existing) && existing == bytes) {
            res.code = AddResult::Code::Duplicate;
            res.id = e.id;
            return res;
        }
    }
    if (totalBytes_ + size > kMaxTotalBytes) {
        res.code = AddResult::Code::LibraryFull; return res;
    }

    // id = "u-" + hash；撞哈希且字节不同加 -2/-3… 后缀
    std::string id = "u-" + toHex64(hash);
    if (find(id)) {
        const std::string base = id;
        for (int n = 2;; ++n) {
            id = base + "-" + std::to_string(n);
            if (!find(id)) break;
        }
    }

    // 复制字节到 <id>.mid（tmp + rename）
    fs::create_directories(dir_, ec);
    if (ec) { res.code = AddResult::Code::IoError; res.message = ec.message(); return res; }
    const fs::path dst = midiPathFor(id);
    const fs::path tmp = dir_ / (id + ".mid.tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { res.code = AddResult::Code::IoError; return res; }
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!out) { res.code = AddResult::Code::IoError; return res; }
    }
    fs::rename(tmp, dst, ec);
    if (ec) { fs::remove(tmp, ec); res.code = AddResult::Code::IoError; res.message = ec.message(); return res; }

    Entry e;
    e.id = id;
    // 标题来源规则（A2）：文件名 stem 优先 —— stem().string() 在 Windows 会把
    // 宽字符按 ANSI 窄转换（中文文件名变问号/乱码），必须走 u8string 再按字节拼回。
    {
        const auto stem8 = srcFile.stem().u8string();
        e.title = pickTitle(std::string(reinterpret_cast<const char*>(stem8.data()),
                                        stem8.size()),
                            parsed.song.title);
    }
    e.source = "user-import";
    e.hash = hash;
    e.bpm = parsed.song.bpm;
    e.durationSeconds = parsed.song.durationSeconds();
    e.noteCount = static_cast<int>(parsed.song.events.size());
    e.melodyTrack = melodyTrack.value_or(-1);
    e.importTimeUnix = static_cast<std::int64_t>(std::time(nullptr));
    e.favorite = false;
    e.fileBytes = size;

    entries_.push_back(e);
    totalBytes_ += size;
    if (!saveIndex()) {
        entries_.pop_back();
        totalBytes_ -= size;
        fs::remove(dst, ec);
        res.code = AddResult::Code::IoError;
        return res;
    }

    res.code = AddResult::Code::Ok;
    res.id = id;
    res.noteCount = e.noteCount;
    return res;
}

bool SongLibrary::rename(const std::string& id, const std::string& newTitle) {
    if (readOnly_) return false;
    Entry* e = nullptr;
    for (auto& it : entries_)
        if (it.id == id) { e = &it; break; }
    if (!e) return false;
    const std::string t = sanitizeTitle(newTitle);
    if (t.empty()) return false;
    e->title = t;
    return saveIndex();
}

bool SongLibrary::remove(const std::string& id) {
    if (readOnly_) return false;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].id != id) continue;
        std::error_code ec;
        std::filesystem::remove(midiPathFor(id), ec);
        totalBytes_ -= entries_[i].fileBytes;
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
        return saveIndex();
    }
    return false;
}

bool SongLibrary::setFavorite(const std::string& id, bool favorite) {
    if (readOnly_) return false;
    for (auto& e : entries_) {
        if (e.id != id) continue;
        e.favorite = favorite;
        return saveIndex();
    }
    return false;
}

Song SongLibrary::loadSong(const std::string& id) {
    Song out;
    const Entry* e = find(id);
    if (!e || !validId(id)) return out;

    std::vector<std::uint8_t> bytes;
    if (!readEntryBytes(id, bytes)) return out;
    MidiParseResult parsed = parseMidi(bytes.data(), bytes.size());
    if (!parsed.ok) return out;
    if (e->melodyTrack >= 0)
        reassignMelodyTrack(parsed, static_cast<std::size_t>(e->melodyTrack));

    out = std::move(parsed.song);
    out.id = e->id;
    out.title = e->title;
    out.bpm = e->bpm;
    out.userImport = true;
    return out;
}

std::string SongLibrary::collectTitleChars() const {
    std::string out;
    for (const auto& e : entries_) out += e.title;
    return out;
}

} // namespace piano
