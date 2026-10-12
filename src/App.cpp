#include "App.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
constexpr unsigned long kMaxWindowsPath = 32768;
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(
    void*, wchar_t*, unsigned long);
extern "C" __declspec(dllimport) int __stdcall SetCurrentDirectoryW(const wchar_t*);
extern "C" __declspec(dllimport) unsigned long __stdcall GetEnvironmentVariableW(
    const wchar_t*, wchar_t*, unsigned long);
#else
#include <unistd.h>
#endif

#include "core/Note.h"
#include "fx/FxSettings.h"
#include "midi/MidiReader.h"
#include "midi/MidiWriter.h"
#include "songs/BuiltinSongs.h"
#include "ui/UiStrings.h"
#include "ui/GeneratedGlyphs.h"

namespace piano {
namespace {

using namespace ui;

constexpr int   kScopePoints   = 150;
constexpr float kScopeInterval = 0.04f;

std::string num(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return std::string(buf);
}

std::string percent(float v) {
    return std::to_string(static_cast<int>(std::lround(v * 100.0f))) + "%";
}

void anchorWorkingDirectoryToExecutable() {
#ifdef _WIN32
    wchar_t path[kMaxWindowsPath]{};
    const unsigned long n = GetModuleFileNameW(nullptr, path, kMaxWindowsPath);
    if (n == 0 || n >= kMaxWindowsPath) return;
    for (unsigned long i = n; i > 0; --i) {
        if (path[i - 1] == L'\\' || path[i - 1] == L'/') {
            path[i - 1] = L'\0';
            SetCurrentDirectoryW(path);
            return;
        }
    }
#else
    const char* dir = GetApplicationDirectory();
    if (dir && *dir) chdir(dir);
#endif
}

std::filesystem::path userDataDirectory() {
#ifdef _WIN32
    wchar_t appData[32768]{};
    const unsigned long n = GetEnvironmentVariableW(L"APPDATA", appData, 32768);
    if (n > 0 && n < 32768)
        return std::filesystem::path(std::wstring(appData, n)) / L"GrandStudioPiano";
#else
    if (const char* home = std::getenv("HOME"))
        return std::filesystem::path(home) / ".local" / "share" / "GrandStudioPiano";
#endif
    return std::filesystem::path("user-data");
}

/// 用户数据目录下的某个文件（fx.txt / favorites.txt / language.txt 统一走这里）。
std::filesystem::path userDataPath(const char* filename) {
    return userDataDirectory() / filename;
}

} // namespace

// ===========================================================================
// 生命周期
// ===========================================================================

bool App::init(std::string* log) {
    anchorWorkingDirectoryToExecutable();
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
#ifdef __APPLE__
    SetConfigFlags(FLAG_WINDOW_HIGHDPI);
#endif
    InitWindow(windowWidth_, windowHeight_, ui::t(ui::kWindowTitle));
    SetWindowMinSize(1100, 880);
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);          // 禁用 ESC 退出，避免演奏时误触

    dpiScale_ = GetWindowScaleDPI().x;
    viewScale_ = std::clamp(std::min(static_cast<float>(GetScreenWidth()) / 1520.0f,
                                     static_cast<float>(GetScreenHeight()) / 1000.0f),
                                     0.65f, 2.5f);
    viewOffset_ = { (static_cast<float>(GetScreenWidth()) - 1520.0f * viewScale_) * 0.5f,
                    (static_cast<float>(GetScreenHeight()) - 1000.0f * viewScale_) * 0.5f };
    ui_.scale = viewScale_;
    ui_.offset = viewOffset_;
    ui_.logicalHeight = 1000.0f;
    sampleLog_ += "[layout] framebuffer " + std::to_string(GetScreenWidth()) + "x" +
                  std::to_string(GetScreenHeight()) + " · dpi " + std::to_string(dpiScale_) + "\n";
    // ---- 特效（布局计算需要知道舞台带是否开启）----
    noteFx_.seedRng(0x5EEDu);
    noteFx_.setAliveCapScale(1.0f);   // 自适应降级初始满额
    if (fxDemo_) {
        fxLevel_ = 2;                 // 演示固定 Standard，保证截图可复现
        fxReducedMotion_ = false;
    } else {
        loadFxSettings();
    }
    noteFx_.setLevel(static_cast<fx::Level>(fxLevel_));
    noteFx_.setReducedMotion(fxReducedMotion_);
    layout_.fxEnabled = fxLevel_ != 0;
    layout_.compute(1520.0f, 1000.0f);
    sampleLog_ += "[fx] level=" + std::to_string(fxLevel_) +
                  " reducedMotion=" + (fxReducedMotion_ ? "1" : "0") + "\n";

    loadLanguage();

    // ---- 曲目先建好，曲名也要进入字形图集 ----
    songs_ = builtinSongs();
    std::vector<std::string> fontSamples = ui::allStrings();
    for (const auto& s : songs_) {
        fontSamples.push_back(s.title);
        fontSamples.push_back(s.composer);
    }
    fontSamples.push_back("导入的 MIDI");
    for (const char* s : { "合成引擎", "次谐波", "组真实采样", "个采样锚点", "点击琴键，开始演奏",
                           "无法打开文件", "文件过小，不是有效的 MIDI 文件", "缺少 MThd 头，不是标准 MIDI 文件",
                           "MThd 块长度非法", "MThd 内容不完整", "暂不支持 SMPTE 时间码格式的 MIDI 文件",
                           "文件中没有可播放的音符事件", "音频设备初始化失败", "音频设备尚未打开",
                           "音频设备启动失败", "导出失败", "导入失败", "导入成功", "个音符",
                           "跟练中不能修改速度", "跟练中不能修改移调" })
        fontSamples.emplace_back(s);
    for (std::size_t i = 0; i < ui_generated::kSourceStringCount; ++i)
        fontSamples.emplace_back(ui_generated::kSourceStrings[i]);
    fontSamples_ = fontSamples;
    fontScale_ = std::max(1.0f, static_cast<float>(GetRenderWidth()) / GetScreenWidth()) * viewScale_;
    fonts_.load(fontSamples_, fontScale_);
    ui_.fonts = &fonts_;

    // ---- 采样音色（可选） ----
    // 只用「纯 ASCII 的相对路径」去找采样，刻意不用 GetApplicationDirectory()：
    //   raylib 返回的 exe 路径是 ANSI(GBK) 字节，而 libstdc++ 的
    //   std::filesystem::path 会把它当 UTF-8 解 —— 工程路径一旦含中文就抛
    //   "Cannot convert character sequence: Illegal byte sequence"。
    //   相对路径由操作系统解析，不经过这层转换，安全。
    //   成品 piano.exe 就在工程根目录，双击时工作目录就是那里，
    //   所以 "assets/samples" 直接命中；"../assets/samples" 覆盖 exe 放在子目录的情况。
    int loadedSamples = 0;
    for (const char* dir : { "assets/samples", "../assets/samples" }) {
        std::string probe;
        const int n = samples_.loadDirectory(dir, &probe);
        if (n > 0) {
            loadedSamples = n;
            sampleLog_ += probe;
            break;
        }
    }

    // ---- 输入 ----
    keyboardMap_.setBindings(kDefaultBindings.data(), kDefaultBindings.size());
    input_.setKeyboardMap(&keyboardMap_);
    input_.setSink(this);
    input_.setHitTest([this](float x, float y, int& midi, float& vel) {
        if (dropdownOpen_) return false;
        return pianoView_.hitTest((x - viewOffset_.x) / viewScale_,
                                  (y - viewOffset_.y) / viewScale_, midi, vel);
    });
    pianoView_.setKeyboardMap(&keyboardMap_);
    pianoView_.setLayout(layout_);
    pianoView_.setViewTransform(viewScale_, viewOffset_);
    pianoView_.centerOn(60, false);

    // ---- 音频 ----
    mixer_.setSampleBank(&samples_);
    mixer_.setVisualEventQueue(&fxQueue_);   // 设备启动前挂上视觉事件队列
    noteFx_.setStageHeight(layout_.fxStage.height);
    fxAtlasScale_ = fontScale_;
    fxLayer_.bake(fxAtlasScale_);
    std::string err;
    if (device_.open(mixer_, &App::schedulerEntry, this, &err)) {
        sampleRate_ = device_.sampleRate();
        mixer_.setMasterVolume(volume_);
        mixer_.setReverbMix(reverb_);
        metronome_.setSampleRate(sampleRate_);
        metronome_.setBpm(static_cast<int>(std::lround(bpmF_)));
        judge_.configure(sampleRate_);
        if (device_.start(&err)) {
            audioReady_ = true;
            setStatus(ui::t(ui::kStatusAudioOk));
        } else {
            audioError_ = err;
            setStatus(ui::t(ui::kStatusNoAudio), true);
        }
    } else {
        audioError_ = err;
        setStatus(ui::t(ui::kStatusNoAudio), true);
    }

    scope_.assign(kScopePoints, 0.0f);

    loadFavorites();
    refreshSongList();
    if (!songs_.empty()) selectSong(0);

    if (loadedSamples == 0) toast(ui::t(ui::kSampleNone));

