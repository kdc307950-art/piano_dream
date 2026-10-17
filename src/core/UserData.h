#pragma once
//
// core/UserData.h —— 用户数据目录（跨平台，header-only，raylib-free）
//
//   Windows : %APPDATA%\GrandStudioPiano
//   macOS   : ~/Library/Application Support/GrandStudioPiano
//             （旧位置 ~/.local/share/GrandStudioPiano 存在且新位置不存在时
//               一次性复制迁移，不删除旧文件；失败静默回落旧位置）
//   Linux   : ~/.local/share/GrandStudioPiano
//
// Windows 下路径一律从宽字符/UTF-8 构造，禁止窄字符串路径。
//
#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall GetEnvironmentVariableW(
    const wchar_t*, wchar_t*, unsigned long);
#endif

namespace piano {

inline std::filesystem::path userDataDirectory() {
#ifdef _WIN32
    // APPDATA is the roaming location used by the desktop build. A few
    // sandboxed or freshly provisioned Windows accounts do not define it, so
    // fall back to LOCALAPPDATA before using a relative directory.
    auto envPath = [](const wchar_t* name) -> std::filesystem::path {
        wchar_t value[32768]{};
        const unsigned long n = GetEnvironmentVariableW(name, value, 32768);
        if (n > 0 && n < 32768) return std::filesystem::path(std::wstring(value, n));
        return {};
    };
    for (const wchar_t* name : { L"APPDATA", L"LOCALAPPDATA" }) {
        const auto base = envPath(name);
        if (!base.empty()) return base / L"GrandStudioPiano";
    }
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) {
        namespace fs = std::filesystem;
        const fs::path legacy = fs::path(home) / ".local" / "share" / "GrandStudioPiano";
        const fs::path modern = fs::path(home) / "Library" / "Application Support" / "GrandStudioPiano";
        std::error_code ec;
        if (fs::exists(legacy, ec) && !fs::exists(modern, ec)) {
            // 一次性迁移：复制而非移动，失败则继续用旧位置。
            // fs::copy 不会创建 modern 的父目录，必须先建（否则首次运行必失败）。
            fs::create_directories(modern.parent_path(), ec);
            ec.clear();
            fs::copy(legacy, modern, fs::copy_options::recursive, ec);
            if (ec) return legacy;
        }
        return modern;
    }
#else
    if (const char* dataHome = std::getenv("XDG_DATA_HOME")) {
        if (*dataHome) return std::filesystem::path(dataHome) / "GrandStudioPiano";
    }
    if (const char* home = std::getenv("HOME"))
        return std::filesystem::path(home) / ".local" / "share" / "GrandStudioPiano";
#endif
    return std::filesystem::path("user-data");
}

/// 用户数据目录下的某个文件（favorites.txt / language.txt / fx.txt / library 等）。
inline std::filesystem::path userDataPath(const char* filename) {
    return userDataDirectory() / filename;
}

/// 旧版本曾把配置写在「当前工作目录」。若 cwd 存在而用户数据目录不存在则复制
/// （不移动不删除）。App::init 早期调用一次。
inline void migrateLegacyUserData() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    if (ec) return;
    for (const char* name : { "favorites.txt", "language.txt", "fx.txt" }) {
        const fs::path legacy = cwd / name;
        const fs::path target = userDataPath(name);
        if (fs::exists(legacy, ec) && !fs::exists(target, ec)) {
            fs::create_directories(target.parent_path(), ec);
            ec.clear();
            fs::copy_file(legacy, target, ec);
        }
    }
}

} // namespace piano
