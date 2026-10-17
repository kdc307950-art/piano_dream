//
// main.cpp —— 程序入口
//
//   piano.exe                       启动图形界面
//   piano.exe --selftest            运行离线自测（不开窗口、不需要声卡）
//   piano.exe --screenshot out.png  渲染约 90 帧后截图退出（用于检视界面）
//   piano.exe --fx-demo [dir]       特效演示：固定种子与帧率，输出 5 张截图后退出
//                                   （默认目录 exports/fx-demo/）
//
#include <cstdio>
#include <charconv>
#include <string_view>
#include <string>

#include "App.h"
#include "core/Console.h"

namespace piano {
int runSelfTest(bool verbose);
}

namespace {

void printUsage(const char* exe) {
    std::printf(
        "Grand Studio Piano 1.0\n"
        "用法：%s [选项]\n\n"
        "  --selftest, -t             运行离线自测\n"
        "  --screenshot [文件]        渲染后截图并退出\n"
        "  --capture-play             截图时自动播放第一首曲目\n"
        "  --frames N                 截图帧数（正整数）\n"
        "  --size WxH                 设置窗口尺寸\n"
        "  --width N / --height N     设置窗口宽高\n"
        "  --fx-demo [目录]            输出特效演示截图\n"
        "  --help, -h                 显示帮助\n"
        "  --version                  显示版本\n",
        exe ? exe : "piano");
}

bool parseInt(std::string_view text, int& value) {
    if (text.empty()) return false;
    int parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return false;
    value = parsed;
    return true;
}

bool nextValue(int argc, char** argv, int& index, std::string& value) {
    if (index + 1 >= argc || !argv[index + 1] || argv[index + 1][0] == '-') return false;
    value = argv[++index];
    return !value.empty();
}

} // namespace

int main(int argc, char** argv) {
    // 让 Windows 控制台按 UTF-8 解释我们的输出，否则中文会变成乱码。
    // 退出时自动恢复原代码页（RAII）。
    piano::ConsoleUtf8 consoleUtf8;

    bool selfTest = false;
    bool capture = false;
    bool capturePlay = false;
    bool fxDemo = false;
    int  captureFrames = 90;
    int  windowWidth = 1520;
    int  windowHeight = 1000;
    std::string capturePath = "screenshot.png";
    std::string fxDemoDir = "exports/fx-demo";

    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) continue;
        const std::string a = argv[i];
        if (a == "--selftest" || a == "-t") {
            selfTest = true;
        } else if (a == "--help" || a == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (a == "--version") {
            std::printf("Grand Studio Piano 1.0\n");
            return 0;
        } else if (a == "--screenshot") {
            capture = true;
            if (i + 1 < argc && argv[i + 1] && argv[i + 1][0] != '-') {
                capturePath = argv[++i];
                if (capturePath.empty()) { printUsage(argv[0]); return 2; }
            }
        } else if (a.rfind("--screenshot=", 0) == 0) {
            capture = true;
            capturePath = a.substr(std::string("--screenshot=").size());
            if (capturePath.empty()) { printUsage(argv[0]); return 2; }
        } else if (a == "--fx-demo") {
            fxDemo = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') fxDemoDir = argv[++i];
        } else if (a.rfind("--fx-demo=", 0) == 0) {
            fxDemo = true;
            fxDemoDir = a.substr(std::string("--fx-demo=").size());
            if (fxDemoDir.empty()) { printUsage(argv[0]); return 2; }
        } else if (a == "--capture-play") {
            capturePlay = true;
        } else if (a == "--frames") {
            std::string value;
            if (!nextValue(argc, argv, i, value) || !parseInt(value, captureFrames) || captureFrames <= 0) {
                std::fprintf(stderr, "--frames 需要正整数。\n");
                return 2;
            }
        } else if (a == "--size") {
            std::string size;
            if (!nextValue(argc, argv, i, size)) { printUsage(argv[0]); return 2; }
            const auto separator = size.find('x');
            int parsedWidth = 0;
            int parsedHeight = 0;
            if (separator == std::string::npos || size.find('x', separator + 1) != std::string::npos ||
                !parseInt(size.substr(0, separator), parsedWidth) ||
                !parseInt(size.substr(separator + 1), parsedHeight) ||
                parsedWidth <= 0 || parsedHeight <= 0) {
                std::fprintf(stderr, "--size 需要形如 1520x1000 的正整数尺寸。\n");
                return 2;
            }
            windowWidth = parsedWidth;
            windowHeight = parsedHeight;
        } else if (a == "--width" || a == "--height") {
            std::string value;
            int parsed = 0;
            if (!nextValue(argc, argv, i, value) || !parseInt(value, parsed) || parsed <= 0) {
                std::fprintf(stderr, "%s 需要正整数。\n", a.c_str());
                return 2;
            }
            if (a == "--width") windowWidth = parsed;
            else windowHeight = parsed;
        } else {
            std::fprintf(stderr, "未知参数：%s\n", a.c_str());
            printUsage(argv[0]);
            return 2;
        }
    }

    if (selfTest) return piano::runSelfTest(true);

    piano::App app;
    app.setWindowSize(windowWidth, windowHeight);
    if (fxDemo) app.setFxDemoMode(fxDemoDir);
    std::string log;
    if (!app.init(&log)) {
        std::fprintf(stderr, "初始化失败：\n%s\n", log.c_str());
        return 1;
    }
    std::printf("%s", log.c_str());
    if (!capture && !fxDemo) {
        std::printf("窗口已打开。关闭窗口即退出程序（ESC 已禁用，避免演奏时误触）。\n");
    }
    std::fflush(stdout);

    if (capture && !fxDemo) app.setCaptureMode(captureFrames, capturePath, capturePlay);

    app.run();
    app.shutdown();
    return 0;
}