    if (log) {
        *log += sampleLog_;
        if (!audioReady_) *log += "[audio] " + audioError_ + "\n";
        *log += "[font] " + fonts_.sourcePath() + " · 请求 " + std::to_string(fonts_.glyphCount()) +
                " 字形，命中 " + std::to_string(fonts_.resolvedGlyphs()) + "\n";
        {
            const std::vector<int> missing = fonts_.missingCodepoints();
            if (!missing.empty()) {
                *log += "[font] 字体中不存在的码点（界面会显示为 ?）：";
                for (int cp : missing) {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "U+%04X ", cp);
                    *log += buf;
                }
                *log += "\n";
            }
        }
        if (audioReady_) {
            *log += "[audio] " + std::to_string(static_cast<int>(sampleRate_)) + " Hz · 每块 " +
                    std::to_string(device_.periodFrames()) + " 帧\n";
        }
        *log += "[songs] " + std::to_string(songs_.size()) + " 首内置曲目\n";
        std::ofstream logFile("piano.log", std::ios::trunc);
        if (logFile) logFile << *log;
    }
    return true;
}

void App::setCaptureMode(int frames, std::string path, bool autoPlaySong) {
    autoExitFrames_  = frames > 0 ? frames : 90;
    screenshotPath_  = std::move(path);
    captureAutoPlay_ = autoPlaySong;
    // 质量门禁截图默认无特效、无舞台带（--fx-demo 例外，它自己管等级）
    if (!fxDemo_) noteFx_.setLevel(fx::Level::Off);
}

void App::setFxDemoMode(std::string dir) {
    fxDemo_ = true;
    fxDemoDir_ = dir.empty() ? "exports/fx-demo" : std::move(dir);
}

void App::setWindowSize(int width, int height) noexcept {
    windowWidth_ = std::clamp(width, 1100, 3840);
    windowHeight_ = std::clamp(height, 880, 2160);
}

void App::run() {
    if (captureAutoPlay_ && !songs_.empty() && !fxDemo_) startSongPlayback();

    // --fx-demo 截图时间点：t = 0.1 / 0.3 / 0.6 / 1.2 / 1.8s（帧 6/18/36/72/108）
    static constexpr int         kDemoShotFrames[5] = { 6, 18, 36, 72, 108 };
    static constexpr const char* kDemoShotNames[5]  = {
        "fx_t0.1s.png", "fx_t0.3s.png", "fx_t0.6s.png", "fx_t1.2s.png", "fx_t1.8s.png"
    };

    int frame = 0;
    for (;;) {
        if (WindowShouldClose()) {
            // 关闭窗口没有可取消的 raylib 事件，因此先自动收尾并导出，避免录音丢失。
            if (recording_) {
                stopRecording();
                exportRecordingMidi();
            }
            break;
        }
        // demo 时钟 = 帧号/60，不取 GetFrameTime，保证截图逐位可复现
        const float dt = fxDemo_ ? (1.0f / 60.0f) : GetFrameTime();
        ui_.beginFrame();
        if (fxDemo_) runFxDemoScript(frame);
        handleInput(dt);
        updateFx(dt);
        draw(dt);

        if (fxDemo_) {
            if (!fxDemoDirReady_) {
                std::error_code ec;
                std::filesystem::create_directories(fxDemoDir_, ec);
                fxDemoDirReady_ = true;
            }
            for (int i = 0; i < 5; ++i) {
                if (frame == kDemoShotFrames[i]) {
                    const std::string path = fxDemoDir_ + "/" + kDemoShotNames[i];
                    TakeScreenshot(path.c_str());
                    std::printf("[fx-demo] 已保存截图：%s\n", path.c_str());
                    std::fflush(stdout);
                }
            }
            if (++frame > 112) break;   // 帧 112 后退出
            continue;
        }

        if (autoExitFrames_ > 0 && ++frame >= autoExitFrames_) {
            if (!screenshotPath_.empty()) {
                TakeScreenshot(screenshotPath_.c_str());
                std::printf("[capture] 已保存截图：%s\n", screenshotPath_.c_str());
                std::fflush(stdout);
            }
            break;
        }
    }
}

void App::shutdown() {
    player_.stop();
    recordedPlayer_.stop();
    device_.pushReleaseSource(Source::Playback);
    device_.pushReleaseSource(Source::Song);
    metronome_.setEnabled(false);
    device_.stop();
    mixer_.setVisualEventQueue(nullptr);   // 设备停止后摘除视觉事件队列
    fxLayer_.unload();
    fonts_.unload();
    if (IsWindowReady()) CloseWindow();
}

// ===========================================================================
// 音频线程调度：Player + RecordedPlayer + Metronome 串接
// ===========================================================================

std::uint32_t App::schedulerEntry(void* user, Mixer& mixer,
                                  std::uint64_t blockStartFrame, std::uint32_t maxFrames) {
    return static_cast<App*>(user)->scheduler(mixer, blockStartFrame, maxFrames);
}

std::uint32_t App::scheduler(Mixer& mixer, std::uint64_t blockStartFrame, std::uint32_t maxFrames) {
    const std::uint32_t a = Player::process(&player_, mixer, blockStartFrame, maxFrames);
    const std::uint32_t b = RecordedPlayer::process(&recordedPlayer_, mixer, blockStartFrame, maxFrames);
    const std::uint32_t c = Metronome::process(&metronome_, mixer, blockStartFrame, maxFrames);
    const std::uint32_t r = std::min({ a, b, c });
    return r < 1 ? 1 : r;
}

// ===========================================================================
// InputSink
// ===========================================================================

void App::onNoteOn(int midi, float velocity) {
    const std::uint64_t frame = mixer_.currentFrame();
    device_.pushNoteOn(midi, velocity, Source::User);

    if (recording_) {
        recorder_.capture(frame, MsgType::NoteOn, midi,
                          static_cast<int>(std::lround(velocity * 127.0f)));
    }
    if (judge_.active()) {
        judge_.onUserNoteOn(midi, frame);
    }
}

void App::onNoteOff(int midi) {
    const std::uint64_t frame = mixer_.currentFrame();
    device_.pushNoteOff(midi, Source::User);
    if (recording_) recorder_.capture(frame, MsgType::NoteOff, midi, 0);
}

void App::onSustainChanged(bool on) {
    sustainUser_ = on;
    if (recording_) recorder_.capture(mixer_.currentFrame(), MsgType::Sustain, 0, on ? 127 : 0);
    pushSustainIfChanged();
}

void App::pushSustainIfChanged() {
    if (playingRecording_) return;   // 回放期间由录制事件掌管踏板
    if (sustainUser_ == sustainEffective_) return;
    sustainEffective_ = sustainUser_;
    device_.pushSustain(sustainEffective_);
}

// ===========================================================================
// 音符粒子：排空音频线程的视觉事件，推进粒子时钟
// ===========================================================================

void App::updateFx(float dt) {
    const auto t0 = std::chrono::steady_clock::now();

    noteFx_.setStageHeight(layout_.fxStage.height);
    const double sr = sampleRate_ > 1000.0 ? sampleRate_ : 48000.0;
    VisualEvent ev;
    int accepted = 0;
    while (fxQueue_.pop(ev)) {
        // 每帧最多取 kPerFrameSpawnMax 个有效事件，其余丢弃不顺延
        if (accepted >= theme::fx::kPerFrameSpawnMax) continue;
        const std::uint64_t frame = mixer_.currentFrame();
        if (frame < ev.audioFrame) continue;
        const double age = static_cast<double>(frame - ev.audioFrame) / sr;
        if (age > static_cast<double>(theme::fx::kStaleEventSeconds)) continue;
        // 键不在可视视口内则不生成（事件消耗掉，不入队顺延）
        float kx = 0.0f, ky = 0.0f;
        if (!pianoView_.keyTopCenter(ev.midi, kx, ky)) continue;
        noteFx_.ingest(ev.midi, ev.velocity, static_cast<std::uint8_t>(ev.source),
                       noteFx_.now() - age);
        ++accepted;
    }
    noteFx_.update(static_cast<double>(dt));

    fxFrameCostMs_ += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
}

// ---------------------------------------------------------------------------
// --fx-demo 演示脚本：按 demo 时钟直接注入 noteFx_，不经过音频设备/队列
// ---------------------------------------------------------------------------
namespace {

struct FxDemoEvent { float t; std::uint8_t midi; float velocity; Source source; };

// 来源配色演示：单音=User（金）、和弦=Song（主文字色）、滑音=Playback（主文字色）
constexpr FxDemoEvent kFxDemoScript[] = {
    { 0.00f, 24, 0.55f, Source::User }, { 0.00f, 96, 0.55f, Source::User },
    { 0.20f, 48, 0.95f, Source::Song }, { 0.20f, 52, 0.95f, Source::Song },
    { 0.20f, 55, 0.95f, Source::Song }, { 0.20f, 60, 0.95f, Source::Song },
    { 0.50f, 62, 0.70f, Source::Playback }, { 0.53f, 64, 0.70f, Source::Playback },
    { 0.56f, 66, 0.70f, Source::Playback }, { 0.59f, 68, 0.70f, Source::Playback },
    { 0.62f, 70, 0.70f, Source::Playback }, { 0.65f, 72, 0.70f, Source::Playback },
    { 0.68f, 74, 0.70f, Source::Playback }, { 0.71f, 76, 0.70f, Source::Playback },
};

} // namespace

