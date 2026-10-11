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
    // Raylib 的绘制坐标已经是逻辑窗口坐标。这里不再套 Camera2D 缩放，
    // 否则 PianoView 的裁剪矩形仍按屏幕像素计算，会把琴键裁成一条细带。
    viewScale_ = 1.0f;
    viewOffset_ = {};
    ui_.scale = 1.0f;
    ui_.offset = {};
    sampleLog_ += "[layout] framebuffer " + std::to_string(GetScreenWidth()) + "x" +
                  std::to_string(GetScreenHeight()) + " · dpi " + std::to_string(dpiScale_) + "\n";
    layout_.compute(static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight()));

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
    fontScale_ = std::max(1.0f, static_cast<float>(GetRenderWidth()) / GetScreenWidth());
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
        return pianoView_.hitTest(x, y, midi, vel);
    });
    pianoView_.setKeyboardMap(&keyboardMap_);
    pianoView_.setLayout(layout_);
    pianoView_.centerOn(60, false);

    // ---- 音频 ----
    mixer_.setSampleBank(&samples_);
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
}

void App::setWindowSize(int width, int height) noexcept {
    windowWidth_ = std::clamp(width, 1100, 3840);
    windowHeight_ = std::clamp(height, 880, 2160);
}

void App::run() {
    if (captureAutoPlay_ && !songs_.empty()) startSongPlayback();

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
        const float dt = GetFrameTime();
        ui_.beginFrame();
        handleInput(dt);
        draw(dt);

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
    const float rasterScale = std::max(1.0f, static_cast<float>(GetRenderWidth()) / GetScreenWidth());
    if (std::fabs(rasterScale - fontScale_) > 0.01f) {
        fontScaleTimer_ += dt;
        if (fontScaleTimer_ >= 0.15f) {
            fonts_.load(fontSamples_, rasterScale);
            fontScale_ = rasterScale;
            fontScaleTimer_ = 0.0f;
        }
    } else fontScaleTimer_ = 0.0f;
    layout_.compute(static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight()));
    pianoView_.setLayout(layout_);

    BeginDrawing();
    ClearBackground(theme::kBg);
    // 背景光晕（呼应 HTML 原版的径向渐变）
    DrawCircleGradient({ layout_.w * 0.5f, -160.0f }, 900.0f,
                       Color{ 41, 54, 76, 90 }, Color{ 11, 16, 25, 0 });
    DrawCircleGradient({ layout_.w * 1.02f, layout_.h * 0.72f }, 520.0f,
                       Color{ 21, 31, 48, 70 }, Color{ 11, 16, 25, 0 });

    drawTopbar();
    drawHero();
    drawConsole();
    drawGuide();
    drawFooter();
    widget::drawDeferredDropdowns(ui_);
    drawOverlays();

    EndDrawing();
}

void App::drawTopbar() {
    const Rectangle& r = layout_.topbar;

    // 徽标
    const Rectangle logo{ r.x, r.y + 3.0f, 40.0f, 38.0f };
    widget::panel(logo, Color{ 58, 48, 37, 255 }, Color{ 116, 98, 73, 255 }, 0.24f);
    for (int i = 0; i < 5; ++i) {
        const float x = logo.x + 8.0f + static_cast<float>(i) * 6.0f;
        DrawLineEx({ x, logo.y + 9.0f }, { x, logo.y + 27.0f }, 2.4f, theme::kGold);
    }

    fonts_.drawSmall(ui::t(ui::kBrandTitle), logo.x + 52.0f, logo.y + 4.0f, theme::kInk);
    fonts_.drawSmall(ui::t(ui::kBrandSub), logo.x + 52.0f, logo.y + 20.0f, theme::kMutedDim);

    // 状态胶囊
    const std::string st = statusText_;
    const float tw = fonts_.smallWidth(st.c_str());
    const float pillW = tw + 46.0f;
    const Rectangle pill{ r.x + r.width - pillW, r.y + 5.0f, pillW, 32.0f };
    widget::panel(pill, Color{ 20, 30, 44, 200 }, Color{ 51, 64, 81, 255 }, 0.5f);
    DrawCircleV({ pill.x + 16.0f, pill.y + pill.height * 0.5f }, 4.0f,
                statusWarn_ ? theme::kWarn : theme::kGood);
    fonts_.drawSmall(st.c_str(), pill.x + 27.0f, pill.y + 9.0f, Color{ 180, 194, 206, 255 });

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

    fonts_.drawSmall(ui::t(ui::kEyebrow), r.x, r.y, theme::kGold);
    const float x1 = r.x;
    const float y1 = r.y + 16.0f;
    fonts_.drawTitle(ui::t(ui::kHeroLine1), x1, y1, theme::kInk);
    fonts_.drawTitle(ui::t(ui::kHeroAccent), x1 + fonts_.titleWidth(ui::t(ui::kHeroLine1)) + 4.0f, y1, theme::kGoldLight);
    fonts_.drawSmall(ui::t(ui::kHeroDesc), x1, y1 + 38.0f, Color{ 158, 172, 189, 255 });

    // 右侧电平波形
    const Rectangle scope{ r.x + r.width - 320.0f, r.y + 12.0f, 300.0f, 46.0f };
    if (!scope_.empty()) {
        const float step = scope.width / static_cast<float>(scope_.size());
        const float mid = scope.y + scope.height * 0.55f;
        for (std::size_t i = 0; i < scope_.size(); ++i) {
            const float v = std::clamp(scope_[i], 0.0f, 1.0f);
            const float h = std::max(1.0f, v * scope.height * 0.82f);
            const float x = scope.x + static_cast<float>(i) * step;
            const Color c = Color{ static_cast<unsigned char>(181 + 60 * v),
                                   static_cast<unsigned char>(139 + 80 * v),
                                   static_cast<unsigned char>(84 + 70 * v), 230 };
            DrawRectangle(static_cast<int>(x), static_cast<int>(mid - h * 0.5f),
                          1, static_cast<int>(h), c);
        }
        DrawLineEx({ scope.x, scope.y + scope.height }, { scope.x + scope.width, scope.y + scope.height },
                   1.0f, Color{ 58, 65, 80, 255 });
        fonts_.drawSmall(ui::t(ui::kLiveSound), scope.x + scope.width - 62.0f, scope.y - 12.0f,
                         Color{ 111, 128, 149, 255 });
    }
}

