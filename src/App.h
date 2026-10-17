#pragma once
//
// App.h —— 应用主控
//
// 把各层串起来：音频引擎 / 输入 / 演奏器 / 录制 / 跟练 / 界面。
// App 是唯一同时接触 UI 线程与音频线程的地方，因此所有跨线程交互都收敛在这里，
// 通过 AudioDevice 的无锁队列与 Player 的「等待音频线程空闲」协议完成。
//
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "raylib.h"

#include "audio/AudioDevice.h"
#include "audio/Mixer.h"
#include "audio/SampleBank.h"
#include "core/Song.h"
#include "core/VisualEvent.h"
#include "fx/FxLayer.h"
#include "fx/NoteParticles.h"
#include "input/InputRouter.h"
#include "input/KeyboardMap.h"
#include "library/SongLibrary.h"
#include "midi/MidiReader.h"
#include "sequencer/Metronome.h"
#include "sequencer/Player.h"
#include "sequencer/PracticeJudge.h"
#include "sequencer/RecordedPlayer.h"
#include "sequencer/Recorder.h"
#include "ui/FileDialog.h"
#include "ui/FontBook.h"
#include "ui/PianoView.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace piano {

class App : public InputSink {
public:
    bool init(std::string* log);
    void run();
    void shutdown();

    /// 自动截图模式：渲染若干帧后存图并退出（用于无人值守地检视界面）。
    void setCaptureMode(int frames, std::string path, bool autoPlaySong);
    /// 特效演示模式：固定种子与 dt，脚本注入音符，定时输出 5 张截图后退出。
    void setFxDemoMode(std::string dir);
    void setWindowSize(int width, int height) noexcept;

    // ---- InputSink ----
    void onNoteOn(int midi, float velocity) override;
    void onNoteOff(int midi) override;
    void onSustainChanged(bool on) override;

private:
    // ---------------- 调度（音频线程） ----------------
    static std::uint32_t schedulerEntry(void* user, Mixer& mixer,
                                        std::uint64_t blockStartFrame, std::uint32_t maxFrames);
    std::uint32_t scheduler(Mixer& mixer, std::uint64_t blockStartFrame, std::uint32_t maxFrames);

    // ---------------- 界面 ----------------
    void handleInput(float dt);
    void updateFx(float dt);   // 排空视觉事件队列并推进音符粒子
    void runFxDemoScript(int frame);   // --fx-demo：按 demo 时钟注入脚本事件
    void drawFxStage();        // 舞台带背景（控制台面板的一部分，画在琴键之前）
    void draw(float dt);
    void drawTopbar();
    void drawHero();
    void drawConsole();
    void drawLibraryPane();
    void drawHeroPanel();
    void drawControlsRow();
    void drawControlsLegacy();
    void drawKeyboardSection();
    void drawTransportRow();
    void drawTransportLegacy();
    void drawSongPanel();
    void drawSongPanelLegacy();
    void drawGuide();
    void drawFooter();
    void drawOverlays();

    // ---------------- 业务动作 ----------------
    void pushSustainIfChanged();
    void startRecording();
    void stopRecording();
    void startPlayback();
    void stopPlayback();
    void exportRecordingMidi();
    void exportSongMidi();
    /// 导入入口：读文件并解析，多含音符轨道的 Format 1 文件先弹轨道选择模态，
    /// 否则直接 finishImport(path, nullopt)。
    void beginImport(const std::filesystem::path& path);
    /// 实际入库 + 界面反馈（ melodyTrack >= 0 时记为该曲的旋律轨）。
    /// quiet=true 时不弹单文件 toast（批量导入由调用方汇总），返回结果码供计数。
    SongLibrary::AddResult::Code finishImport(const std::filesystem::path& path,
                                              std::optional<int> melodyTrack,
                                              bool quiet = false);
    /// ⋯菜单「导入 MIDI…」：弹系统文件对话框（pfd 异步模型，结果在 handleInput 轮询）。
    void openImportDialog();
    void drawTrackModal();
    bool modalActive() const { return trackModalOpen_; }
    void refreshSongList();
    void selectSong(int index);
    /// 惰性加载：用户曲库条目首次被选中/播放时才解析 .mid 文件并填充 events。
    /// 返回 false 表示加载失败（调用方应中止后续动作）。
    bool ensureSongLoaded(int index);
    void startSongPlayback();
    void stopSongPlayback();
    void applyPracticeMode();
    void loadFavorites();
    void saveFavorites();
    void loadLanguage();
    void saveLanguage() const;
    void loadFxSettings();      // fx.txt 不存在或坏值时回落默认（Standard / 关）
    void saveFxSettings() const;
    void applyFxSettings();     // 档位/减少动态效果改动即生效并落盘
    bool isFavorite(int index) const;
    void toggleFavorite(int index);

    std::string timestampName(const char* prefix, const char* ext) const;
    std::filesystem::path exportDirectory() const;
    void setStatus(const std::string& text, bool warn = false);
    void toast(const std::string& text);