void App::runFxDemoScript(int frame) {
    const double now = static_cast<double>(frame) / 60.0;
    constexpr int kCount = static_cast<int>(sizeof(kFxDemoScript) / sizeof(kFxDemoScript[0]));
    while (fxDemoCursor_ < kCount &&
           static_cast<double>(kFxDemoScript[fxDemoCursor_].t) <= now + 1e-9) {
        const FxDemoEvent& e = kFxDemoScript[fxDemoCursor_++];
        noteFx_.ingest(e.midi, e.velocity, static_cast<std::uint8_t>(e.source),
                       static_cast<double>(e.t));
    }
}

// ===========================================================================
// 输入处理
// ===========================================================================

void App::handleInput(float dt) {
    // 搜索框焦点决定电脑键盘是否用于演奏
    input_.setKeyboardEnabled(!searchFocused_ && !dropdownOpen_);

    input_.update();
    pianoView_.updateScrollAnimation(dt);

    // ---- 滚轮滚动琴键视口 ----
    const Vector2 mouse = ui_.mouse;
    if (CheckCollisionPointRec(mouse, layout_.keys)) {
        const float wheel = GetMouseWheelMove();
        if (std::fabs(wheel) > 0.001f) pianoView_.scrollBy(-wheel * 90.0f);
    }

    // ---- 拖动「琴盖」区域滚动（与 HTML 原版一致）----
    if (!railDragging_ && ui_.mousePressed && CheckCollisionPointRec(mouse, layout_.rail)) {
        railDragging_ = true;
        railLastX_ = mouse.x;
        ui_.consumedClick = true;
    }
    if (railDragging_) {
        if (ui_.mouseDown) {
            pianoView_.scrollBy(-(mouse.x - railLastX_) * 2.0f);
            railLastX_ = mouse.x;
        } else {
            railDragging_ = false;
        }
    }

    // ---- 拖入 .mid 文件 ----
    if (IsFileDropped()) {
        const FilePathList files = LoadDroppedFiles();
        for (unsigned i = 0; i < files.count; ++i) importMidiFile(files.paths[i]);
        UnloadDroppedFiles(files);
    }

    // ---- 波形采样 ----
    scopeTimer_ += dt;
    if (scopeTimer_ >= kScopeInterval) {
        scopeTimer_ = 0.0f;
        if (!scope_.empty()) {
            std::rotate(scope_.begin(), scope_.begin() + 1, scope_.end());
            scope_.back() = mixer_.peakLevel();
        }
    }

    // ---- 提示条计时 ----
    if (toastTimer_ > 0.0f) toastTimer_ = std::max(0.0f, toastTimer_ - dt);

    // ---- 演奏器 / 回放结束检测 ----
    if (songPlaying_ && !player_.playing()) {
        songPlaying_ = false;
        if (judge_.active()) judge_.finish(mixer_.currentFrame());
        setStatus(ui::t(ui::kSongIdle));
    }
    if (playingRecording_ && !recordedPlayer_.playing()) {
        playingRecording_ = false;
        device_.pushReleaseSource(Source::Playback);
        sustainEffective_ = !sustainUser_;
        pushSustainIfChanged();
        setStatus(ui::t(ui::kToastPlayDone));
    }
    if (songPlaying_) {
        judge_.update(mixer_.currentFrame());
        if (followKeyboard_) {
            // 找到当前正在发声的最高音，只在它快要移出视域时才滚动，避免来回抖动
            int focus = -1;
            for (int m = kMidiHigh; m >= kMidiLow; --m) {
                if (mixer_.visualCount(m) > 0) { focus = m; break; }
            }
            if (focus >= 0) {
                const float x = pianoView_.xOfNote(focus);
                const float left = pianoView_.scroll();
                const float right = left + layout_.keys.width;
                if (x < left + layout_.keyW * 2.0f || x > right - layout_.keyW * 3.0f)
                    pianoView_.centerOn(focus, true);
            }
        }
    }

    // ---- 搜索框文本输入 ----
    if (searchFocused_) {
        int c = 0;
        while ((c = GetCharPressed()) > 0) {
            if (c >= 32 && c < 127 && searchText_.size() < 24)
                searchText_.push_back(static_cast<char>(c));
        }
        if (IsKeyPressed(KEY_BACKSPACE) && !searchText_.empty()) searchText_.pop_back();
        if (IsKeyPressed(KEY_ESCAPE)) searchFocused_ = false;
        if (IsKeyPressed(KEY_ENTER)) {
            searchFocused_ = false;
            refreshSongList();
        }
    }
}

// ===========================================================================
// 绘制
// ===========================================================================

void App::draw(float dt) {
    // 使用实际 framebuffer 比例，避免将系统 DPI 再乘进逻辑坐标。
    const float rasterScale = std::max(1.0f, static_cast<float>(GetRenderWidth()) / GetScreenWidth()) * viewScale_;
    if (std::fabs(rasterScale - fontScale_) > 0.01f) {
        fontScaleTimer_ += dt;
        if (fontScaleTimer_ >= 0.15f) {
            fonts_.load(fontSamples_, rasterScale);
            fontScale_ = rasterScale;
            fontScaleTimer_ = 0.0f;
        }
    } else fontScaleTimer_ = 0.0f;
    // 音符图集的重烘焙与字体同一防抖策略：变化 > 0.01 且稳定 0.15s
    if (std::fabs(rasterScale - fxAtlasScale_) > 0.01f) {
        fxAtlasTimer_ += dt;
        if (fxAtlasTimer_ >= 0.15f) {
            fxLayer_.bake(rasterScale);
            fxAtlasScale_ = rasterScale;
            fxAtlasTimer_ = 0.0f;
        }
    } else fxAtlasTimer_ = 0.0f;
    const float nextScale = std::clamp(std::min(static_cast<float>(GetScreenWidth()) / 1520.0f,
                                                static_cast<float>(GetScreenHeight()) / 1000.0f),
                                       0.65f, 2.5f);
    viewScale_ = nextScale;
    viewOffset_ = { (static_cast<float>(GetScreenWidth()) - 1520.0f * viewScale_) * 0.5f,
                    (static_cast<float>(GetScreenHeight()) - 1000.0f * viewScale_) * 0.5f };
    ui_.scale = viewScale_;
    ui_.offset = viewOffset_;
    layout_.fxEnabled = noteFx_.level() != fx::Level::Off;
    layout_.compute(1520.0f, 1000.0f);
    pianoView_.setLayout(layout_);
    pianoView_.setViewTransform(viewScale_, viewOffset_);
    fxLayer_.setViewTransform(viewScale_, viewOffset_);

    // 窗口尺寸或 DPI/缩放变化：粒子坐标全部失效，清空重来（保留种子与配置），
    // 同时让降级统计忽略接下来的 kDegradeIgnoreFrames 帧（重烘焙/重排布会虚高耗时）
    if (prevScreenW_ != 0 &&
        (GetScreenWidth() != prevScreenW_ || GetScreenHeight() != prevScreenH_ ||
         std::fabs(viewScale_ - prevViewScale_) > 1e-4f)) {
        noteFx_.clear();
        fxIgnoreFrames_ = theme::fx::kDegradeIgnoreFrames;
    }
    prevScreenW_ = GetScreenWidth();
    prevScreenH_ = GetScreenHeight();
    prevViewScale_ = viewScale_;

    BeginDrawing();
    ClearBackground(theme::kBg);
    Camera2D uiCamera{};
    uiCamera.offset = viewOffset_;
    uiCamera.zoom = viewScale_;
    BeginMode2D(uiCamera);
    // 背景：顶部暖金灯光 + 右下冷蓝反光 + 左下一点紫调，底部轻轻压暗（暗角）
    DrawCircleGradient({ layout_.w * 0.5f, -200.0f }, 1050.0f,
                       Color{ 92, 70, 38, 62 }, Color{ 10, 14, 22, 0 });
    DrawCircleGradient({ layout_.w * 1.04f, layout_.h * 0.80f }, 560.0f,
                       Color{ 34, 52, 80, 54 }, Color{ 10, 14, 22, 0 });
    DrawCircleGradient({ layout_.w * -0.05f, layout_.h * 0.55f }, 480.0f,
                       Color{ 48, 40, 66, 34 }, Color{ 10, 14, 22, 0 });
    DrawRectangleGradientV(0, static_cast<int>(layout_.h * 0.62f),
                           static_cast<int>(layout_.w), static_cast<int>(layout_.h * 0.38f),
                           Color{ 0, 0, 0, 0 }, Color{ 0, 0, 0, 60 });

    drawTopbar();
    drawHero();
    drawConsole();
    {
        // 特效绘制提交计入降级预算
        const auto t0 = std::chrono::steady_clock::now();
        fxLayer_.draw(noteFx_, pianoView_, layout_);   // 特效：琴键之后、下拉菜单之前
        fxFrameCostMs_ += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    }
    drawGuide();
    drawFooter();
    widget::drawDeferredDropdowns(ui_);
    drawOverlays();

    EndMode2D();
    EndDrawing();

    // ---- 自适应降级：连续 kDegradeFrames 帧超预算 → 存活上限减半（当次会话不回升）----
    // 忽略：dt > kDtMaxSeconds 的帧（卡顿/拖拽）、视图变化后的 kDegradeIgnoreFrames 帧、
    // 演示与截图模式。
    if (!fxDemo_ && autoExitFrames_ == 0) {
        if (fxIgnoreFrames_ > 0) {
            --fxIgnoreFrames_;
        } else if (dt <= static_cast<float>(theme::fx::kDtMaxSeconds)) {
            if (fxFrameCostMs_ > static_cast<double>(theme::fx::kBudgetMs)) {
                if (!fxDegraded_ && ++fxOverBudgetFrames_ >= theme::fx::kDegradeFrames) {
                    fxDegraded_ = true;
                    fxOverBudgetFrames_ = 0;
                    noteFx_.setAliveCapScale(0.5f);
                    std::printf("[fx] over budget %.2fms for %d frames, alive cap halved\n",
                                fxFrameCostMs_, theme::fx::kDegradeFrames);
                    std::fflush(stdout);
                }
            } else {
                fxOverBudgetFrames_ = 0;
            }
        }
    }
    fxFrameCostMs_ = 0.0;
}