void App::drawConsole() {
    const Rectangle& r = layout_.console;
    widget::panel(r, theme::kPanel, theme::kLineSoft, 0.035f);
    // 顶部高光
    DrawRectangleRounded({ r.x + 18.0f, r.y, r.width - 36.0f, 1.0f }, 1.0f, 4,
                         Color{ 197, 200, 209, 40 });

    // 头部
    fonts_.drawBody(ui::t(ui::kInstrument), layout_.header.x, layout_.header.y, theme::kInk);
    fonts_.drawSmall(ui::t(ui::kInstrumentSub), layout_.header.x, layout_.header.y + 22.0f, theme::kMutedDim);

    const std::string badge = samples_.empty()
        ? std::string(ui::t(ui::kSynthBadge))
        : ("● " + std::to_string(samples_.size()) + " 组真实采样");
    const float bw = fonts_.smallWidth(badge.c_str());
    const Rectangle bp{ layout_.header.x + layout_.header.width - bw - 28.0f,
                        layout_.header.y + 3.0f, bw + 24.0f, 28.0f };
    widget::panel(bp, Color{ 202, 162, 107, 12 }, Color{ 132, 105, 66, 110 }, 0.5f);
    fonts_.drawSmall(badge.c_str(), bp.x + 12.0f, bp.y + 7.0f, Color{ 232, 204, 158, 255 });

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

    // 音色引擎
    fonts_.drawSmall(ui::t(ui::kEngineLabel), r.x, y0, theme::kMutedDim);
    {
        const std::vector<std::string> items{ ui::t(ui::kEngineAuto), ui::t(ui::kEngineSample), ui::t(ui::kEngineSynth) };
        int idx = engineMode_;
        if (widget::dropdown(ui_, 1001, { r.x, ctrlY, 250.0f, 34.0f }, items, idx, dropdownOpen_, dropdownId_)) {
            engineMode_ = idx;
            mixer_.setEngineMode(static_cast<Mixer::EngineMode>(idx));
            toast(idx == 2 ? ui::t(ui::kEngineSynth) : (idx == 1 ? ui::t(ui::kEngineSample) : ui::t(ui::kEngineAuto)));
        }
    }

    // 主音量
    {
        float v = volume_ * 100.0f;
        if (widget::slider(ui_, 1002, { r.x + 272.0f, y0, 240.0f, 34.0f }, ui::t(ui::kVolumeLabel),
                           percent(volume_).c_str(), v, 0.0f, 100.0f)) {
            volume_ = v / 100.0f;
            mixer_.setMasterVolume(volume_);
        }
    }

    // 空间混响
    {
        float v = reverb_ * 100.0f;
        if (widget::slider(ui_, 1003, { r.x + 534.0f, y0, 240.0f, 34.0f }, ui::t(ui::kReverbLabel),
                           percent(reverb_).c_str(), v, 0.0f, 70.0f)) {
            reverb_ = v / 100.0f;
            mixer_.setReverbMix(reverb_);
        }
    }

    // 延音踏板
    {
        const Rectangle pb{ r.x + 796.0f, ctrlY, 150.0f, 34.0f };
        if (widget::button(ui_, 1004, pb, ui::t(ui::kPedalWord), false, true, sustainUser_)) {
            input_.setSustainManual(!input_.sustainManual());
        }
        const char* state = sustainUser_ ? ui::t(ui::kOn) : ui::t(ui::kOff);
        fonts_.drawSmall(ui::t(ui::kPedalLabel), pb.x + pb.width + 12.0f, y0, theme::kMutedDim);
        fonts_.drawSmall(state, pb.x + pb.width + 12.0f, ctrlY + 9.0f,
                         sustainUser_ ? theme::kGoldLight : theme::kMuted);
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
    // 琴盖提示条
    const Rectangle& rail = layout_.rail;
    widget::panel(rail, Color{ 7, 11, 18, 255 }, Color{ 8, 13, 20, 255 }, 0.25f);
    fonts_.drawSmall("Grand Studio · 01", rail.x + 12.0f, rail.y + 7.0f, Color{ 189, 194, 204, 140 });
    {
        const float tw = fonts_.smallWidth(ui::t(ui::kRailTip));
        fonts_.drawSmall(ui::t(ui::kRailTip), rail.x + rail.width - tw - 12.0f, rail.y + 7.0f,
                         Color{ 117, 131, 153, 255 });
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

    DrawLineEx({ x - 16.0f, r.y + 7.0f }, { x - 16.0f, r.y + 35.0f }, 1.0f, Color{ 255, 255, 255, 25 });

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
            DrawCircleV({ x + static_cast<float>(i) * 16.0f, r.y + 21.0f }, on ? 6.0f : 4.0f,
                        on ? theme::kGoldLight : Color{ 60, 72, 90, 255 });
        }
    }

    // 录制进度条
    const float p = recording_
        ? std::fmod(static_cast<double>(mixer_.currentFrame()) / std::max(1.0, sampleRate_), 30.0) / 30.0
        : (playingRecording_ ? recordedPlayer_.progress() : 0.0);
    widget::progressBar({ r.x, r.y + 46.0f - 2.0f, r.width, 3.0f }, static_cast<float>(p),
                        Color{ 255, 224, 169, 255 });
}