    // ---------------- 成员 ----------------
    Mixer        mixer_;
    AudioDevice  device_;
    SampleBank   samples_;
    VisualEventQueue fxQueue_;       // 音频线程 → UI 线程的音符视觉事件
    fx::NoteFx       noteFx_;        // 「按键飘出音符」粒子逻辑
    fx::FxLayer      fxLayer_;       // 音符图集与渲染层
    KeyboardMap  keyboardMap_;
    InputRouter  input_;
    PianoView    pianoView_;
    Player       player_;
    RecordedPlayer recordedPlayer_;
    Metronome    metronome_;
    Recorder     recorder_;
    PracticeJudge judge_;
    FontBook     fonts_;
    Texture2D    heroTexture_{};
    bool         heroTextureReady_ = false;
    Texture2D    coverSheet_{};
    bool         coverSheetReady_ = false;
    UiLayout     layout_;
    UiContext    ui_;

    std::vector<Song> songs_;
    std::vector<int>  songView_;      // 过滤后的索引
    std::vector<std::string> favorites_;
    SongLibrary library_;             // 用户曲库（索引 + 去重 + 惰性解析）

    // 界面状态
    int   songIndex_ = 0;
    int   difficultyFilter_ = 0;      // 0=全部 1=入门 2=进阶
    int   practiceMode_ = 0;          // 0=完整播放 1=仅伴奏跟练 2=节拍引导
    int   scoreTab_ = 0;              // 0=乐谱跟练 1=钢琴卷帘 2=练习统计
    int   engineMode_ = 0;            // 0=自动 1=采样 2=合成
    bool  showFavoritesOnly_ = false;
    bool  showKeyHints_ = false;

    float volume_ = 0.75f;
    float reverb_ = 0.28f;
    float speed_ = 1.0f;
    float transposeF_ = 0.0f;
    float bpmF_ = 100.0f;

    bool sustainUser_ = false;
    bool sustainEffective_ = false;

    bool recording_ = false;
    bool playingRecording_ = false;
    bool songPlaying_ = false;
    bool followKeyboard_ = false;
    bool metronomeOn_ = false;
    bool muteMelody_ = false;
    bool muteAccomp_ = false;

    // 视图拖动 / 进度条拖动
    bool  railDragging_ = false;
    float railLastX_ = 0.0f;
    bool  seeking_ = false;

    // 搜索框
    bool  searchFocused_ = false;
    std::string searchText_;

    // 下拉菜单
    bool dropdownOpen_ = false;
    int  dropdownId_ = 0;

    // 轨道选择模态（导入多轨 MIDI 时弹出）
    bool trackModalOpen_ = false;
    std::filesystem::path trackModalPath_;
    std::vector<int>      trackModalTracks_;   // 每行对应的原始轨号（仅含音符 ≥1 的轨）
    std::vector<MidiParseResult::TrackStat> trackModalStats_;
    int   trackModalSel_ = 0;                  // 行索引
    float trackModalScroll_ = 0.0f;

    // 系统文件对话框（存在即打开中；后端可用性一次性检测后缓存）
    std::unique_ptr<FileDialog> importDialog_;
    int pfdAvailable_ = -1;                    // -1 未检测，0/1 缓存

    // 波形显示
    std::vector<float> scope_;
    float scopeTimer_ = 0.0f;

    // 提示条
    std::string statusText_ = "点击琴键，开始演奏";
    bool statusWarn_ = false;
    std::string toastText_;
    float toastTimer_ = 0.0f;

    // 拖拽导入
    float dragHintTimer_ = 0.0f;

    double sampleRate_ = 48000.0;
    bool   audioReady_ = false;
    std::string audioError_;
    std::string sampleLog_;

    // 截图模式
    int         autoExitFrames_ = 0;
    std::string screenshotPath_;
    bool        captureAutoPlay_ = false;

    // --fx-demo 演示模式
    bool        fxDemo_ = false;
    std::string fxDemoDir_;
    int         fxDemoCursor_ = 0;    // 脚本推进游标
    bool        fxDemoDirReady_ = false;

    // 特效图集重烘焙防抖 / 视图变化检测
    float fxAtlasScale_ = 0.0f;
    float fxAtlasTimer_ = 0.0f;
    float prevViewScale_ = 0.0f;
    int   prevScreenW_ = 0;
    int   prevScreenH_ = 0;

    // 特效设置（持久化到 fx.txt）
    int  fxLevel_ = 2;                 // 0=关 1=少 2=标准 3=多
    bool fxReducedMotion_ = false;

    // 自适应降级：只统计特效 update + 绘制提交，当次会话不回升
    double fxFrameCostMs_ = 0.0;
    int    fxOverBudgetFrames_ = 0;
    int    fxIgnoreFrames_ = 0;
    bool   fxDegraded_ = false;
    int         windowWidth_ = 1520;
    int         windowHeight_ = 1000;
    float       dpiScale_ = 1.0f;
    float       fontScale_ = 1.0f;
    float       fontScaleTimer_ = 0.0f;
    std::vector<std::string> fontSamples_;
    float       fontRebakeTimer_ = -1.0f;   // ≥0 时表示导入新标题后待重烘焙字体图集（防抖）
    float       viewScale_ = 1.0f;
    Vector2     viewOffset_{};
};

} // namespace piano
