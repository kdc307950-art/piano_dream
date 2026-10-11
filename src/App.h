#pragma once
//
// App.h —— 应用主控
//
// 把各层串起来：音频引擎 / 输入 / 演奏器 / 录制 / 跟练 / 界面。
// App 是唯一同时接触 UI 线程与音频线程的地方，因此所有跨线程交互都收敛在这里，
// 通过 AudioDevice 的无锁队列与 Player 的「等待音频线程空闲」协议完成。
//
#include <cstdint>
#include <string>
#include <vector>

#include "raylib.h"

#include "audio/AudioDevice.h"
#include "audio/Mixer.h"
#include "audio/SampleBank.h"
#include "core/Song.h"
#include "input/InputRouter.h"
#include "input/KeyboardMap.h"
#include "sequencer/Metronome.h"
#include "sequencer/Player.h"
#include "sequencer/PracticeJudge.h"
#include "sequencer/RecordedPlayer.h"
#include "sequencer/Recorder.h"
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
    void draw(float dt);
    void drawTopbar();
    void drawHero();
    void drawConsole();
    void drawControlsRow();
    void drawKeyboardSection();
    void drawTransportRow();
    void drawSongPanel();
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
    void importMidiFile(const std::string& path);
    void refreshSongList();
    void selectSong(int index);
    void startSongPlayback();
    void stopSongPlayback();
    void applyPracticeMode();
    void loadFavorites();
    void saveFavorites();
    bool isFavorite(int index) const;
    void toggleFavorite(int index);

    std::string timestampName(const char* prefix, const char* ext) const;
    std::string exportDirectory() const;
    void setStatus(const std::string& text, bool warn = false);
    void toast(const std::string& text);

    // ---------------- 成员 ----------------
    Mixer        mixer_;
    AudioDevice  device_;
    SampleBank   samples_;
    KeyboardMap  keyboardMap_;
    InputRouter  input_;
    PianoView    pianoView_;
    Player       player_;
    RecordedPlayer recordedPlayer_;
    Metronome    metronome_;
    Recorder     recorder_;
    PracticeJudge judge_;
    FontBook     fonts_;
    UiLayout     layout_;
    UiContext    ui_;

    std::vector<Song> songs_;
    std::vector<int>  songView_;      // 过滤后的索引
    std::vector<std::string> favorites_;

    // 界面状态
    int   songIndex_ = 0;
    int   difficultyFilter_ = 0;      // 0=全部 1=入门 2=进阶
    int   practiceMode_ = 0;          // 0=完整播放 1=仅伴奏跟练 2=节拍引导
    int   engineMode_ = 0;            // 0=自动 1=采样 2=合成
    bool  showFavoritesOnly_ = false;

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
};

} // namespace piano