void App::drawTopbar() {
    const Rectangle& r = layout_.topbar;

    // 徽标：深色铜牌上五条高低错落的金线，像声波也像琴键
    const Rectangle logo{ r.x, r.y + 3.0f, 40.0f, 38.0f };
    widget::softShadow(logo, 0.24f, 3.0f, 40);
    widget::panel(logo, Color{ 46, 36, 24, 255 }, Color{ 148, 124, 84, 255 }, 0.24f);
    widget::bevel(logo, 26, 30);
    const float barHeights[5] = { 10.0f, 17.0f, 13.0f, 19.0f, 11.0f };
    for (int i = 0; i < 5; ++i) {
        const float x = logo.x + 8.0f + static_cast<float>(i) * 6.0f;
        const float half = barHeights[i] * 0.5f;
        const float cy = logo.y + logo.height * 0.5f;
        DrawLineEx({ x, cy - half }, { x, cy + half }, 2.4f,
                   (i % 2 == 0) ? theme::kGold : theme::kGoldLight);
    }

    fonts_.drawSmall(ui::t(ui::kBrandTitle), logo.x + 52.0f, logo.y + 4.0f, theme::kInk);
    fonts_.drawSmall(ui::t(ui::kBrandSub), logo.x + 52.0f, logo.y + 20.0f, theme::kMutedDim);

    // 状态胶囊：玻璃质感 + 指示灯带一圈光晕
    const std::string st = statusText_;
    const float tw = fonts_.smallWidth(st.c_str());
    const float pillW = tw + 46.0f;
    const Rectangle pill{ r.x + r.width - pillW, r.y + 5.0f, pillW, 32.0f };
    widget::panel(pill, Color{ 15, 23, 35, 220 }, Color{ 58, 72, 92, 255 }, 0.5f);
    widget::bevel(pill, 18, 20);
    const Vector2 dot{ pill.x + 16.0f, pill.y + pill.height * 0.5f };
    const Color dotColor = statusWarn_ ? theme::kWarn : theme::kGood;
    DrawCircleGradient(dot, 9.0f, Color{ dotColor.r, dotColor.g, dotColor.b, 100 },
                       Color{ dotColor.r, dotColor.g, dotColor.b, 0 });
    DrawCircleV(dot, 3.5f, dotColor);
    fonts_.drawSmall(st.c_str(), pill.x + 27.0f, pill.y + 9.0f, Color{ 184, 196, 210, 255 });

    const Rectangle languageButton{ pill.x - 68.0f, pill.y, 58.0f, pill.height };
    const char* languageLabel = ui::language() == ui::Language::Chinese ? "EN" : "中文";
    if (widget::button(ui_, 9000, languageButton, languageLabel, false, true)) {
        ui::setLanguage(ui::language() == ui::Language::Chinese
                            ? ui::Language::English : ui::Language::Chinese);
        saveLanguage();
        setStatus(ui::t(ui::kStatusAudioOk));
    }
}

void App::drawHero() {
    const Rectangle& r = layout_.hero;

    // 常态头部保持一行，把纵向空间留给琴键；大标题只在空状态提示中显示。
    fonts_.drawSmall(ui::t(ui::kEyebrow), r.x, r.y + 4.0f, theme::kGold);
    const float eyebrowW = fonts_.smallWidth(ui::t(ui::kEyebrow));
    fonts_.drawSmall(ui::t(ui::kHeroDesc), r.x + eyebrowW + 18.0f, r.y + 4.0f,
                     Color{ 158, 172, 189, 255 });

    // 六段真实电平表：历史峰值来自音频线程的 atomic peakLevel()。
    const Rectangle meter{ r.x + r.width - 220.0f, r.y + 5.0f, 220.0f, 14.0f };
    fonts_.drawSmall(ui::t(ui::kLiveSound), meter.x - 70.0f, meter.y + 1.0f, theme::kMuted);
    for (int i = 0; i < 6; ++i) {
        const std::size_t sample = scope_.empty() ? 0 :
            std::min(scope_.size() - 1, scope_.size() - 1 - static_cast<std::size_t>(i * 4));
        const float level = scope_.empty() ? mixer_.peakLevel() : std::clamp(scope_[sample], 0.0f, 1.0f);
        const float x = meter.x + static_cast<float>(i) * 35.0f;
        const Color c = i >= 5 ? theme::kWarn : (i >= 4 ? theme::kGold : theme::kGood);
        const bool lit = level > static_cast<float>(i + 1) / 6.0f;
        const Rectangle seg{ x, meter.y, 29.0f, meter.height };
        DrawRectangleRounded(seg, 0.35f, 4, lit ? c : Color{ 38, 50, 66, 255 });
        if (lit) DrawRectangleRounded({ seg.x, seg.y, seg.width, seg.height * 0.45f }, 0.35f, 4,
                                      Color{ 255, 255, 255, 34 });
    }
}

void App::drawFxStage() {
    const Rectangle& st = layout_.fxStage;
    if (st.height <= 0.0f) return;
    // 舞台带：控制台面板的一部分，用既有深色令牌，画在琴键之前
    widget::panel(st, theme::kBgDeep, theme::kLine, 0.06f);
    DrawRectangleRounded({ st.x + 12.0f, st.y + st.height - 1.5f, st.width - 24.0f, 1.0f },
                         1.0f, 2, Color{ 213, 173, 106, 28 });
}

void App::drawConsole() {
    const Rectangle& r = layout_.console;
    widget::softShadow(r, 0.035f, 8.0f, 66);
    widget::panel(r, theme::kPanel, theme::kLineSoft, 0.035f);
    // 顶部高光：一丝暖色，像灯光落在漆面琴身上
    DrawRectangleRounded({ r.x + 18.0f, r.y, r.width - 36.0f, 1.0f }, 1.0f, 4,
                         Color{ 236, 220, 190, 22 });

    // 头部
    fonts_.drawBody(ui::t(ui::kInstrument), layout_.header.x, layout_.header.y, theme::kInk);
    fonts_.drawSmall(ui::t(ui::kInstrumentSub), layout_.header.x, layout_.header.y + 22.0f, theme::kMutedDim);

    const std::string badge = samples_.empty()
        ? std::string(ui::t(ui::kSynthBadge))
        : ("● " + std::to_string(samples_.size()) + " 组真实采样");
    const float bw = fonts_.smallWidth(badge.c_str());
    const Rectangle bp{ layout_.header.x + layout_.header.width - bw - 28.0f,
                        layout_.header.y + 3.0f, bw + 24.0f, 28.0f };
    widget::panel(bp, Color{ 202, 162, 107, 14 }, Color{ 150, 118, 70, 120 }, 0.5f);
    fonts_.drawSmall(badge.c_str(), bp.x + 12.0f, bp.y + 7.0f, Color{ 236, 206, 152, 255 });

    drawFxStage();
    drawKeyboardSection();
    drawTransportRow();
    drawSongPanel();
    // 下拉列表必须在琴键、运输栏和曲目面板之后绘制，避免被后续内容盖住。
    drawControlsRow();
}

