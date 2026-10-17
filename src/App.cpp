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
#else
#include <unistd.h>
#endif

#include "core/Note.h"
#include "core/UserData.h"
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

/// .mid / .midi 扩展名判断（大小写不敏感）
bool hasMidiExtension(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".mid" || ext == ".midi";
}

/// 带 %d 的文案模板填数
std::string formatInt(const char* pattern, int v) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), pattern, v);
    return std::string(buf);
}

/// 批量导入汇总：「导入 N 首 · 跳过 M 首重复 · K 个非 MIDI · 失败 J 个」，按非零项组合
std::string importSummary(int imported, int dup, int nonMidi, int failed) {
    std::string out;
    auto part = [&](const char* pattern, int v) {
        if (v <= 0) return;
        if (!out.empty()) out += " · ";
        out += formatInt(pattern, v);
    };
    part(ui::t(ui::kImpSumImported), imported);
    part(ui::t(ui::kImpSumDup), dup);
    part(ui::t(ui::kImpSumNonMidi), nonMidi);
    part(ui::t(ui::kImpSumFailed), failed);
    return out;
}

/// 生成不覆盖已有导出的路径。时间戳只精确到秒，连续点击导出时很容易
/// 得到同名文件；软件版应保留每次用户明确发起的导出。
std::filesystem::path uniqueExportPath(const std::filesystem::path& dir,
                                       const std::string& filename) {
    std::error_code ec;
    std::filesystem::path candidate = dir / filename;
    if (!std::filesystem::exists(candidate, ec)) return candidate;

    const std::filesystem::path stem = candidate.stem();
    const std::filesystem::path ext = candidate.extension();
    for (unsigned int n = 2; n < 1000000; ++n) {
        candidate = dir / (stem.string() + "_" + std::to_string(n) + ext.string());
        ec.clear();
        if (!std::filesystem::exists(candidate, ec)) return candidate;
    }
    // Practically unreachable; return the original path so the normal save
    // error is surfaced instead of looping forever.
    return dir / filename;
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

} // namespace

// ===========================================================================
// 生命周期
// ===========================================================================