void App::drawSongPanel() {
    const Rectangle& r = layout_.song;
    const bool compact = r.width < 1200.0f;
    widget::panel(r, Color{ 13, 23, 36, 255 }, theme::kLine, 0.06f);

    const float x = r.x + 14.0f;
    float y = r.y + 8.0f;

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
        widget::panel(search, searchFocused_ ? Color{ 25, 37, 53, 255 } : Color{ 21, 31, 46, 255 },
                      searchFocused_ ? theme::kGold : theme::kLineSoft, 0.28f);
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
            const std::string title = fonts_.canRender(s.title) ? s.title : std::string("导入的 MIDI");
            titles.push_back(title + "  ·  " + std::to_string(s.bpm) + " BPM");
        }
        if (titles.empty()) titles.push_back("（没有匹配的曲目）");

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
                toast("跟练中不能修改速度");
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
                toast("跟练中不能修改移调");
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
        widget::progressBar(bar, prog, Color{ 177, 140, 86, 255 });
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
        widget::panel(c, Color{ 17, 26, 38, 130 }, Color{ 43, 53, 67, 160 }, 0.14f);

        const Rectangle icon{ c.x + 12.0f, c.y + 12.0f, 30.0f, 30.0f };
        widget::panel(icon, Color{ 213, 173, 106, 14 }, Color{ 130, 111, 81, 130 }, 0.24f);
        fonts_.drawBody(cards[i].icon, icon.x + 7.0f, icon.y + 6.0f, Color{ 212, 175, 119, 255 });

        const float tx = c.x + 52.0f;
        fonts_.drawSmall(cards[i].title, tx, c.y + 11.0f, Color{ 220, 229, 239, 255 });
        auto drawLines = [&](const char* text, float y) {
            std::string s(text ? text : "");
            std::size_t start = 0;
            for (;;) {
                const std::size_t end = s.find('\n', start);
                const std::string line = s.substr(start, end == std::string::npos ? end : end - start);
                fonts_.drawSmall(line.c_str(), tx, y, Color{ 140, 154, 170, 255 });
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
        widget::panel(t, Color{ 231, 199, 150, static_cast<unsigned char>(250 * alpha) },
                      Color{ 0, 0, 0, 0 }, 0.5f);
        fonts_.drawBody(toastText_.c_str(), t.x + 18.0f, t.y + 8.0f,
                        Color{ 20, 27, 38, static_cast<unsigned char>(255 * alpha) });
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
    if (!fonts_.canRender(s.title)) s.title = "导入的 MIDI";

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
    std::ifstream in(userDataDirectory() / "favorites.txt");
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
    std::ofstream out(dir / "favorites.txt", std::ios::trunc);
    if (!out) return;
    for (const auto& id : favorites_) out << id << '\n';
}

void App::loadLanguage() {
    std::ifstream in(userDataDirectory() / "language.txt");
    std::string value;
    if (in >> value && value == "en") ui::setLanguage(ui::Language::English);
    else ui::setLanguage(ui::Language::Chinese);
}

void App::saveLanguage() const {
    const std::filesystem::path dir = userDataDirectory();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir / "language.txt", std::ios::trunc);
    if (out) out << (ui::language() == ui::Language::English ? "en\n" : "zh\n");
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