void App::drawControlsRow() {
    const Rectangle& r = layout_.controls;
    const bool compact = r.width < 1200.0f;
    const float y0 = r.y;
    const float ctrlY = y0 + 20.0f;

    // 横向预算：为特效档位与「减少动态效果」压缩了既有控件的宽度与间距，功能不变。
    const float engineW = compact ? 150.0f : 190.0f;
    const float sliderW = compact ? 150.0f : 190.0f;
    float cx = r.x;

    // 音色引擎
    fonts_.drawSmall(ui::t(ui::kEngineLabel), cx, y0, theme::kMutedDim);
    {
        const std::vector<std::string> items{ ui::t(ui::kEngineAuto), ui::t(ui::kEngineSample), ui::t(ui::kEngineSynth) };
        int idx = engineMode_;
        if (widget::dropdown(ui_, 1001, { cx, ctrlY, engineW, 34.0f }, items, idx, dropdownOpen_, dropdownId_)) {
            engineMode_ = idx;
            mixer_.setEngineMode(static_cast<Mixer::EngineMode>(idx));
            toast(idx == 2 ? ui::t(ui::kEngineSynth) : (idx == 1 ? ui::t(ui::kEngineSample) : ui::t(ui::kEngineAuto)));
        }
    }
    cx += engineW + 22.0f;

    // 主音量
    {
        float v = volume_ * 100.0f;
        if (widget::slider(ui_, 1002, { cx, y0, sliderW, 34.0f }, ui::t(ui::kVolumeLabel),
                           percent(volume_).c_str(), v, 0.0f, 100.0f)) {
            volume_ = v / 100.0f;
            mixer_.setMasterVolume(volume_);
        }
    }
    cx += sliderW + 20.0f;

    // 空间混响
    {
        float v = reverb_ * 100.0f;
        if (widget::slider(ui_, 1003, { cx, y0, sliderW, 34.0f }, ui::t(ui::kReverbLabel),
                           percent(reverb_).c_str(), v, 0.0f, 70.0f)) {
            reverb_ = v / 100.0f;
            mixer_.setReverbMix(reverb_);
        }
    }
    cx += sliderW + 20.0f;

    // 延音踏板
    {
        const Rectangle pb{ cx, ctrlY, 96.0f, 34.0f };
        if (widget::button(ui_, 1004, pb, ui::t(ui::kPedalWord), false, true, sustainUser_)) {
            input_.setSustainManual(!input_.sustainManual());
        }
        const char* state = sustainUser_ ? ui::t(ui::kOn) : ui::t(ui::kOff);
        fonts_.drawSmall(ui::t(ui::kPedalLabel), pb.x + pb.width + 10.0f, y0, theme::kMutedDim);
        fonts_.drawSmall(state, pb.x + pb.width + 10.0f, ctrlY + 9.0f,
                         sustainUser_ ? theme::kGoldLight : theme::kMuted);
    }
    cx += 190.0f;

    // 音符特效档位：关 / 少 / 标准 / 多
    fonts_.drawSmall(ui::t(ui::kFxLabel), cx, y0, theme::kMutedDim);
    {
        const std::vector<std::string> items{ ui::t(ui::kFxOff), ui::t(ui::kFxLow),
                                              ui::t(ui::kFxStandard), ui::t(ui::kFxHigh) };
        int idx = fxLevel_;
        if (widget::dropdown(ui_, 1005, { cx, ctrlY, 110.0f, 34.0f }, items, idx,
                             dropdownOpen_, dropdownId_)) {
            fxLevel_ = idx;
            applyFxSettings();
        }
    }
    cx += 122.0f;

    // 减少动态效果
    if (widget::checkbox(ui_, { cx, ctrlY + 6.0f, 150.0f, 22.0f },
                         ui::t(ui::kFxReducedMotion), fxReducedMotion_)) {
        fxReducedMotion_ = !fxReducedMotion_;
        applyFxSettings();
    }

    // 音源信息
    if (!compact) {
        const std::string info = samples_.empty()
            ? std::string("合成引擎 · 16 次谐波")
            : (std::to_string(samples_.size()) + " 个采样锚点");
        const float iw = fonts_.smallWidth(info.c_str());
        fonts_.drawSmall(info.c_str(), r.x + r.width - iw, ctrlY + 9.0f, theme::kMutedDim);
    }
}

void App::drawKeyboardSection() {
    // 琴盖提示条：漆面底板 + 下沿一道细金线
    const Rectangle& rail = layout_.rail;
    widget::softShadow(rail, 0.25f, 3.0f, 34);
    widget::panel(rail, Color{ 13, 18, 26, 255 }, Color{ 28, 36, 48, 255 }, 0.25f);
    DrawRectangleRounded({ rail.x + 8.0f, rail.y + rail.height - 1.5f, rail.width - 16.0f, 1.0f },
                         1.0f, 2, Color{ 213, 173, 106, 42 });
    fonts_.drawSmall("Grand Studio · 01", rail.x + 12.0f, rail.y + 7.0f, Color{ 198, 168, 116, 220 });
    {
        const float tw = fonts_.smallWidth(ui::t(ui::kRailTip));
        fonts_.drawSmall(ui::t(ui::kRailTip), rail.x + rail.width - tw - 12.0f, rail.y + 7.0f,
                         Color{ 120, 134, 156, 255 });
    }

    pianoView_.draw(mixer_, input_, fonts_);

    // 底部信息行
    const Rectangle& r = layout_.bottom;
    fonts_.drawSmall(ui::t(ui::kKeyRangeTip), r.x, r.y + 6.0f, Color{ 133, 147, 166, 255 });

    const float cx = r.x + r.width * 0.5f;
    const Rectangle down{ cx - 74.0f, r.y, 28.0f, 26.0f };
    const Rectangle up{ cx + 46.0f, r.y, 28.0f, 26.0f };
    if (widget::smallButton(ui_, down, "−")) {
        pianoView_.centerOn(pianoView_.visibleCenterNote() - 12, true);
    }
    if (widget::smallButton(ui_, up, "+")) {
        pianoView_.centerOn(pianoView_.visibleCenterNote() + 12, true);
    }
    const std::string center = noteName(pianoView_.visibleCenterNote());
    const float cw = fonts_.bodyWidth(center.c_str());
    fonts_.drawBody(center.c_str(), cx - cw * 0.5f, r.y + 4.0f, theme::kGold);

    const int active = mixer_.visualTotal();
    const std::string tip = active > 0
        ? (std::string(ui::t(ui::kPlayingPrefix)) + std::to_string(active) + ui::t(ui::kPlayingSuffix))
        : std::string(ui::t(ui::kPolyTip));
    const float tw = fonts_.smallWidth(tip.c_str());
    fonts_.drawSmall(tip.c_str(), r.x + r.width - tw, r.y + 6.0f, Color{ 133, 147, 166, 255 });
}

void App::drawTransportRow() {
    const Rectangle& r = layout_.transport;
    float x = r.x;

    const Rectangle rec{ x, r.y, 140.0f, 42.0f };
    if (widget::button(ui_, 2001, rec, recording_ ? ui::t(ui::kRecordStop) : ui::t(ui::kRecordStart),
                       !recording_, true, recording_)) {
        if (recording_) stopRecording(); else startRecording();
    }
    x += 152.0f;

    const bool canPlay = recorder_.hasNotes() && !recording_ && !playingRecording_;
    if (widget::button(ui_, 2002, { x, r.y, 110.0f, 42.0f }, ui::t(ui::kPlayback), false, canPlay)) startPlayback();
    x += 122.0f;

    const bool canStop = recording_ || playingRecording_ || songPlaying_;
    if (widget::button(ui_, 2003, { x, r.y, 100.0f, 42.0f }, ui::t(ui::kStop), false, canStop)) {
        if (recording_) stopRecording();
        if (playingRecording_) stopPlayback();
        if (songPlaying_) stopSongPlayback();
    }
    x += 112.0f;

    const bool canExport = recorder_.hasNotes() && !recording_;
    if (widget::button(ui_, 2004, { x, r.y, 140.0f, 42.0f }, ui::t(ui::kExportMidi), false, canExport)) {
        exportRecordingMidi();
    }
    x += 168.0f;

    DrawLineEx({ x - 16.0f, r.y + 7.0f }, { x - 16.0f, r.y + 35.0f }, 1.0f, Color{ 255, 255, 255, 16 });

    widget::softShadow({ x - 8.0f, r.y - 5.0f, 510.0f, 52.0f }, 0.14f, 3.0f, 28);
    widget::panel({ x - 8.0f, r.y - 5.0f, 510.0f, 52.0f },
                   Color{ 19, 29, 43, 120 }, Color{ 58, 72, 92, 140 }, 0.14f);
    if (widget::button(ui_, 2005, { x, r.y, 140.0f, 42.0f },
                       metronomeOn_ ? ui::t(ui::kMetronomeOff) : ui::t(ui::kMetronome), false, true, metronomeOn_)) {
        metronomeOn_ = !metronomeOn_;
        metronome_.setEnabled(metronomeOn_);
    }
    x += 156.0f;

    float bpm = bpmF_;
    if (widget::slider(ui_, 2006, { x, r.y, 220.0f, 42.0f }, ui::t(ui::kBpm),
                       std::to_string(static_cast<int>(std::lround(bpmF_))).c_str(), bpm, 40.0f, 200.0f)) {
        bpmF_ = bpm;
        metronome_.setBpm(static_cast<int>(std::lround(bpmF_)));
    }
    x += 236.0f;

    // 节拍指示灯
    if (metronomeOn_) {
        const std::uint64_t now = mixer_.currentFrame();
        const std::uint64_t last = metronome_.lastClickFrame();
        const bool flash = now >= last && (now - last) < static_cast<std::uint64_t>(sampleRate_ * 0.09);
        const int beat = metronome_.currentBeat();
        for (int i = 0; i < 4; ++i) {
            const bool on = flash && (beat % 4 == i);
            const Vector2 c{ x + static_cast<float>(i) * 16.0f, r.y + 21.0f };
            if (on) {
                DrawCircleGradient(c, 11.0f, Color{ 246, 210, 150, 90 }, Color{ 246, 210, 150, 0 });
                DrawCircleV(c, 5.0f, theme::kGoldLight);
            } else {
                DrawCircleV(c, 3.5f, Color{ 52, 64, 80, 255 });
            }
        }
    }

    // 录制进度条
    const float p = recording_
        ? std::fmod(static_cast<double>(mixer_.currentFrame()) / std::max(1.0, sampleRate_), 30.0) / 30.0
        : (playingRecording_ ? recordedPlayer_.progress() : 0.0);
    widget::progressBar({ r.x, r.y + 46.0f - 2.0f, r.width, 3.0f }, static_cast<float>(p),
                        Color{ 228, 186, 120, 255 });
}

