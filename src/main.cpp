//
// main.cpp —— 程序入口
//
//   piano.exe                       启动图形界面
//   piano.exe --selftest            运行离线自测（不开窗口、不需要声卡）
//   piano.exe --screenshot out.png  渲染约 90 帧后截图退出（用于检视界面）
//
#include <cstdio>
#include <cstdlib>
#include <string>

#include "App.h"
#include "core/Console.h"

namespace piano {
int runSelfTest(bool verbose);
}

int main(int argc, char** argv) {
    // 让 Windows 控制台按 UTF-8 解释我们的输出，否则中文会变成乱码。
    // 退出时自动恢复原代码页（RAII）。
    piano::ConsoleUtf8 consoleUtf8;

    bool selfTest = false;
    bool capture = false;
    bool capturePlay = false;
    int  captureFrames = 90;
    int  windowWidth = 1520;
    int  windowHeight = 1000;
    std::string capturePath = "screenshot.png";

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--selftest" || a == "-t") {
            selfTest = true;
        } else if (a == "--screenshot") {
            capture = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') capturePath = argv[++i];
        } else if (a.rfind("--screenshot=", 0) == 0) {
            capture = true;
            capturePath = a.substr(std::string("--screenshot=").size());
        } else if (a == "--capture-play") {
            capturePlay = true;
        } else if (a == "--frames" && i + 1 < argc) {
            captureFrames = std::atoi(argv[++i]);
        } else if (a == "--size" && i + 1 < argc) {
            const std::string size = argv[++i];
            const auto separator = size.find('x');
            if (separator == std::string::npos) return 2;
            windowWidth = std::atoi(size.substr(0, separator).c_str());
            windowHeight = std::atoi(size.substr(separator + 1).c_str());
        } else if (a == "--width" && i + 1 < argc) {
            windowWidth = std::atoi(argv[++i]);
        } else if (a == "--height" && i + 1 < argc) {
            windowHeight = std::atoi(argv[++i]);
        }
    }

    if (selfTest) return piano::runSelfTest(true);

    piano::App app;
    app.setWindowSize(windowWidth, windowHeight);
    std::string log;
    if (!app.init(&log)) {
        std::fprintf(stderr, "初始化失败：\n%s\n", log.c_str());
        return 1;
    }
    std::printf("%s", log.c_str());
    if (!capture) {
        std::printf("窗口已打开。关闭窗口即退出程序（ESC 已禁用，避免演奏时误触）。\n");
    }
    std::fflush(stdout);

    if (capture) app.setCaptureMode(captureFrames, capturePath, capturePlay);

    app.run();
    app.shutdown();
    return 0;
}