bool App::init(std::string* log) {
    anchorWorkingDirectoryToExecutable();
    migrateLegacyUserData();   // 旧版本把配置写在 cwd：存在则复制到用户数据目录
    // 语言必须在创建窗口前加载，否则已保存的英文设置会让窗口标题仍然
    // 使用中文，而窗口内部文案随后才切换，造成启动瞬间的不一致。
    loadLanguage();
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
#ifdef __APPLE__
    SetConfigFlags(FLAG_WINDOW_HIGHDPI);
#endif
    InitWindow(windowWidth_, windowHeight_, ui::t(ui::kWindowTitle));
    if (!IsWindowReady()) {
        if (log) *log += "[window] 无法创建图形窗口（请检查显卡驱动或显示会话）\n";
        return false;
    }
    // 中央舞台的图片只作为视觉底图；所有文字、按钮和交互仍由 raylib
    // 控件绘制。使用 ASCII 相对路径，兼容中文工程目录。
    for (const char* path : { "assets/ui-mockups/hero-piano-stage.png",
                              "../assets/ui-mockups/hero-piano-stage.png" }) {
        if (FileExists(path)) {
            heroTexture_ = LoadTexture(path);
            heroTextureReady_ = IsTextureValid(heroTexture_);
            if (heroTextureReady_) {
                SetTextureFilter(heroTexture_, TEXTURE_FILTER_BILINEAR);
                break;
            }
        }
    }
    for (const char* path : { "assets/ui-mockups/song-cover-sheet.png",
                              "../assets/ui-mockups/song-cover-sheet.png" }) {
        if (FileExists(path)) {
            coverSheet_ = LoadTexture(path);
            coverSheetReady_ = IsTextureValid(coverSheet_);
            if (coverSheetReady_) {
                SetTextureFilter(coverSheet_, TEXTURE_FILTER_BILINEAR);
                break;
            }
        }
    }
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

    // ---- 曲目先建好，曲名也要进入字形图集 ----
    songs_ = builtinSongs();

    // ---- 用户曲库：只加载索引，曲目以占位条目（events 为空）追加在内置曲目之后，
    //      首次选中/播放时再由 ensureSongLoaded 惰性解析 ----
    library_.load(userDataPath("library"));
    for (const auto& e : library_.entries()) {
        Song s;
        s.id         = e.id;
        s.title      = e.title;
        s.composer   = "用户文件";
        s.bpm        = e.bpm;
        s.difficulty = 2;
        s.userImport = true;
        songs_.push_back(std::move(s));
    }

    std::vector<std::string> fontSamples = ui::allStrings();
    fontSamples.push_back(library_.collectTitleChars());
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
        sampleLog_ += probe;
        if (n > 0) {
            loadedSamples = n;
            break;
        }
    }

    // ---- 输入 ----
    keyboardMap_.setBindings(kDefaultBindings.data(), kDefaultBindings.size());
    input_.setKeyboardMap(&keyboardMap_);
    input_.setSink(this);
    input_.setHitTest([this](float x, float y, int& midi, float& vel) {
        if (dropdownOpen_ || trackModalOpen_) return false;
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
    // 收藏裁决（A1.5）：favorites.txt ∪ 曲库索引 favorite=1 取并集（并集不丢收藏；
    // 取消收藏的正常路径会同时写两处，所以合并不会复活已取消的收藏）。
    // 不一致立即写回两处，使重启后一致。
    {
        const std::vector<std::string> merged = mergeFavoriteIds(library_.entries(), favorites_);
        for (const auto& id : merged) {
            const SongLibrary::Entry* e = library_.find(id);
            if (e && !e->favorite) library_.setFavorite(id, true);   // 文件有、索引没有的回写索引
        }
        if (merged != favorites_) {   // 索引有、文件没有的由并集结果覆盖 favorites.txt
            favorites_ = merged;
            saveFavorites();
        }
    }
    refreshSongList();
    if (!songs_.empty()) selectSong(0);

    // 合成器是没有外部采样时的正常回退路径；仅写入启动日志，避免每次启动
    // 都在主工作区覆盖一条提示，影响桌面端的首屏可用性。
    if (loadedSamples == 0 && log) *log += "[audio] " + std::string(ui::t(ui::kSampleNone)) + "\n";

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
    // pfd 的 executor 析构会停等用户关框：退出前先强制关掉仍开着的文件对话框
    if (importDialog_) { importDialog_->kill(); importDialog_.reset(); }
    player_.stop();
    recordedPlayer_.stop();
    device_.pushReleaseSource(Source::Playback);
    device_.pushReleaseSource(Source::Song);
    metronome_.setEnabled(false);
    device_.stop();
    mixer_.setVisualEventQueue(nullptr);   // 设备停止后摘除视觉事件队列
    fxLayer_.unload();
    if (heroTextureReady_) {
        UnloadTexture(heroTexture_);
        heroTexture_ = {};
        heroTextureReady_ = false;
    }
    if (coverSheetReady_) {
        UnloadTexture(coverSheet_);
        coverSheet_ = {};
        coverSheetReady_ = false;
    }
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
    // 搜索框焦点 / 下拉展开 / 模态弹窗时，电脑键盘不用于演奏
    input_.setKeyboardEnabled(!searchFocused_ && !dropdownOpen_ && !trackModalOpen_);

    input_.update();
    pianoView_.updateScrollAnimation(dt);

    // 键位提示默认隐藏，F1 可在演奏教学与纯视觉模式之间切换。
    if (!searchFocused_ && !dropdownOpen_ && !trackModalOpen_ && IsKeyPressed(KEY_F1)) {
        showKeyHints_ = !showKeyHints_;
    }

    // ---- 轨道选择模态的键盘确认/取消 ----
    if (trackModalOpen_) {
        if (IsKeyPressed(KEY_ENTER)) {
            const int track = (trackModalSel_ >= 0 &&
                               trackModalSel_ < static_cast<int>(trackModalTracks_.size()))
                ? trackModalTracks_[static_cast<std::size_t>(trackModalSel_)] : -1;
            const std::filesystem::path path = trackModalPath_;
            trackModalOpen_ = false;
            finishImport(path, track >= 0 ? std::optional<int>(track) : std::nullopt);
        } else if (IsKeyPressed(KEY_ESCAPE)) {
            trackModalOpen_ = false;
            toast(ui::t(ui::kToastImportCancel));
        }
    }

    // ---- 系统文件对话框（pfd）结果轮询 ----
    if (importDialog_ && importDialog_->ready()) {
        const std::vector<std::string> picked = importDialog_->result();
        importDialog_.reset();
        if (!picked.empty()) {
            // pfd 返回 UTF-8 路径，按字节构造 fs::path，中文文件名不走 ANSI
            const std::string& p = picked.front();
            beginImport(std::filesystem::path(std::u8string(p.begin(), p.end())));
        }
    }

    // ---- 滚轮滚动琴键视口 ----
    const Vector2 mouse = ui_.mouse;
    if (!dropdownOpen_ && !trackModalOpen_ && CheckCollisionPointRec(mouse, layout_.keys)) {
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
        if (files.count == 1 && !trackModalOpen_) {
            // 单文件：交互式导入（可能弹轨道选择模态；重复时定位已有条目）
            const std::filesystem::path p(files.paths[0]);
            if (hasMidiExtension(p)) beginImport(p);
            else toast(formatInt(ui::t(ui::kImpSumNonMidi), 1));
        } else {
            // 批量：扩展名过滤 + 逐文件入库，最后汇总成一条 toast。
            // 多轨文件在批量模式下不再逐个弹窗，旋律轨按自动判定。
            int imported = 0, dup = 0, nonMidi = 0, failed = 0;
            for (unsigned i = 0; i < files.count; ++i) {
                const std::filesystem::path p(files.paths[i]);
                if (!hasMidiExtension(p)) { ++nonMidi; continue; }
                using Code = SongLibrary::AddResult::Code;
                const Code c = finishImport(p, std::nullopt, true);
                if (c == Code::Ok) ++imported;
                else if (c == Code::Duplicate) ++dup;
                else ++failed;
            }
            toast(importSummary(imported, dup, nonMidi, failed));
        }
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
    // 导入的新曲目标题可能带来图集外字符：同一防抖节奏重烘焙字体
    if (fontRebakeTimer_ >= 0.0f) {
        fontRebakeTimer_ += dt;
        if (fontRebakeTimer_ >= 0.15f) {
            fonts_.load(fontSamples_, fontScale_);
            fontRebakeTimer_ = -1.0f;
        }
    }
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
    // 模态开启时整帧吃掉背景控件的点击（模态自己的按钮用原始输入状态判定）
    if (trackModalOpen_) ui_.consumedClick = true;
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
    widget::softShadow(r, 0.12f, 4.0f, 32);
    widget::panel(r, Color{ 12, 22, 35, 235 }, Color{ 42, 59, 79, 255 }, 0.12f);

    // 徽标：深色铜牌上五条高低错落的金线，保持与原版品牌一致。
    const Rectangle logo{ r.x + 10.0f, r.y + 9.0f, 40.0f, 40.0f };
    widget::panel(logo, Color{ 46, 36, 24, 255 }, Color{ 148, 124, 84, 255 }, 0.24f);
    widget::bevel(logo, 26, 30);
    const float barHeights[5] = { 10.0f, 17.0f, 13.0f, 19.0f, 11.0f };
    for (int i = 0; i < 5; ++i) {
        const float bx = logo.x + 8.0f + static_cast<float>(i) * 6.0f;
        const float half = barHeights[i] * 0.5f;
        const float cy = logo.y + logo.height * 0.5f;
        DrawLineEx({ bx, cy - half }, { bx, cy + half }, 2.4f,
                    (i % 2 == 0) ? theme::kGold : theme::kGoldLight);
    }

    fonts_.drawTitle(ui::t(ui::kBrandTitle), logo.x + 54.0f, r.y + 8.0f, theme::kInk);
    fonts_.drawSmall(ui::t(ui::kBrandSub), logo.x + 56.0f, r.y + 39.0f, theme::kMutedDim);

    const char* range = ui::t(ui::kInstrumentSub);
    fonts_.drawSmall(range, r.x + r.width - 600.0f, r.y + 22.0f, theme::kMuted);

    // 状态胶囊、设置/电平图标和语言按钮组成右侧工具区。
    const std::string st = statusText_;
    const float tw = fonts_.smallWidth(st.c_str());
    const float pillW = std::clamp(tw + 42.0f, 150.0f, 230.0f);
    const Rectangle pill{ r.x + r.width - pillW - 168.0f, r.y + 13.0f, pillW, 32.0f };
    widget::panel(pill, Color{ 15, 23, 35, 220 }, Color{ 58, 72, 92, 255 }, 0.5f);
    const Vector2 dot{ pill.x + 16.0f, pill.y + pill.height * 0.5f };
    // 当前版本没有实时 MIDI 设备枚举服务，状态点不伪装成“已连接”；
    // 仅在确实发生错误时使用警告色，正常工作状态保持中性。
    const Color dotColor = statusWarn_ ? theme::kWarn : Color{ 145, 160, 177, 255 };
    DrawCircleV(dot, 3.5f, dotColor);
    fonts_.drawSmall(st.c_str(), pill.x + 27.0f, pill.y + 9.0f, Color{ 184, 196, 210, 255 });

    const Rectangle tools{ pill.x + pill.width + 10.0f, pill.y, 92.0f, pill.height };
    widget::panel(tools, Color{ 15, 23, 35, 220 }, Color{ 58, 72, 92, 255 }, 0.35f);
    DrawLineEx({ tools.x + 46.0f, tools.y + 7.0f }, { tools.x + 46.0f, tools.y + 25.0f },
               1.0f, Color{ 100, 116, 139, 180 });
    DrawCircleV({ tools.x + 23.0f, tools.y + 16.0f }, 5.0f, theme::kMuted);
    DrawLineEx({ tools.x + 16.0f, tools.y + 16.0f }, { tools.x + 30.0f, tools.y + 16.0f },
               2.0f, theme::kPanel);
    for (int i = 0; i < 3; ++i)
        DrawRectangleRounded({ tools.x + 60.0f + i * 8.0f, tools.y + 20.0f - i * 5.0f,
                               4.0f, 8.0f + i * 5.0f }, 0.25f, 3, theme::kMuted);

    const Rectangle languageButton{ r.x + r.width - 58.0f, r.y + 13.0f, 48.0f, 32.0f };
    const char* languageLabel = ui::language() == ui::Language::Chinese ? "EN" : "中文";
    if (widget::button(ui_, 9000, languageButton, languageLabel, false, true)) {
        ui::setLanguage(ui::language() == ui::Language::Chinese
                            ? ui::Language::English : ui::Language::Chinese);
        saveLanguage();
        setStatus(ui::t(ui::kStatusAudioOk));
    }
}

void App::drawHero() {
    // 顶栏已经承载品牌与状态；首屏主视觉在 drawHeroPanel() 中绘制。
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
    // 三栏首屏：曲库、品牌舞台、音色控制。
    drawLibraryPane();
    drawHeroPanel();
    drawControlsRow();
    drawKeyboardSection();
    drawTransportRow();
    drawSongPanel();
}

void App::drawLibraryPane() {
    const Rectangle& r = layout_.library;
    widget::softShadow(r, 0.08f, 5.0f, 46);
    widget::panel(r, Color{ 13, 23, 36, 248 }, theme::kLine, 0.08f);

    const float navW = 126.0f;
    DrawRectangleRounded({ r.x, r.y, navW, r.height }, 0.08f, 8, Color{ 11, 20, 32, 180 });
    fonts_.drawBody("音", r.x + 18.0f, r.y + 18.0f, theme::kInk);
    fonts_.drawBody("曲库", r.x + 48.0f, r.y + 17.0f, theme::kInk);

    const char* nav[] = { "全部曲目", "经典古典", "流行音乐", "影视原声", "练习曲目", "我的收藏" };
    for (int i = 0; i < 6; ++i) {
        const Rectangle item{ r.x + 8.0f, r.y + 56.0f + static_cast<float>(i) * 42.0f,
                              navW - 16.0f, 34.0f };
        const bool active = (i == 5 && showFavoritesOnly_) ||
                            (i == 0 && !showFavoritesOnly_ && difficultyFilter_ == 0) ||
                            (i == 1 && !showFavoritesOnly_ && difficultyFilter_ == 1) ||
                            (i == 2 && !showFavoritesOnly_ && difficultyFilter_ == 2);
        if (active) widget::panel(item, Color{ 150, 112, 58, 128 }, Color{ 206, 162, 96, 255 }, 0.18f);
        const char* icon = i == 0 ? "曲" : (i == 5 ? "★" : "·");
        fonts_.drawBody(icon, item.x + 14.0f, item.y + 7.0f,
                        active ? theme::kGoldLight : theme::kMuted);
        fonts_.drawSmall(nav[i], item.x + 42.0f, item.y + 9.0f,
                         active ? theme::kInk : theme::kMuted);
        if (ui_.mousePressed && ui_.hovered(item) && !ui_.consumedClick) {
            showFavoritesOnly_ = i == 5;
            if (i == 0) difficultyFilter_ = 0;
            else if (i == 1) difficultyFilter_ = 1;
            else if (i == 2) difficultyFilter_ = 2;
            else if (i >= 3 && i <= 4) difficultyFilter_ = 0;
            refreshSongList();
            ui_.consumedClick = true;
        }
    }

    const float x = r.x + navW + 14.0f;
    const float rightW = r.width - navW - 24.0f;
    const Rectangle search{ x, r.y + 14.0f, rightW, 34.0f };
    widget::panel(search, searchFocused_ ? Color{ 26, 38, 55, 255 } : Color{ 19, 29, 43, 255 },
                  searchFocused_ ? theme::kGold : theme::kLineSoft, 0.22f);
    DrawCircleLines(static_cast<int>(search.x + 18.0f), static_cast<int>(search.y + 16.0f),
                    5.0f, theme::kMuted);
    DrawLineEx({ search.x + 22.0f, search.y + 20.0f }, { search.x + 26.0f, search.y + 24.0f },
               1.5f, theme::kMuted);
    if (ui_.mousePressed && ui_.hovered(search) && !ui_.consumedClick) {
        searchFocused_ = true;
        ui_.consumedClick = true;
    } else if (ui_.mousePressed && !ui_.hovered(search) && !ui_.consumedClick && searchFocused_) {
        searchFocused_ = false;
        refreshSongList();
        ui_.consumedClick = true;
    }
    const std::string shown = searchText_.empty() && !searchFocused_
        ? std::string(ui::t(ui::kSearchHint))
        : searchText_ + (searchFocused_ ? "|" : "");
    fonts_.drawSmall(shown.c_str(), search.x + 34.0f, search.y + 10.0f,
                     searchText_.empty() && !searchFocused_ ? theme::kMutedDim : theme::kInk);

    const int visible = std::min(5, static_cast<int>(songView_.size()));
    const float listY = search.y + search.height + 10.0f;
    for (int rowIndex = 0; rowIndex < visible; ++rowIndex) {
        const int songIndex = songView_[static_cast<std::size_t>(rowIndex)];
        const Song& song = songs_[static_cast<std::size_t>(songIndex)];
        const Rectangle row{ x, listY + static_cast<float>(rowIndex) * 50.0f, rightW, 44.0f };
        const bool active = songIndex == songIndex_;
        const bool hover = ui_.hovered(row);
        if (active || hover)
            widget::panel(row, active ? Color{ 59, 48, 35, 230 } : Color{ 26, 38, 55, 230 },
                          active ? theme::kGold : theme::kLineSoft, 0.16f);
        const Rectangle thumb{ row.x + 6.0f, row.y + 5.0f, 42.0f, 34.0f };
        widget::panel(thumb, Color{ 28, 42, 58, 255 }, Color{ 70, 88, 110, 180 }, 0.12f);
        if (coverSheetReady_) {
            static constexpr Rectangle coverRects[5] = {
                { 14.0f, 8.0f, 490.0f, 493.0f },
                { 520.0f, 8.0f, 490.0f, 493.0f },
                { 14.0f, 514.0f, 490.0f, 499.0f },
                { 520.0f, 514.0f, 490.0f, 499.0f },
                { 14.0f, 1027.0f, 490.0f, 499.0f }
            };
            const Rectangle src = coverRects[static_cast<std::size_t>(std::min(rowIndex, 4))];
            DrawTexturePro(coverSheet_, src, thumb, { 0.0f, 0.0f }, 0.0f, WHITE);
            DrawRectangleRoundedLinesEx(thumb, 0.12f, 5, 1.0f,
                                        active ? Color{ 238, 194, 118, 190 } : Color{ 70, 88, 110, 180 });
        } else {
            DrawCircleGradient({ thumb.x + 24.0f, thumb.y + 16.0f }, 11.0f,
                               Color{ 213, 173, 106, 55 }, Color{ 20, 33, 48, 0 });
        }
        fonts_.drawSmall(song.title.c_str(), row.x + 56.0f, row.y + 8.0f,
                         active ? theme::kInk : Color{ 205, 214, 226, 255 });
        fonts_.drawSmall(song.composer.c_str(), row.x + 56.0f, row.y + 25.0f, theme::kMuted);
        if (isFavorite(songIndex)) {
            DrawPoly({ row.x + row.width - 22.0f, row.y + 21.0f }, 5, 6.5f, 0.0f, theme::kGoldLight);
        }
        if (hover && ui_.mousePressed && !ui_.consumedClick) {
            selectSong(songIndex);
            ui_.consumedClick = true;
        }
    }
}

void App::drawHeroPanel() {
    const Rectangle& r = layout_.heroPanel;
    widget::softShadow(r, 0.08f, 5.0f, 48);
    widget::panel(r, Color{ 12, 22, 35, 255 }, theme::kLine, 0.08f);

    widget::scissorLogical(ui_, r);
    if (heroTextureReady_) {
        const Rectangle src{ 0.0f, 0.0f, static_cast<float>(heroTexture_.width),
                             static_cast<float>(heroTexture_.height) };
        DrawTexturePro(heroTexture_, src, r, { 0.0f, 0.0f }, 0.0f, WHITE);
    }
    DrawRectangleGradientV(static_cast<int>(r.x), static_cast<int>(r.y),
                           static_cast<int>(r.width), static_cast<int>(r.height),
                           Color{ 4, 10, 18, 155 }, Color{ 4, 10, 18, 72 });
    DrawRectangleGradientV(static_cast<int>(r.x), static_cast<int>(r.y + r.height * 0.58f),
                           static_cast<int>(r.width), static_cast<int>(r.height * 0.42f),
                           Color{ 4, 10, 18, 20 }, Color{ 4, 10, 18, 190 });

    const char* title = ui::t(ui::kBrandTitle);
    const float tw = fonts_.titleWidth(title);
    fonts_.drawTitle(title, r.x + (r.width - tw) * 0.5f, r.y + 132.0f, theme::kGoldLight);
    const char* sub = ui::t(ui::kBrandSub);
    const float sw = fonts_.bodyWidth(sub);
    fonts_.drawBody(sub, r.x + (r.width - sw) * 0.5f, r.y + 180.0f, theme::kInk);
    const float iw = fonts_.smallWidth(ui::t(ui::kInstrumentSub));
    fonts_.drawSmall(ui::t(ui::kInstrumentSub), r.x + (r.width - iw) * 0.5f,
                     r.y + 218.0f, Color{ 206, 215, 227, 225 });

    // 金色点阵波形，呼应参考图和音频波形。
    const float cy = r.y + r.height - 48.0f;
    for (int i = 0; i < 52; ++i) {
        const float t = static_cast<float>(i) / 51.0f;
        const float wave = std::sin(t * 8.0f + 0.45f) * 12.0f + std::sin(t * 18.0f) * 5.0f;
        const float amp = (1.0f - std::fabs(2.0f * t - 1.0f) * 0.38f);
        DrawCircleV({ r.x + 20.0f + t * (r.width - 40.0f), cy + wave },
                    1.7f, Color{ 246, 210, 150, static_cast<unsigned char>(140 * amp) });
        if (i % 2 == 0)
            DrawLineEx({ r.x + 20.0f + t * (r.width - 40.0f), cy + wave },
                       { r.x + 20.0f + t * (r.width - 40.0f), cy + wave + 9.0f * amp },
                       1.0f, Color{ 213, 173, 106, static_cast<unsigned char>(95 * amp) });
    }
    EndScissorMode();

    widget::panel({ r.x + 12.0f, r.y + 12.0f, 92.0f, 26.0f },
                   Color{ 8, 15, 25, 160 }, Color{ 120, 104, 78, 120 }, 0.2f);
    fonts_.drawSmall(ui::t(ui::kEyebrow), r.x + 22.0f, r.y + 19.0f, theme::kGoldLight);
}

void App::drawControlsLegacy() {
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

void App::drawControlsRow() {
    const Rectangle& r = layout_.controls;
    widget::softShadow(r, 0.08f, 5.0f, 46);
    widget::panel(r, Color{ 13, 23, 36, 248 }, theme::kLine, 0.08f);
    fonts_.drawBody("控制台", r.x + 22.0f, r.y + 18.0f, theme::kInk);
    fonts_.drawSmall("音色、音量与演奏设置", r.x + 22.0f, r.y + 44.0f, theme::kMuted);

    const float x = r.x + 22.0f;
    const float w = r.width - 44.0f;
    const float y0 = r.y + 64.0f;
    // 右侧控制台保持四个独立的视觉层级：音色、音量、混响、演奏扩展。
    // 控件仍共享同一套状态与命中区域，只增加卡片底色和描边。
    const Color cardFill{ 16, 28, 43, 190 };
    const Color cardLine{ 42, 59, 79, 210 };
    widget::panel({ x - 8.0f, y0 - 8.0f, w + 16.0f, 60.0f }, cardFill, cardLine, 0.14f);
    widget::panel({ x - 8.0f, y0 + 56.0f, w + 16.0f, 48.0f }, cardFill, cardLine, 0.14f);
    widget::panel({ x - 8.0f, y0 + 104.0f, w + 16.0f, 48.0f }, cardFill, cardLine, 0.14f);
    widget::panel({ x - 8.0f, y0 + 160.0f, w + 16.0f, 136.0f }, cardFill, cardLine, 0.14f);
    fonts_.drawSmall(ui::t(ui::kEngineLabel), x, y0, theme::kMutedDim);
    {
        const std::vector<std::string> items{ ui::t(ui::kEngineAuto), ui::t(ui::kEngineSample),
                                              ui::t(ui::kEngineSynth) };
        int idx = engineMode_;
        if (widget::dropdown(ui_, 1101, { x, y0 + 18.0f, w, 34.0f }, items, idx,
                             dropdownOpen_, dropdownId_)) {
            engineMode_ = idx;
            mixer_.setEngineMode(static_cast<Mixer::EngineMode>(idx));
            toast(idx == 2 ? ui::t(ui::kEngineSynth) : (idx == 1 ? ui::t(ui::kEngineSample)
                                                                  : ui::t(ui::kEngineAuto)));
        }
    }

    float volume = volume_ * 100.0f;
    if (widget::slider(ui_, 1102, { x, y0 + 64.0f, w, 34.0f }, ui::t(ui::kVolumeLabel),
                       percent(volume_).c_str(), volume, 0.0f, 100.0f)) {
        volume_ = volume / 100.0f;
        mixer_.setMasterVolume(volume_);
    }

    float reverb = reverb_ * 100.0f;
    if (widget::slider(ui_, 1103, { x, y0 + 112.0f, w, 34.0f }, ui::t(ui::kReverbLabel),
                       percent(reverb_).c_str(), reverb, 0.0f, 70.0f)) {
        reverb_ = reverb / 100.0f;
        mixer_.setReverbMix(reverb_);
    }

    const Rectangle pedal{ x, y0 + 170.0f, 126.0f, 36.0f };
    if (widget::button(ui_, 1104, pedal, ui::t(ui::kPedalWord), false, true, sustainUser_))
        input_.setSustainManual(!input_.sustainManual());
    fonts_.drawSmall(ui::t(ui::kPedalLabel), x + 142.0f, y0 + 176.0f, theme::kMutedDim);
    fonts_.drawSmall(sustainUser_ ? ui::t(ui::kOn) : ui::t(ui::kOff), x + 142.0f,
                     y0 + 193.0f, sustainUser_ ? theme::kGoldLight : theme::kMuted);

    fonts_.drawSmall(ui::t(ui::kFxLabel), x, y0 + 200.0f, theme::kMutedDim);
    {
        const std::vector<std::string> items{ ui::t(ui::kFxOff), ui::t(ui::kFxLow),
                                              ui::t(ui::kFxStandard), ui::t(ui::kFxHigh) };
        int idx = fxLevel_;
        if (widget::dropdown(ui_, 1105, { x, y0 + 218.0f, w, 34.0f }, items, idx,
                             dropdownOpen_, dropdownId_)) {
            fxLevel_ = idx;
            applyFxSettings();
        }
    }
    if (widget::checkbox(ui_, { x, y0 + 266.0f, w, 22.0f },
                         ui::t(ui::kFxReducedMotion), fxReducedMotion_)) {
        fxReducedMotion_ = !fxReducedMotion_;
        applyFxSettings();
    }
}

void App::drawKeyboardSection() {
    drawFxStage();
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

    pianoView_.setShowKeyHints(showKeyHints_);
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
    const float gap = 8.0f;
    const float by = r.y + 12.0f;
    const float bh = 48.0f;

    const Rectangle rec{ x, by, 126.0f, bh };
    if (widget::button(ui_, 2101, rec, recording_ ? ui::t(ui::kRecordStop) : ui::t(ui::kRecordStart),
                       !recording_, true, recording_)) {
        if (recording_) stopRecording(); else startRecording();
    }
    x += rec.width + gap;
    if (widget::button(ui_, 2102, { x, by, 100.0f, bh }, ui::t(ui::kPlayback), false,
                       recorder_.hasNotes() && !recording_ && !playingRecording_)) startPlayback();
    x += 100.0f + gap;
    if (widget::button(ui_, 2103, { x, by, 90.0f, bh }, ui::t(ui::kStop), false,
                       recording_ || playingRecording_ || songPlaying_)) {
        if (recording_) stopRecording();
        if (playingRecording_) stopPlayback();
        if (songPlaying_) stopSongPlayback();
    }
    x += 90.0f + gap;
    if (widget::button(ui_, 2104, { x, by, 118.0f, bh }, ui::t(ui::kExportMidi), false,
                       recorder_.hasNotes() && !recording_)) exportRecordingMidi();
    x += 118.0f + 16.0f;

    DrawLineEx({ x - 8.0f, by + 4.0f }, { x - 8.0f, by + bh - 4.0f }, 1.0f,
               Color{ 255, 255, 255, 20 });
    if (widget::button(ui_, 2105, { x, by, 110.0f, bh }, ui::t(ui::kPlaySong), true,
                       !songs_.empty() && !songPlaying_)) startSongPlayback();
    x += 110.0f + gap;
    if (widget::button(ui_, 2106, { x, by, 98.0f, bh }, ui::t(ui::kStopSong), false,
                       songPlaying_)) stopSongPlayback();
    x += 98.0f + gap;

    const std::vector<std::string> modes{ ui::t(ui::kModeFull), ui::t(ui::kModeAccomp),
                                          ui::t(ui::kModeGuide) };
    int mode = practiceMode_;
    if (widget::dropdown(ui_, 2107, { x, by, 130.0f, bh }, modes, mode,
                         dropdownOpen_, dropdownId_)) {
        practiceMode_ = mode;
        applyPracticeMode();
        if (mode == 0) judge_.reset();
    }
    x += 130.0f + 14.0f;

    const Rectangle metro{ x, by, 112.0f, bh };
    if (widget::button(ui_, 2108, metro, metronomeOn_ ? ui::t(ui::kMetronomeOff) : ui::t(ui::kMetronome),
                       false, true, metronomeOn_)) {
        metronomeOn_ = !metronomeOn_;
        metronome_.setEnabled(metronomeOn_ || practiceMode_ == 2);
    }
    x += metro.width + gap;
    float bpm = bpmF_;
    if (widget::slider(ui_, 2109, { x, by, 170.0f, bh }, ui::t(ui::kBpm),
                       std::to_string(static_cast<int>(std::lround(bpmF_))).c_str(), bpm,
                       40.0f, 200.0f)) {
        bpmF_ = bpm;
        metronome_.setBpm(static_cast<int>(std::lround(bpmF_)));
    }

    const Rectangle wave{ r.x + r.width - 352.0f, r.y + 8.0f, 332.0f, 64.0f };
    widget::panel(wave, Color{ 11, 21, 34, 220 }, Color{ 42, 59, 79, 220 }, 0.12f);
    const float peak = std::clamp(mixer_.peakLevel(), 0.02f, 1.0f);
    for (int i = 0; i < 64; ++i) {
        const std::size_t sample = scope_.empty() ? 0 :
            std::min(scope_.size() - 1, static_cast<std::size_t>(i * scope_.size() / 64));
        const float level = scope_.empty() ? peak : std::clamp(scope_[sample], 0.0f, 1.0f);
        const float idleWave = std::fabs(std::sin(static_cast<float>(i) * 0.57f)) * 0.16f +
                               std::fabs(std::sin(static_cast<float>(i) * 0.19f + 1.4f)) * 0.08f;
        const float h = 5.0f + std::max(level * 34.0f, idleWave * 24.0f);
        const float xx = wave.x + 12.0f + static_cast<float>(i) * (wave.width - 24.0f) / 63.0f;
        DrawLineEx({ xx, wave.y + wave.height * 0.5f - h * 0.5f },
                   { xx, wave.y + wave.height * 0.5f + h * 0.5f }, 1.3f,
                   Color{ 182, 211, 235, static_cast<unsigned char>(90 + level * 150.0f) });
    }
    DrawRectangleRounded({ r.x, r.y + r.height - 5.0f, r.width, 3.0f }, 1.0f, 4,
                          Color{ 32, 45, 61, 255 });
    const float progress = recording_ ? std::fmod(static_cast<double>(mixer_.currentFrame()) /
                                                   std::max(1.0, sampleRate_), 30.0) / 30.0
                                      : (playingRecording_ ? recordedPlayer_.progress()
                                                            : (songPlaying_ ? static_cast<float>(player_.progress()) : 0.0f));
    DrawRectangleRounded({ r.x, r.y + r.height - 5.0f, r.width * std::clamp(progress, 0.0f, 1.0f), 3.0f },
                         1.0f, 4, theme::kGold);
}

void App::drawTransportLegacy() {
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
    widget::softShadow(r, 0.08f, 5.0f, 44);
    widget::panel(r, Color{ 11, 22, 35, 248 }, theme::kLine, 0.08f);

    const float navW = 132.0f;
    DrawRectangleRounded({ r.x, r.y, navW, r.height }, 0.08f, 8, Color{ 10, 19, 31, 190 });
    const char* tabs[] = { "乐谱跟练", "钢琴卷帘", "练习统计" };
    for (int i = 0; i < 3; ++i) {
        const Rectangle tab{ r.x + 10.0f, r.y + 16.0f + static_cast<float>(i) * 48.0f,
                             navW - 20.0f, 38.0f };
        if (widget::button(ui_, 2200 + i, tab, tabs[i], i == scoreTab_, true, i == scoreTab_))
            scoreTab_ = i;
    }
    fonts_.drawSmall("跟练模式", r.x + 20.0f, r.y + r.height - 27.0f, theme::kMuted);

    const float x = r.x + navW + 18.0f;
    const float w = r.width - navW - 30.0f;
    const std::string title = songs_.empty() ? std::string("跟练谱面")
                                             : songs_[static_cast<std::size_t>(songIndex_)].title;
    fonts_.drawBody(title.c_str(), x, r.y + 12.0f, theme::kInk);
    const std::string meta = songs_.empty() ? std::string("选择曲目开始练习")
                                            : (std::to_string(songs_[static_cast<std::size_t>(songIndex_)].bpm) + " BPM");
    fonts_.drawSmall(meta.c_str(), x, r.y + 34.0f, theme::kMuted);

    const float staffX = x + 4.0f;
    const float staffY = r.y + 64.0f;
    const float staffW = w - 8.0f;
    const float lineGap = 9.0f;
    for (int staff = 0; staff < 2; ++staff) {
        const float sy = staffY + static_cast<float>(staff) * 55.0f;
        for (int line = 0; line < 5; ++line)
            DrawLineEx({ staffX, sy + line * lineGap },
                       { staffX + staffW, sy + line * lineGap }, 1.0f,
                       Color{ 126, 143, 163, 150 });
        DrawLineEx({ staffX + 4.0f, sy - 4.0f }, { staffX + 4.0f, sy + 4.0f * lineGap + 4.0f },
                   1.0f, Color{ 174, 188, 204, 180 });
    }
    fonts_.drawTitle("G", staffX + 8.0f, staffY - 3.0f, Color{ 233, 237, 244, 225 });
    fonts_.drawTitle("F", staffX + 8.0f, staffY + 52.0f, Color{ 233, 237, 244, 190 });

    if (!songs_.empty()) {
        const Song& song = songs_[static_cast<std::size_t>(songIndex_)];
        const std::uint64_t duration = std::max<std::uint64_t>(1, song.durationTicks());
        int drawn = 0;
        for (const auto& event : song.events) {
            if (event.track != kTrackMelody || drawn >= 32) continue;
            const float nx = staffX + 46.0f + static_cast<float>(event.tick) /
                             static_cast<float>(duration) * (staffW - 60.0f);
            const int pitch = std::clamp(static_cast<int>(event.note), 48, 84);
            const float ny = staffY + 4.0f * lineGap - (pitch - 60) * 1.7f;
            DrawEllipse(static_cast<int>(nx), static_cast<int>(ny), 5.5f, 3.8f,
                        event.velocity > 100 ? theme::kGoldLight : theme::kInk);
            DrawLineEx({ nx + 4.0f, ny }, { nx + 4.0f, ny - 22.0f }, 1.3f, theme::kInk);
            if (event.duration > 360)
                DrawLineEx({ nx + 4.0f, ny - 22.0f }, { nx + 16.0f, ny - 22.0f }, 1.2f, theme::kInk);
            ++drawn;
        }
    }

    if (songPlaying_) {
        const float px = staffX + std::clamp(static_cast<float>(player_.progress()), 0.0f, 1.0f) * staffW;
        DrawLineEx({ px, staffY - 7.0f }, { px, staffY + 102.0f }, 2.0f,
                    Color{ 246, 210, 150, 210 });
        DrawCircleGradient({ px, staffY - 7.0f }, 8.0f, Color{ 246, 210, 150, 100 },
                           Color{ 246, 210, 150, 0 });
    }

    const float statY = r.y + r.height - 28.0f;
    fonts_.drawSmall(ui::t(ui::kJudgeTitle), x, statY, theme::kMutedDim);
    const std::string stats = std::string(ui::t(ui::kPerfect)) + " " + std::to_string(judge_.perfect()) +
        "    " + ui::t(ui::kGood) + " " + std::to_string(judge_.good()) +
        "    " + ui::t(ui::kMiss) + " " + std::to_string(judge_.miss()) +
        "    " + ui::t(ui::kAccuracy) + " " + std::to_string(static_cast<int>(std::lround(judge_.accuracy() * 100.0f))) + "%";
    fonts_.drawSmall(stats.c_str(), x + 84.0f, statY, theme::kGoldLight);
}

void App::drawSongPanelLegacy() {
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

    // ---- 第一行：搜索 / 难度 / 曲名 / 播放 / 停止 / 收藏 / 模式 / ⋯ ----
    // （导出曲目与只看收藏收进 ⋯ 菜单，消除非紧凑布局的横向溢出）
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
        const float favX = stopX + stopW + 12.0f;
        const float favW = compact ? 96.0f : 108.0f;
        const float modeX = favX + favW + 12.0f;
        const float modeW = compact ? 140.0f : 160.0f;
        const float moreX = modeX + modeW + 12.0f;
        const float moreW = compact ? 36.0f : 40.0f;
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

        const bool fav = isFavorite(songIndex_);
        if (widget::button(ui_, 3007, { favX, y, favW, 32.0f },
                           fav ? ui::t(ui::kFavRemove) : ui::t(ui::kFavAdd),
                           false, !songs_.empty(), fav)) {
            toggleFavorite(songIndex_);
        }

        const std::vector<std::string> modes{ ui::t(ui::kModeFull), ui::t(ui::kModeAccomp), ui::t(ui::kModeGuide) };
        int mi = practiceMode_;
        if (widget::dropdown(ui_, 3006, { modeX, y, modeW, 32.0f }, modes, mi, dropdownOpen_, dropdownId_)) {
            practiceMode_ = mi;
            applyPracticeMode();
            if (mi == 0) judge_.reset();
            toast(modes[static_cast<std::size_t>(mi)]);
        }

        // ⋯ 菜单按钮：画三个圆点，不依赖字体里有没有「⋯」字形
        const Rectangle more{ moreX, y, moreW, 32.0f };
        const bool menuOpen = dropdownOpen_ && dropdownId_ == 3011;
        {
            const bool mHover = ui_.hovered(more);
            widget::panel(more,
                          (mHover || menuOpen) ? Color{ 26, 38, 55, 255 } : Color{ 19, 29, 43, 255 },
                          menuOpen ? theme::kGold
                                   : (mHover ? Color{ 88, 104, 128, 255 } : theme::kLineSoft), 0.28f);
            widget::bevel(more, 12, 20);
            const float cxm = more.x + more.width * 0.5f;
            const float cym = more.y + more.height * 0.5f;
            for (int d = -1; d <= 1; ++d)
                DrawCircleV({ cxm + static_cast<float>(d) * 8.0f, cym }, 2.2f,
                            (mHover || menuOpen) ? theme::kGoldLight : theme::kMuted);
            if (mHover && ui_.mousePressed && !ui_.consumedClick) {
                ui_.consumedClick = true;
                dropdownOpen_ = !menuOpen;
                dropdownId_ = dropdownOpen_ ? 3011 : 0;
                ui_.dropdownScroll = 0.0f;
            }
        }

        // ⋯ 菜单：导入 MIDI… / 导出曲目 / 只看收藏
        if (menuOpen) {
            const std::vector<std::string> items{
                ui::t(ui::kMenuImport), ui::t(ui::kExportSong), ui::t(ui::kFavOnly)
            };
            const float rowH = widget::kDropdownRowH;
            const float listW = compact ? 168.0f : 190.0f;
            const float listH = rowH * static_cast<float>(items.size()) + 8.0f;
            Rectangle list{ more.x + more.width - listW, y + 36.0f, listW, listH };
            if (list.x < r.x + 6.0f) list.x = r.x + 6.0f;
            if (list.y + list.height > ui_.logicalHeight - 8.0f)
                list.y = y - listH - 4.0f;

            widget::panel(list, Color{ 15, 23, 35, 250 }, Color{ 62, 76, 96, 255 }, 0.08f);
            int picked = -1;
            for (int i = 0; i < static_cast<int>(items.size()); ++i) {
                const Rectangle row{ list.x + 4.0f, list.y + 4.0f + rowH * static_cast<float>(i),
                                     list.width - 8.0f, rowH };
                const bool rowHover = ui_.hovered(row);
                if (rowHover) {
                    DrawRectangleRounded(row, 0.2f, 4, Color{ 213, 173, 106, 22 });
                    DrawRectangleRounded({ row.x, row.y + 4.0f, 2.5f, row.height - 8.0f }, 1.0f, 2,
                                         Color{ 213, 173, 106, 190 });
                }
                // 只看收藏是开关项：开启时该行用金色标出
                const bool mark = (i == 2 && showFavoritesOnly_);
                fonts_.drawSmall(items[static_cast<std::size_t>(i)].c_str(),
                                 row.x + 8.0f, row.y + 7.0f,
                                 mark ? theme::kGoldLight : theme::kInk);
                if (rowHover && ui_.mousePressed) { picked = i; ui_.consumedClick = true; }
            }
            // 点击菜单外关闭
            if (ui_.mousePressed && !ui_.consumedClick &&
                !widget::pointIn(list, ui_.mouse) && !widget::pointIn(more, ui_.mouse)) {
                dropdownOpen_ = false;
                dropdownId_ = 0;
                ui_.consumedClick = true;
            }
            // 复用下拉顶绘机制，让菜单浮在琴键与说明卡之上
            ui_.deferredDropdowns.push_back({ list, items, showFavoritesOnly_ ? 2 : -1, 0.0f });
            if (picked >= 0) {
                dropdownOpen_ = false;
                dropdownId_ = 0;
                if (picked == 0) openImportDialog();
                else if (picked == 1) exportSongMidi();
                else { showFavoritesOnly_ = !showFavoritesOnly_; refreshSongList(); }
            }
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
    if (r.height < 60.0f) {
        widget::panel(r, Color{ 12, 22, 35, 220 }, Color{ 42, 59, 79, 220 }, 0.12f);
        fonts_.drawSmall("?", r.x + 18.0f, r.y + 10.0f, theme::kGoldLight);
        fonts_.drawSmall("选择曲目后点击播放曲目，或开启跟练判定开始练习。",
                         r.x + 44.0f, r.y + 10.0f, theme::kMuted);
        const char* shortcut = "F1 键位提示   空格 延音   R 录制   M 节拍器   ← → 切换曲目";
        const float sw = fonts_.smallWidth(shortcut);
        fonts_.drawSmall(shortcut, r.x + r.width - sw - 18.0f, r.y + 10.0f, theme::kMutedDim);
        return;
    }
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
    if (toastTimer_ > 0.0f && !toastText_.empty()) {        const float tw = fonts_.bodyWidth(toastText_.c_str());
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
    drawTrackModal();   // 模态永远画在最顶层
}

// ===========================================================================
// 轨道选择模态（导入多轨 MIDI）
// ===========================================================================

void App::drawTrackModal() {
    if (!trackModalOpen_) return;

    // 遮罩：盖住整个逻辑画面
    DrawRectangle(0, 0, static_cast<int>(layout_.w), static_cast<int>(layout_.h),
                  Color{ 5, 9, 16, 178 });

    const float rowH = 34.0f;
    const int total = static_cast<int>(trackModalTracks_.size());
    const int visible = std::max(1, std::min(total, 10));
    const float panelW = 520.0f;
    const float panelH = 60.0f + rowH * static_cast<float>(visible) + 66.0f;
    const Rectangle box{ (layout_.w - panelW) * 0.5f, (layout_.h - panelH) * 0.5f,
                         panelW, panelH };
    widget::softShadow(box, 0.06f, 8.0f, 90);
    widget::panel(box, Color{ 16, 25, 38, 255 }, theme::kLine, 0.06f);
    fonts_.drawBody(ui::t(ui::kTrackModalTitle), box.x + 22.0f, box.y + 20.0f, theme::kInk);

    // 轨道行：「轨 N · 音符 X · 音域 A0—C8」，限高 10 行 + 滚轮 + 细滚动条
    const Rectangle rowsRect{ box.x + 14.0f, box.y + 52.0f,
                              panelW - 28.0f, rowH * static_cast<float>(visible) };
    const float maxScroll = rowH * static_cast<float>(std::max(0, total - visible));
    if (maxScroll > 0.0f && widget::pointIn(rowsRect, ui_.mouse)) {
        trackModalScroll_ -= GetMouseWheelMove() * rowH;
        trackModalScroll_ = std::clamp(trackModalScroll_, 0.0f, maxScroll);
    }
    widget::scissorLogical(ui_, rowsRect);
    for (int i = 0; i < total; ++i) {
        const int trackNo = trackModalTracks_[static_cast<std::size_t>(i)];
        const MidiParseResult::TrackStat& st = trackModalStats_[static_cast<std::size_t>(trackNo)];
        const Rectangle row{ rowsRect.x,
                             rowsRect.y + rowH * static_cast<float>(i) - trackModalScroll_,
                             rowsRect.width, rowH - 4.0f };
        if (row.y + row.height < rowsRect.y || row.y > rowsRect.y + rowsRect.height) continue;
        const bool rowHover = ui_.hovered(row) && widget::pointIn(rowsRect, ui_.mouse);
        const bool selRow = (i == trackModalSel_);
        if (selRow || rowHover)
            DrawRectangleRounded(row, 0.16f, 6,
                                 selRow ? Color{ 150, 112, 58, 110 } : Color{ 213, 173, 106, 18 });
        if (selRow)
            DrawRectangleRoundedLinesEx(row, 0.16f, 6, 1.0f, Color{ 206, 162, 96, 255 });
        std::string line = std::string(ui::t(ui::kTrackWord)) + " " + std::to_string(trackNo) +
                           " · " + std::to_string(st.noteCount) + " " + ui::t(ui::kTrackNotes);
        if (st.lowest >= 0)
            line += std::string(" · ") + ui::t(ui::kTrackRange) + " " +
                    noteName(st.lowest) + "—" + noteName(st.highest);
        fonts_.drawSmall(line.c_str(), row.x + 12.0f, row.y + 10.0f,
                         selRow ? theme::kGoldLight : theme::kInk);
        if (rowHover && ui_.mousePressed) trackModalSel_ = i;
    }
    EndScissorMode();
    widget::drawDropdownScrollbar(rowsRect, maxScroll, trackModalScroll_, visible, total);

    // 确认 / 取消（背景点击已在模态开启时被整帧吃掉，这里直接用原始点击状态）
    auto modalButton = [&](Rectangle br, const char* label, bool primary) {
        const bool hov = ui_.hovered(br);
        const bool press = hov && ui_.mouseDown;
        const Color fill = primary
            ? (press ? Color{ 210, 168, 106, 255 }
               : hov ? Color{ 242, 203, 142, 255 } : Color{ 230, 188, 124, 255 })
            : (press ? Color{ 19, 28, 41, 255 }
               : hov ? Color{ 37, 52, 74, 255 } : Color{ 23, 33, 48, 255 });
        const Color border = primary ? Color{ 198, 158, 96, 255 }
                                     : (hov ? Color{ 88, 104, 128, 255 } : theme::kLineSoft);
        widget::panel(br, fill, border, 0.22f);
        const float tw = fonts_.bodyWidth(label);
        fonts_.drawBody(label, br.x + (br.width - tw) * 0.5f, br.y + 9.0f,
                        primary ? Color{ 32, 34, 38, 255 } : Color{ 222, 230, 240, 255 });
        return hov && ui_.mousePressed;
    };
    const Rectangle okR{ box.x + panelW - 118.0f - 108.0f, box.y + panelH - 48.0f, 96.0f, 34.0f };
    const Rectangle cancelR{ box.x + panelW - 118.0f, box.y + panelH - 48.0f, 96.0f, 34.0f };

    const bool confirm = modalButton(okR, ui::t(ui::kConfirm), true);
    const bool cancel  = modalButton(cancelR, ui::t(ui::kCancel), false) ||
                         (ui_.mousePressed && !widget::pointIn(box, ui_.mouse));
    if (confirm) {
        const int track = (trackModalSel_ >= 0 &&
                           trackModalSel_ < static_cast<int>(trackModalTracks_.size()))
            ? trackModalTracks_[static_cast<std::size_t>(trackModalSel_)] : -1;
        const std::filesystem::path path = trackModalPath_;
        trackModalOpen_ = false;
        finishImport(path, track >= 0 ? std::optional<int>(track) : std::nullopt);
    } else if (cancel) {
        trackModalOpen_ = false;
        toast(ui::t(ui::kToastImportCancel));
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

    const std::filesystem::path path = uniqueExportPath(dir, timestampName("Grand_Studio", ".mid"));
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

    const std::filesystem::path path = uniqueExportPath(dir, timestampName("Song", ".mid"));
    std::string err;
    if (saveMidiFile(path, bytes, &err)) {
        toast(std::string(ui::t(ui::kToastExported)) + " · " + fs::path(path).filename().string());
    } else {
        toast(std::string(ui::t(ui::kToastExportFail)) + "：" + err);
    }
}

void App::beginImport(const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (!ec && size <= SongLibrary::kMaxFileBytes) {
        std::ifstream in(path, std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                              std::istreambuf_iterator<char>());
        if (!bytes.empty()) {
            const MidiParseResult pr = parseMidi(bytes.data(), bytes.size());
            if (pr.ok && pr.format >= 1) {
                // 含音符的轨道 ≥2 → 弹模态让用户选旋律轨，默认预选音符最多的轨
                trackModalTracks_.clear();
                for (std::size_t i = 0; i < pr.trackStats.size(); ++i)
                    if (pr.trackStats[i].noteCount > 0)
                        trackModalTracks_.push_back(static_cast<int>(i));
                if (trackModalTracks_.size() >= 2) {
                    int best = 0;
                    for (std::size_t i = 1; i < trackModalTracks_.size(); ++i)
                        if (pr.trackStats[static_cast<std::size_t>(trackModalTracks_[i])].noteCount >
                            pr.trackStats[static_cast<std::size_t>(trackModalTracks_[best])].noteCount)
                            best = static_cast<int>(i);
                    trackModalPath_   = path;
                    trackModalStats_  = pr.trackStats;
                    trackModalSel_    = best;
                    trackModalScroll_ = 0.0f;
                    trackModalOpen_   = true;
                    dropdownOpen_     = false;
                    dropdownId_       = 0;
                    searchFocused_    = false;
                    return;
                }
            }
        }
    }
    finishImport(path, std::nullopt);
}

SongLibrary::AddResult::Code App::finishImport(const std::filesystem::path& path,
                                               std::optional<int> melodyTrack, bool quiet) {
    const SongLibrary::AddResult r = library_.add(path, melodyTrack);
    using Code = SongLibrary::AddResult::Code;

    if (r.code == Code::Ok) {
        const SongLibrary::Entry* e = library_.find(r.id);
        Song s;
        s.id         = r.id;
        s.title      = e ? e->title : std::string(ui::t(ui::kImportedMidi));
        s.composer   = "用户文件";
        s.bpm        = e ? e->bpm : kDefaultBpm;
        s.difficulty = 2;
        s.userImport = true;
        // 新标题可能含有图集外的字符：并入样本，防抖重烘焙（不打断导入流程）
        if (!fonts_.canRender(s.title)) {
            fontSamples_.push_back(s.title);
            fontRebakeTimer_ = 0.0f;
        }
        songs_.push_back(std::move(s));
        refreshSongList();
        if (!quiet) {
            // 选中刚导入的曲目
            for (std::size_t i = 0; i < songView_.size(); ++i) {
                if (songView_[i] == static_cast<int>(songs_.size()) - 1) { selectSong(songView_[i]); break; }
            }
            toast(std::string(ui::t(ui::kToastImportOk)) + " · " + std::to_string(r.noteCount) + " 个音符");
            setStatus(ui::t(ui::kToastImportOk));
        }
        return r.code;
    }

    if (r.code == Code::Duplicate) {
        if (!quiet) {
            toast(ui::t(ui::kLibDuplicate));
            // 清掉过滤条件，定位并选中已有条目
            showFavoritesOnly_ = false;
            searchText_.clear();
            refreshSongList();
            for (std::size_t i = 0; i < songs_.size(); ++i) {
                if (songs_[i].id != r.id) continue;
                for (int v : songView_) {
                    if (v == static_cast<int>(i)) { selectSong(v); break; }
                }
                break;
            }
        }
        return r.code;
    }

    std::string text;
    switch (r.code) {
    case Code::TooLarge:     text = ui::t(ui::kLibTooLarge); break;
    case Code::TooManySongs: text = ui::t(ui::kLibTooMany); break;
    case Code::TooManyNotes: text = ui::t(ui::kLibTooManyNotes); break;
    case Code::LibraryFull:  text = ui::t(ui::kLibFull); break;
    case Code::IoError:
    case Code::ReadOnly:     text = ui::t(ui::kLibIoError); break;
    default:
        text = std::string(ui::t(ui::kToastImportFail)) +
               (r.message.empty() ? "" : "：" + r.message);
        break;
    }
    if (!quiet) { toast(text); setStatus(text, true); }
    return r.code;
}

void App::openImportDialog() {
    if (importDialog_) return;   // 已打开，忽略重复点击
    if (pfdAvailable_ < 0) pfdAvailable_ = FileDialog::available() ? 1 : 0;
    if (!pfdAvailable_) {
        toast(ui::t(ui::kLibNoFileDialog));
        return;
    }
    importDialog_ = std::make_unique<FileDialog>(ui::t(ui::kMenuImport));
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
    if (!ensureSongLoaded(index)) return;
    songIndex_ = index;
    player_.setSong(songs_[static_cast<std::size_t>(index)], sampleRate_);
    player_.setSpeed(speed_);
    player_.setTranspose(static_cast<int>(std::lround(transposeF_)));
    applyPracticeMode();
    judge_.reset();
    setStatus(ui::t(ui::kSongIdle));
}

bool App::ensureSongLoaded(int index) {
    if (index < 0 || index >= static_cast<int>(songs_.size())) return false;
    Song& s = songs_[static_cast<std::size_t>(index)];
    if (!s.userImport || !s.events.empty()) return true;
    if (s.id.rfind("u-", 0) != 0) return true;   // 非曲库条目（无对应 .mid 文件）
    Song loaded = library_.loadSong(s.id);
    if (loaded.events.empty()) {
        const std::string text = std::string(ui::t(ui::kToastImportFail)) + "：" + s.title;
        toast(text);
        setStatus(text, true);
        return false;
    }
    // 索引里的标题/id 为准（解析结果里的标题来自 MIDI 元事件，可能缺失）
    loaded.id    = s.id;
    loaded.title = s.title;
    s = std::move(loaded);
    return true;
}

void App::startSongPlayback() {
    if (songs_.empty()) { toast(ui::t(ui::kToastNoSong)); return; }
    if (!ensureSongLoaded(songIndex_)) return;
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
    if (id.rfind("u-", 0) == 0) {
        const bool nowFav = std::find(favorites_.begin(), favorites_.end(), id) != favorites_.end();
        library_.setFavorite(id, nowFav);
    }
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