void App::drawSongPanel() {
    const Rectangle& r = layout_.song;
    const bool compact = r.width < 1200.0f;
    widget::softShadow(r, 0.06f, 5.0f, 44);
    widget::panel(r, Color{ 13, 21, 33, 255 }, theme::kLine, 0.06f);

    if (songs_.empty() || songView_.empty()) {
        // 无曲目时保持固定高度，避免拖放提示把整页撑开。
        const Rectangle drop{ r.x + 18.0f, r.y + 12.0f, r.width - 36.0f,
                              std::min(104.0f, r.height - 24.0f) };
        widget::panel(drop, Color{ 18, 28, 42, 180 }, Color{ 62, 76, 96, 200 }, 0.16f);
        const char* title = ui::t(ui::kHeroLine1);
        const float tw = fonts_.bodyWidth(title);
        fonts_.drawBody(title, drop.x + (drop.width - tw) * 0.5f, drop.y + 20.0f, theme::kInk);
        const char* hint = ui::t(ui::kImportHint);
        const float hw = fonts_.smallWidth(hint);
        fonts_.drawSmall(hint, drop.x + (drop.width - hw) * 0.5f, drop.y + 56.0f, theme::kMuted);
        return;
    }

    const float x = r.x + 14.0f;
    float y = r.y + 8.0f;

    // 顶部控制行按用途分组，减少连续控件造成的视觉拥挤。
    if (!compact) {
        const Color groupLine{ 58, 72, 92, 150 };
        DrawRectangleRoundedLinesEx({ x - 8.0f, y - 5.0f, 630.0f, 42.0f }, 0.12f, 6, 1.0f, groupLine);
        DrawRectangleRoundedLinesEx({ x + 642.0f, y - 5.0f, 235.0f, 42.0f }, 0.12f, 6, 1.0f, groupLine);
        DrawRectangleRoundedLinesEx({ x + 888.0f, y - 5.0f, 500.0f, 42.0f }, 0.12f, 6, 1.0f, groupLine);
    }

    // ---- 第一行 ----
    {
        const float searchW = compact ? 150.0f : 190.0f;
        const float filterX = x + searchW + 12.0f;
        const float filterW = compact ? 100.0f : 120.0f;
        const float titleX = filterX + filterW + 12.0f;
        const float titleW = compact ? 220.0f : 300.0f;
        const float playX = titleX + titleW + 12.0f;
        const float playW = compact ? 90.0f : 108.0f;
        const float stopX = playX + playW + 12.0f;
        const float stopW = compact ? 90.0f : 108.0f;
        const float exportX = stopX + stopW + 12.0f;
        const float exportW = compact ? 100.0f : 120.0f;
        const Rectangle search{ x, y, searchW, 32.0f };
        widget::panel(search, searchFocused_ ? Color{ 26, 38, 55, 255 } : Color{ 19, 29, 43, 255 },
                      searchFocused_ ? theme::kGold : theme::kLineSoft, 0.28f);
        widget::bevel(search, 10, 18);
        const bool hover = ui_.hovered(search);
        if (ui_.mousePressed && hover && !ui_.consumedClick) {
            searchFocused_ = true;
            ui_.consumedClick = true;
        } else if (ui_.mousePressed && !hover && !ui_.consumedClick && searchFocused_) {
            searchFocused_ = false;
            ui_.consumedClick = true;
            refreshSongList();
        }
        if (searchText_.empty() && !searchFocused_) {
            fonts_.drawSmall(ui::t(ui::kSearchHint), search.x + 10.0f, search.y + 9.0f, theme::kMutedDim);
        } else {
            const std::string shown = searchText_ + (searchFocused_ ? "|" : "");
            fonts_.drawSmall(shown.c_str(), search.x + 10.0f, search.y + 9.0f, theme::kInk);
        }

        const std::vector<std::string> diffs{ ui::t(ui::kDiffAll), ui::t(ui::kDiffEasy), ui::t(ui::kDiffHard) };
        int di = difficultyFilter_;
        if (widget::dropdown(ui_, 3001, { filterX, y, filterW, 32.0f }, diffs, di, dropdownOpen_, dropdownId_)) {
            difficultyFilter_ = di;
            refreshSongList();
        }

        // 曲目下拉
        std::vector<std::string> titles;
        titles.reserve(songView_.size());
        for (int idx : songView_) {
            const Song& s = songs_[static_cast<std::size_t>(idx)];
            const std::string title = fonts_.canRender(s.title) ? s.title : std::string(ui::t(ui::kImportedMidi));
            titles.push_back(title + "  ·  " + std::to_string(s.bpm) + " BPM");
        }
        if (titles.empty()) titles.push_back(std::string("（") + ui::t(ui::kNoMatchingSongs) + "）");

        int sel = 0;
        for (std::size_t i = 0; i < songView_.size(); ++i)
            if (songView_[i] == songIndex_) { sel = static_cast<int>(i); break; }

        if (widget::dropdown(ui_, 3002, { titleX, y, titleW, 32.0f }, titles, sel, dropdownOpen_, dropdownId_)) {
            if (sel >= 0 && sel < static_cast<int>(songView_.size())) selectSong(songView_[static_cast<std::size_t>(sel)]);
        }

        if (widget::button(ui_, 3003, { playX, y, playW, 32.0f }, ui::t(ui::kPlaySong),
                           !songPlaying_, !songs_.empty() && !songPlaying_)) {
            startSongPlayback();
        }
        if (widget::button(ui_, 3004, { stopX, y, stopW, 32.0f }, ui::t(ui::kStopSong), false, songPlaying_)) {
            stopSongPlayback();
        }
        if (widget::button(ui_, 3005, { exportX, y, exportW, 32.0f }, ui::t(ui::kExportSong), false, !songs_.empty())) {
            exportSongMidi();
        }

        const std::vector<std::string> modes{ ui::t(ui::kModeFull), ui::t(ui::kModeAccomp), ui::t(ui::kModeGuide) };
        int mi = practiceMode_;
        if (!compact && widget::dropdown(ui_, 3006, { x + 1018.0f, y, 160.0f, 32.0f }, modes, mi, dropdownOpen_, dropdownId_)) {
            practiceMode_ = mi;
            applyPracticeMode();
            if (mi == 0) judge_.reset();
            toast(modes[static_cast<std::size_t>(mi)]);
        }

        const bool fav = isFavorite(songIndex_);
        if (!compact && widget::button(ui_, 3007, { x + 1190.0f, y, 108.0f, 32.0f },
                           fav ? ui::t(ui::kFavRemove) : ui::t(ui::kFavAdd), false, true, fav)) {
            toggleFavorite(songIndex_);
        }
        if (!compact && widget::checkbox(ui_, { x + 1310.0f, y, 130.0f, 32.0f }, ui::t(ui::kFavOnly), showFavoritesOnly_)) {
            showFavoritesOnly_ = !showFavoritesOnly_;
            refreshSongList();
        }
    }

    // ---- 第二行：速度 / 移调 / 静音 ----
    y += 42.0f;
    {
        float sp = speed_;
        const float sliderW = compact ? 170.0f : 210.0f;
        if (widget::slider(ui_, 3008, { x, y, sliderW, 34.0f }, ui::t(ui::kSpeed),
                           (num(speed_, 1) + "×").c_str(), sp, 0.5f, 1.5f)) {
            if (songPlaying_ && practiceMode_ != 0) {
                speed_ = static_cast<float>(player_.speed());
                toast(ui::t(ui::kPracticeSpeedLock));
            } else {
                speed_ = sp;
                if (songPlaying_) player_.setSpeed(speed_);
            }
        }
        float tr = transposeF_;
        const std::string trText =
            (tr >= 0.0f ? "+" : "") + std::to_string(static_cast<int>(std::lround(tr)));
        if (widget::slider(ui_, 3009, { x + sliderW + 22.0f, y, sliderW, 34.0f }, ui::t(ui::kTranspose),
                           trText.c_str(), tr, -6.0f, 6.0f)) {
            transposeF_ = tr;
            if (songPlaying_ && practiceMode_ != 0) {
                transposeF_ = static_cast<float>(player_.transpose());
                toast(ui::t(ui::kPracticeTransposeLock));
            } else {
                player_.setTranspose(static_cast<int>(std::lround(transposeF_)));
            }
        }
        if (widget::checkbox(ui_, { x + 466.0f, y + 8.0f, 120.0f, 22.0f }, ui::t(ui::kMuteMelody), muteMelody_)) {
            muteMelody_ = !muteMelody_;
            player_.setTrackMuted(kTrackMelody, muteMelody_);
        }
        if (widget::checkbox(ui_, { x + 596.0f, y + 8.0f, 120.0f, 22.0f }, ui::t(ui::kMuteAccomp), muteAccomp_)) {
            muteAccomp_ = !muteAccomp_;
            player_.setTrackMuted(kTrackAccomp, muteAccomp_);
        }
        if (widget::checkbox(ui_, { x + 726.0f, y + 8.0f, 120.0f, 22.0f }, ui::t(ui::kFollowKey), followKeyboard_)) {
            followKeyboard_ = !followKeyboard_;
        }
        if (!compact) fonts_.drawSmall(ui::t(ui::kImportHint), x + 860.0f, y + 11.0f, theme::kMutedDim);
    }

    // ---- 第三行：进度 + 状态 ----
    y += 42.0f;
    {
        const float timelineW = compact ? 300.0f : 420.0f;
        const Rectangle bar{ x, y + 6.0f, timelineW, 6.0f };
        const float prog = songPlaying_ ? static_cast<float>(player_.progress()) : 0.0f;
        widget::progressBar(bar, prog, Color{ 196, 152, 92, 255 });
        if (songPlaying_ && ui_.mouseDown &&
            CheckCollisionPointRec(ui_.mouse, { bar.x, bar.y - 8.0f, bar.width, 22.0f })) {
            const float t = std::clamp((ui_.mouse.x - bar.x) / bar.width, 0.0f, 1.0f);
            const Song& s = songs_[static_cast<std::size_t>(songIndex_)];
            const double tick = static_cast<double>(s.durationTicks()) * t;
            player_.play(mixer_.currentFrame() + static_cast<std::uint64_t>(sampleRate_ * 0.08), tick);
            if (judge_.active()) {
                judge_.load(player_.song(), mixer_.currentFrame() + static_cast<std::uint64_t>(sampleRate_ * 0.08),
                            speed_, player_.transpose());
            }
        }

        const double posSec = (songPlaying_ && songIndex_ >= 0 &&
                               songIndex_ < static_cast<int>(songs_.size()))
            ? player_.positionTick() / static_cast<double>(songs_[static_cast<std::size_t>(songIndex_)].ppq) *
                  60.0 / static_cast<double>(songs_[static_cast<std::size_t>(songIndex_)].bpm) /
                  std::max(0.01, static_cast<double>(speed_))
            : 0.0;
        const std::string timeText = num(posSec, 1) + "s";
        fonts_.drawSmall(timeText.c_str(), x + timelineW + 14.0f, y + 2.0f, theme::kGold);
    }

    // ---- 第四行：跟练判定 ----
    y += 30.0f;
    {
        widget::panel({ x - 8.0f, y - 8.0f, 640.0f, 34.0f },
                       Color{ 19, 29, 43, 120 }, Color{ 58, 72, 92, 130 }, 0.12f);
        fonts_.drawSmall(ui::t(ui::kJudgeTitle), x, y, theme::kMutedDim);
        float cx = x + fonts_.smallWidth(ui::t(ui::kJudgeTitle)) + 14.0f;
        auto stat = [&](const char* label, int value, Color c) {
            const std::string s = std::string(label) + " " + std::to_string(value);
            fonts_.drawSmall(s.c_str(), cx, y, c);
            cx += fonts_.smallWidth(s.c_str()) + 18.0f;
        };
        stat(ui::t(ui::kPerfect), judge_.perfect(), theme::kGoldLight);
        stat(ui::t(ui::kGood), judge_.good(), theme::kGood);
        stat(ui::t(ui::kMiss), judge_.miss(), theme::kBad);
        stat(ui::t(ui::kExtra), judge_.extra(), theme::kMuted);
        stat(ui::t(ui::kCombo), judge_.combo(), theme::kInk);
        stat(ui::t(ui::kAccuracy), static_cast<int>(std::lround(judge_.accuracy() * 100.0f)), theme::kInk);

        if (!judge_.lastText().empty() && judge_.judged() + judge_.extra() > 0) {
            const std::string& t = judge_.lastText();
            const float tw = fonts_.smallWidth(t.c_str());
            fonts_.drawSmall(t.c_str(), x + 420.0f - tw, y, theme::kGold);
        }
    }
}

void App::drawGuide() {
    const Rectangle& r = layout_.guide;
    const float gap = 14.0f;
    const float cw = (r.width - gap * 2.0f) / 3.0f;

    struct Card { const char* title; const char* body; const char* body2; const char* icon; };
    // 图标用单个汉字而不是符号：键盘/音符类的装饰符号微软雅黑不含，
    // 会渲染成 '?'（见 ui/UiStrings.h 里的说明）。
    const Card cards[3] = {
        { ui::t(ui::kGuide1Title), ui::t(ui::kGuide1Body), ui::t(ui::kGuide1Body2), "键" },
        { ui::t(ui::kGuide2Title), ui::t(ui::kGuide2Body), ui::t(ui::kGuide2Body2), "触" },
        { ui::t(ui::kGuide3Title), ui::t(ui::kGuide3Body), ui::t(ui::kGuide3Body2), "录" },
    };

    for (int i = 0; i < 3; ++i) {
        const Rectangle c{ r.x + static_cast<float>(i) * (cw + gap), r.y, cw, r.height };
        widget::softShadow(c, 0.14f, 4.0f, 30);
        widget::panel(c, Color{ 16, 25, 37, 150 }, Color{ 48, 60, 76, 170 }, 0.14f);
        widget::bevel(c, 10, 14);

        const Rectangle icon{ c.x + 12.0f, c.y + 12.0f, 30.0f, 30.0f };
        widget::panel(icon, Color{ 213, 173, 106, 16 }, Color{ 158, 132, 88, 150 }, 0.24f);
        fonts_.drawBody(cards[i].icon, icon.x + 7.0f, icon.y + 6.0f, Color{ 226, 190, 130, 255 });

        const float tx = c.x + 52.0f;
        fonts_.drawSmall(cards[i].title, tx, c.y + 11.0f, Color{ 224, 232, 242, 255 });
        auto drawLines = [&](const char* text, float y) {
            std::string s(text ? text : "");
            std::size_t start = 0;
            for (;;) {
                const std::size_t end = s.find('\n', start);
                const std::string line = s.substr(start, end == std::string::npos ? end : end - start);
                fonts_.drawSmall(line.c_str(), tx, y, Color{ 146, 160, 176, 255 });
                y += 15.0f;
                if (end == std::string::npos) break;
                start = end + 1;
            }
        };
        drawLines(cards[i].body, c.y + 29.0f);
        drawLines(cards[i].body2, c.y + 59.0f);
    }
}

void App::drawFooter() {
    const Rectangle& r = layout_.footer;
    const float tw = fonts_.smallWidth(ui::t(ui::kFooter));
    fonts_.drawSmall(ui::t(ui::kFooter), r.x + (r.width - tw) * 0.5f, r.y, Color{ 100, 116, 139, 255 });
}

void App::drawOverlays() {
    if (toastTimer_ > 0.0f && !toastText_.empty()) {
        const float tw = fonts_.bodyWidth(toastText_.c_str());
        // Toast 放在曲目面板底部上方，避免覆盖说明卡和页脚；
        // 说明卡会随窗口高度调整，不能用固定的屏幕底部坐标。
        const float toastY = std::max(150.0f, layout_.guide.y - 42.0f);
        const Rectangle t{ (layout_.w - tw) * 0.5f - 18.0f,
                           toastY, tw + 36.0f, 34.0f };
        const float alpha = std::clamp(toastTimer_ / 0.4f, 0.0f, 1.0f);
        const unsigned char a8 = static_cast<unsigned char>(255 * alpha);
        widget::softShadow(t, 0.5f, 4.0f, static_cast<unsigned char>(70 * alpha));
        widget::panel(t, Color{ 240, 220, 178, a8 },
                      Color{ 186, 146, 92, static_cast<unsigned char>(130 * alpha) }, 0.5f);
        fonts_.drawBody(toastText_.c_str(), t.x + 18.0f, t.y + 8.0f,
                        Color{ 30, 34, 42, a8 });
    }
}

// ===========================================================================
// 业务动作
// ===========================================================================

void App::startRecording() {
    if (songPlaying_) stopSongPlayback();
    if (playingRecording_) stopPlayback();
    recorder_.start(mixer_.currentFrame());
    if (sustainUser_) recorder_.capture(mixer_.currentFrame(), MsgType::Sustain, 0, 127);
    recording_ = true;
    setStatus(ui::t(ui::kToastRecordStart));
    toast(ui::t(ui::kToastRecordStart));
}

void App::stopRecording() {
    if (!recording_) return;
    // 收尾：把仍按住的键补上 NoteOff，避免导出后出现挂音
    const std::uint64_t frame = mixer_.currentFrame();
    for (int m = kMidiLow; m <= kMidiHigh; ++m) {
        const int n = mixer_.visualCount(m);
        for (int k = 0; k < n; ++k) recorder_.capture(frame, MsgType::NoteOff, m, 0);
    }
    recorder_.capture(frame, MsgType::Sustain, 0, 0);
    recorder_.stop(frame);
    recording_ = false;
    setStatus(recorder_.hasNotes() ? ui::t(ui::kToastRecordDone) : ui::t(ui::kToastRecordEmpty),
              !recorder_.hasNotes());
    toast(recorder_.hasNotes() ? ui::t(ui::kToastRecordDone) : ui::t(ui::kToastRecordEmpty));
}

void App::startPlayback() {
    if (!recorder_.hasNotes()) return;
    if (songPlaying_) stopSongPlayback();
    recordedPlayer_.setEvents(recorder_.events(), sampleRate_);
    recordedPlayer_.play(mixer_.currentFrame() + static_cast<std::uint64_t>(sampleRate_ * 0.08));
    playingRecording_ = true;
    setStatus(ui::t(ui::kPlayback));
}

void App::stopPlayback() {
    if (!playingRecording_) return;
    recordedPlayer_.stop();
    device_.pushReleaseSource(Source::Playback);
    playingRecording_ = false;
    // 回放接管了踏板，结束后恢复用户踏板状态
    sustainEffective_ = !sustainUser_;
    pushSustainIfChanged();
}

void App::exportRecordingMidi() {
    namespace fs = std::filesystem;
    if (!recorder_.hasNotes()) return;

    std::error_code ec;
    const std::filesystem::path dir = exportDirectory();
    fs::create_directories(dir, ec);

    MidiWriteOptions opts;
    opts.bpm = static_cast<int>(std::lround(bpmF_));
    opts.ppq = kDefaultPpq;
    const std::vector<std::uint8_t> bytes =
        writeRecordingMidi(recorder_.events(), sampleRate_, recorder_.durationFrames(), opts);

    const std::filesystem::path path = dir / timestampName("Grand_Studio", ".mid");
    std::string err;
    if (saveMidiFile(path, bytes, &err)) {
        setStatus(ui::t(ui::kToastExported));
        toast(std::string(ui::t(ui::kToastExported)) + " · " + fs::path(path).filename().string());
    } else {
        setStatus(ui::t(ui::kToastExportFail), true);
        toast(std::string(ui::t(ui::kToastExportFail)) + "：" + err);
    }
}

void App::exportSongMidi() {
    namespace fs = std::filesystem;
    if (songs_.empty()) { toast(ui::t(ui::kToastNoSong)); return; }
    const Song& s = songs_[static_cast<std::size_t>(songIndex_)];

    std::error_code ec;
    const std::filesystem::path dir = exportDirectory();
    fs::create_directories(dir, ec);

    MidiWriteOptions opts;
    opts.bpm = s.bpm;
    opts.ppq = s.ppq;
    const std::vector<std::uint8_t> bytes = writeSongMidi(s, opts);

    const std::filesystem::path path = dir / timestampName("Song", ".mid");
    std::string err;
    if (saveMidiFile(path, bytes, &err)) {
        toast(std::string(ui::t(ui::kToastExported)) + " · " + fs::path(path).filename().string());
    } else {
        toast(std::string(ui::t(ui::kToastExportFail)) + "：" + err);
    }
}

void App::importMidiFile(const std::string& path) {
    const MidiParseResult r = loadMidiFile(path);
    if (!r.ok) {
        toast(std::string(ui::t(ui::kToastImportFail)) + "：" + r.error);
        setStatus(std::string(ui::t(ui::kToastImportFail)) + "：" + r.error, true);
        return;
    }
    Song s = r.song;
    s.id = "import-" + std::to_string(songs_.size());
    if (!fonts_.canRender(s.title)) s.title = ui::t(ui::kImportedMidi);

    songs_.push_back(std::move(s));
    refreshSongList();
    // 选中刚导入的曲目
    for (std::size_t i = 0; i < songView_.size(); ++i) {
        if (songView_[i] == static_cast<int>(songs_.size()) - 1) { selectSong(songView_[i]); break; }
    }
    toast(std::string(ui::t(ui::kToastImportOk)) + " · " + std::to_string(r.song.events.size()) + " 个音符");
    setStatus(ui::t(ui::kToastImportOk));
}

void App::refreshSongList() {
    songView_.clear();
    for (std::size_t i = 0; i < songs_.size(); ++i) {
        const Song& s = songs_[i];
        if (difficultyFilter_ != 0 && s.difficulty != difficultyFilter_) continue;
        if (showFavoritesOnly_ && !isFavorite(static_cast<int>(i))) continue;
        if (!searchText_.empty()) {
            const std::string title = fonts_.canRender(s.title) ? s.title : std::string("imported midi");
            if (title.find(searchText_) == std::string::npos) continue;
        }
        songView_.push_back(static_cast<int>(i));
    }
}

void App::selectSong(int index) {
    if (index < 0 || index >= static_cast<int>(songs_.size())) return;
    if (songPlaying_) stopSongPlayback();
    songIndex_ = index;
    player_.setSong(songs_[static_cast<std::size_t>(index)], sampleRate_);
    player_.setSpeed(speed_);
    player_.setTranspose(static_cast<int>(std::lround(transposeF_)));
    applyPracticeMode();
    judge_.reset();
    setStatus(ui::t(ui::kSongIdle));
}

void App::startSongPlayback() {
    if (songs_.empty()) { toast(ui::t(ui::kToastNoSong)); return; }
    if (recording_) stopRecording();
    if (playingRecording_) stopPlayback();

    player_.setSong(songs_[static_cast<std::size_t>(songIndex_)], sampleRate_);
    player_.setSpeed(speed_);
    player_.setTranspose(static_cast<int>(std::lround(transposeF_)));
    applyPracticeMode();

    const std::uint64_t anchor = mixer_.currentFrame() + static_cast<std::uint64_t>(sampleRate_ * 0.12);
    if (practiceMode_ != 0) judge_.load(player_.song(), anchor, speed_, player_.transpose());

    player_.play(anchor, 0.0);
    songPlaying_ = true;
    metronome_.setEnabled(metronomeOn_ || practiceMode_ == 2);
    setStatus(std::string(ui::t(ui::kPlaySong)) + " · " + songs_[static_cast<std::size_t>(songIndex_)].title);
}

void App::stopSongPlayback() {
    if (!songPlaying_) return;
    player_.stop();
    device_.pushReleaseSource(Source::Song);
    songPlaying_ = false;
    metronome_.setEnabled(metronomeOn_);
    if (judge_.active()) judge_.finish(mixer_.currentFrame());
    setStatus(ui::t(ui::kSongIdle));
}

void App::applyPracticeMode() {
    switch (practiceMode_) {
    case 1:  muteMelody_ = true;  muteAccomp_ = false; break;
    case 2:  muteMelody_ = true;  muteAccomp_ = true;  break;
    default: muteMelody_ = false; muteAccomp_ = false; break;
    }
    player_.setTrackMuted(kTrackMelody, muteMelody_);
    player_.setTrackMuted(kTrackAccomp, muteAccomp_);
}

bool App::isFavorite(int index) const {
    if (index < 0 || index >= static_cast<int>(songs_.size())) return false;
    const std::string& id = songs_[static_cast<std::size_t>(index)].id;
    return std::find(favorites_.begin(), favorites_.end(), id) != favorites_.end();
}

void App::toggleFavorite(int index) {
    if (index < 0 || index >= static_cast<int>(songs_.size())) return;
    const std::string& id = songs_[static_cast<std::size_t>(index)].id;
    auto it = std::find(favorites_.begin(), favorites_.end(), id);
    if (it == favorites_.end()) {
        favorites_.push_back(id);
        toast(ui::t(ui::kToastFavAdded));
    } else {
        favorites_.erase(it);
        toast(ui::t(ui::kToastFavRemoved));
    }
    saveFavorites();
    refreshSongList();
}

void App::loadFavorites() {
    favorites_.clear();
    std::ifstream in(userDataPath("favorites.txt"));
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
            line.pop_back();
        if (!line.empty()) favorites_.push_back(line);
    }
}

void App::saveFavorites() {
    const std::filesystem::path dir = userDataDirectory();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(userDataPath("favorites.txt"), std::ios::trunc);
    if (!out) return;
    for (const auto& id : favorites_) out << id << '\n';
}

void App::loadLanguage() {
    std::ifstream in(userDataPath("language.txt"));
    std::string value;
    if (in >> value && value == "en") ui::setLanguage(ui::Language::English);
    else ui::setLanguage(ui::Language::Chinese);
}

void App::saveLanguage() const {
    const std::filesystem::path dir = userDataDirectory();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(userDataPath("language.txt"), std::ios::trunc);
    if (out) out << (ui::language() == ui::Language::English ? "en\n" : "zh\n");
}

void App::loadFxSettings() {
    std::ifstream in(userDataPath("fx.txt"));
    if (!in) return;   // 文件不存在：保留默认（Standard / 关）
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const fx::FxSettings s = fx::parseFxSettings(text);
    fxLevel_ = s.level;
    fxReducedMotion_ = s.reducedMotion;
}

void App::saveFxSettings() const {
    const std::filesystem::path dir = userDataDirectory();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(userDataPath("fx.txt"), std::ios::trunc);
    if (out) out << fx::formatFxSettings(fx::FxSettings{ fxLevel_, fxReducedMotion_ });
}

void App::applyFxSettings() {
    noteFx_.setLevel(static_cast<fx::Level>(fxLevel_));
    noteFx_.setReducedMotion(fxReducedMotion_);
    saveFxSettings();
}

std::string App::timestampName(const char* prefix, const char* ext) const {
    const std::time_t t = std::time(nullptr);
    std::tm* tmv = std::localtime(&t);
    char buf[64] = { 0 };
    if (tmv) std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", tmv);
    return std::string(prefix) + "_" + buf + ext;
}

std::filesystem::path App::exportDirectory() const {
    return userDataDirectory() / "exports";
}

void App::setStatus(const std::string& text, bool warn) {
    statusText_ = text;
    statusWarn_ = warn;
}

void App::toast(const std::string& text) {
    toastText_ = text;
    toastTimer_ = 2.6f;
}

} // namespace piano
