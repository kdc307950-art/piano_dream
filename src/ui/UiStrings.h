#pragma once
//
// ui/UiStrings.h —— 界面文案集中表
//
// 两个目的：
//   1. 字体加载时只把这些字符串里出现的码点做进字形图集 —— 中文才不会显示成 '?'；
//      文案与绘制共用同一份常量，从根本上杜绝「加了新文案但忘记补字形」。
//   2. 将来要做多语言，只需替换这一张表。
//
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace piano {
namespace ui {

enum class Language { Chinese, English };
inline Language gLanguage = Language::Chinese;

inline void setLanguage(Language language) noexcept { gLanguage = language; }
inline Language language() noexcept { return gLanguage; }

/// 将中文文案常量映射到英文表。未登记的内部文案保持中文，避免状态信息丢失。
inline const char* t(const char* chinese) {
    if (gLanguage == Language::Chinese || !chinese) return chinese;
    static const std::unordered_map<std::string_view, const char*> en = {
        {"Grand Studio · C++ 88 键钢琴", "Grand Studio · C++ 88-Key Piano"},
        {"让音乐，", "Make music, "}, {"触手可及。", "Within reach."},
        {"一架属于你的 88 键三角钢琴。支持电脑键盘、鼠标、多点触控、力度响应、延音踏板与 MIDI 导入导出。", "Your 88-key grand piano. Keyboard, mouse, touch, dynamics, sustain and MIDI import/export."},
        {"音色引擎", "Sound engine"}, {"原声钢琴 · 自动", "Acoustic piano · Auto"},
        {"真实采样 · 优先", "Samples · Priority"}, {"物理泛音 · 离线", "Physical harmonics · Offline"},
        {"主音量", "Master volume"}, {"空间混响", "Reverb"}, {"延音踏板", "Sustain pedal"},
        {"踏板", "Pedal"}, {"开启", "On"}, {"关闭", "Off"},
        {"录制", "Record"}, {"开始录制", "Start recording"}, {"结束录制", "Stop recording"},
        {"回放", "Playback"}, {"■ 停止", "■ Stop"}, {"节拍器", "Metronome"},
        {"关闭节拍器", "Disable metronome"}, {"搜索曲目", "Search songs"},
        {"全部难度", "All levels"}, {"入门", "Easy"}, {"进阶", "Advanced"},
        {"只看收藏", "Favorites only"}, {"播放曲目", "Play song"}, {"■ 停止曲目", "■ Stop song"},
        {"完整播放", "Full playback"}, {"仅伴奏跟练", "Practice with accompaniment"}, {"节拍引导", "Beat guide"},
        {"↓ 导出 MIDI", "↓ Export MIDI"}, {"↓ 导出曲目", "↓ Export song"},
        {"☆ 收藏", "☆ Favorite"}, {"★ 已收藏", "★ Favorited"},
        {"速度", "Speed"}, {"移调", "Transpose"}, {"静音旋律", "Mute melody"},
        {"静音伴奏", "Mute accompaniment"}, {"跟随音域", "Follow range"},
        {"滚动鼠标滚轮、拖动此处或点击 ± 切换音域", "Scroll, drag here, or click ± to change range"},
        {"同时支持多个音符", "Multiple notes supported"}, {"88 KEYS  ·  A0 — C8", "88 KEYS  ·  A0 — C8"},
        {"点击琴键，开始演奏", "Click a key to play"}, {"音频已就绪", "Audio ready"},
        {"音频设备不可用，界面仍可使用", "Audio unavailable; UI remains usable"},
        {"导入：把 .mid 文件拖到窗口内", "Import: drop a .mid file into the window"},
        {"跟练判定", "Practice judge"}, {"完美", "Perfect"}, {"不错", "Good"},
        {"漏音", "Miss"}, {"多余", "Extra"}, {"连击", "Combo"}, {"准确率", "Accuracy"}, {"得分", "Score"},
        {"正在演奏 ", "Playing "}, {" 个音符", " notes"},
        {"电脑键盘演奏", "Computer keyboard"}, {"细腻触键体验", "Expressive touch"},
        {"留住演奏灵感", "Keep your ideas"},
        {"下排 Z—M，上排 Q—P，\n黑键用 S、2 等。", "Bottom row Z-M, top row Q-P,\nblack keys use S, 2, etc."},
        {"空格临时延音，[ ] 升降八度。", "Space sustains; [ ] shift octave."},
        {"鼠标与多点触控，\n越靠近琴键下沿力度越强。", "Mouse and multitouch,\npress lower for more force."},
        {"可滑奏、可和弦，松键自动止音。", "Glissando and chords; release to stop."},
        {"一键录制回放，\n导出标准 MIDI 文件。", "Record and replay,\nexport standard MIDI."},
        {"放入钢琴采样即可切换真实音色。", "Add piano samples for realism."},
        {"GRAND STUDIO · 让每个灵感，都有声音。   C++ / raylib / miniaudio 实现", "GRAND STUDIO · Give every idea a voice.   Built with C++ / raylib / miniaudio"},
        {"请选择曲目开始自动播放", "Select a song to start playback"},
        {"录制已开始 · 尽情演奏", "Recording started · Have fun"}, {"还没有录制到音符", "No notes recorded"},
        {"录制完成，可以回放或导出 MIDI", "Recording complete; play back or export MIDI"},
        {"回放结束", "Playback finished"}, {"MIDI 文件已导出", "MIDI file exported"},
        {"导出失败", "Export failed"}, {"MIDI 导入成功", "MIDI imported"}, {"MIDI 导入失败", "MIDI import failed"},
        {"已加入收藏", "Added to favorites"}, {"已取消收藏", "Removed from favorites"},
        {"请先选择一首曲目", "Select a song first"}, {"未找到采样，使用合成音色", "No samples found; using synth"},
        {"● 离线合成音色", "● Offline synth"}, {"中", "中"},
        {"文件过小，不是有效的 MIDI 文件", "File is too small; not a valid MIDI file"},
        {"缺少 MThd 头，不是标准 MIDI 文件", "Missing MThd header; not a standard MIDI file"},
        {"MThd 块长度非法", "Invalid MThd chunk length"}, {"MThd 内容不完整", "Incomplete MThd content"},
        {"暂不支持 SMPTE 时间码格式的 MIDI 文件", "SMPTE time-code MIDI files are not supported"},
        {"文件中没有可播放的音符事件", "The file contains no playable note events"},
        {"无法打开文件", "Unable to open file"}, {"无法写入文件", "Unable to write file"},
        {"写入过程中出错", "An error occurred while writing the file"},
        {"音频设备初始化失败", "Audio device initialization failed"},
        {"音频设备尚未打开", "Audio device is not open"}, {"音频设备启动失败", "Audio device failed to start"},
        {"跟练中不能修改速度", "Speed cannot change during practice"},
        {"跟练中不能修改移调", "Transpose cannot change during practice"},
        {"没有匹配的曲目", "No matching songs"}, {"导入的 MIDI", "Imported MIDI"},
        {"我的录制", "My recording"}, {"本机录制", "Local recording"},
        {"音符特效", "Notes FX"}, {"关", "Off"}, {"少", "Low"},
        {"标准", "Standard"}, {"多", "High"}, {"减少动态效果", "Reduced motion"},
        {"已在曲库中", "Already in library"}, {"文件超过 1MB 上限", "File exceeds the 1MB limit"},
        {"曲库已满（500 首）", "Library is full (500 songs)"},
        {"音符数超过上限", "Note count exceeds the limit"},
        {"曲库总容量已满", "Library storage is full"}, {"曲库写入失败", "Failed to write library"},
        {"导入 MIDI…", "Import MIDI…"}, {"选择旋律轨", "Choose melody track"},
        {"轨", "Track"}, {"音符", "notes"}, {"音域", "Range"},
        {"确认", "OK"}, {"取消", "Cancel"}, {"已取消导入", "Import cancelled"},
        {"系统文件对话框不可用，请把 .mid 文件拖入窗口",
         "System file dialog unavailable — drop .mid files into the window"},
        {"导入 %d 首", "Imported %d"}, {"跳过 %d 首重复", "%d duplicates skipped"},
        {"%d 个非 MIDI", "%d non-MIDI"}, {"失败 %d 个", "%d failed"}
    };
    const auto it = en.find(chinese);
    return it == en.end() ? chinese : it->second;
}

// ---- 品牌 / 顶部 ----
inline constexpr const char* kWindowTitle   = "Grand Studio · C++ 88 键钢琴";
inline constexpr const char* kBrandTitle    = "GRAND STUDIO";
inline constexpr const char* kBrandSub      = "DIGITAL PIANO EXPERIENCE";

// ---- Hero ----
inline constexpr const char* kEyebrow       = "THE ART OF SOUND · NO. 01";
inline constexpr const char* kHeroLine1     = "让音乐，";
inline constexpr const char* kHeroAccent    = "触手可及。";
inline constexpr const char* kHeroDesc      =
    "一架属于你的 88 键三角钢琴。支持电脑键盘、鼠标、多点触控、力度响应、延音踏板与 MIDI 导入导出。";
inline constexpr const char* kLiveSound     = "LIVE SOUND";

// ---- 控制台 ----
inline constexpr const char* kInstrument    = "Concert Grand";
inline constexpr const char* kInstrumentSub = "88 KEYS · A0 — C8 · POLYPHONIC";
inline constexpr const char* kEngineLabel   = "音色引擎";
inline constexpr const char* kEngineAuto    = "原声钢琴 · 自动";
inline constexpr const char* kEngineSample  = "真实采样 · 优先";
inline constexpr const char* kEngineSynth   = "物理泛音 · 离线";
inline constexpr const char* kVolumeLabel   = "主音量";
inline constexpr const char* kReverbLabel   = "空间混响";
inline constexpr const char* kPedalLabel    = "延音踏板";
inline constexpr const char* kPedalWord     = "踏板";
inline constexpr const char* kOn            = "开启";
inline constexpr const char* kOff           = "关闭";
inline constexpr const char* kFxLabel         = "音符特效";
inline constexpr const char* kFxOff           = "关";
inline constexpr const char* kFxLow           = "少";
inline constexpr const char* kFxStandard      = "标准";
inline constexpr const char* kFxHigh          = "多";
inline constexpr const char* kFxReducedMotion = "减少动态效果";
inline constexpr const char* kRailTip       = "滚动鼠标滚轮、拖动此处或点击 ± 切换音域";
inline constexpr const char* kKeyRangeTip   = "88 KEYS  ·  A0 — C8";
inline constexpr const char* kPolyTip       = "同时支持多个音符";
inline constexpr const char* kPlayingPrefix = "正在演奏 ";
inline constexpr const char* kPlayingSuffix = " 个音符";

// ---- 传输 ----
inline constexpr const char* kRecordStart   = "开始录制";
inline constexpr const char* kRecordStop    = "结束录制";
inline constexpr const char* kPlayback      = "回放";
inline constexpr const char* kStop          = "■ 停止";
inline constexpr const char* kExportMidi    = "↓ 导出 MIDI";
inline constexpr const char* kMetronome     = "节拍器";
inline constexpr const char* kMetronomeOff  = "关闭节拍器";
inline constexpr const char* kBpm           = "BPM";

// ---- 曲目面板 ----
inline constexpr const char* kSearchHint    = "搜索曲目";
inline constexpr const char* kDiffAll       = "全部难度";
inline constexpr const char* kDiffEasy      = "入门";
inline constexpr const char* kDiffHard      = "进阶";
inline constexpr const char* kFavAdd        = "☆ 收藏";
inline constexpr const char* kFavRemove     = "★ 已收藏";
inline constexpr const char* kFavOnly       = "只看收藏";
inline constexpr const char* kPlaySong      = "播放曲目";
inline constexpr const char* kStopSong      = "■ 停止曲目";
inline constexpr const char* kExportSong    = "↓ 导出曲目";
inline constexpr const char* kImportHint    = "导入：把 .mid 文件拖到窗口内";
inline constexpr const char* kModeFull      = "完整播放";
inline constexpr const char* kModeAccomp    = "仅伴奏跟练";
inline constexpr const char* kModeGuide     = "节拍引导";
inline constexpr const char* kSpeed         = "速度";
inline constexpr const char* kTranspose     = "移调";
inline constexpr const char* kMuteMelody    = "静音旋律";
inline constexpr const char* kMuteAccomp    = "静音伴奏";
inline constexpr const char* kFollowKey     = "跟随音域";
inline constexpr const char* kSongIdle      = "请选择曲目开始自动播放";

// ---- 导入、音频与跟练状态 ----
inline constexpr const char* kMidiTooSmall = "文件过小，不是有效的 MIDI 文件";
inline constexpr const char* kMidiNoHeader = "缺少 MThd 头，不是标准 MIDI 文件";
inline constexpr const char* kMidiBadHeader = "MThd 块长度非法";
inline constexpr const char* kMidiIncomplete = "MThd 内容不完整";
inline constexpr const char* kMidiSmpte = "暂不支持 SMPTE 时间码格式的 MIDI 文件";
inline constexpr const char* kMidiNoNotes = "文件中没有可播放的音符事件";
inline constexpr const char* kMidiOpenFail = "无法打开文件";
inline constexpr const char* kWriteFail = "无法写入文件";
inline constexpr const char* kWriteError = "写入过程中出错";
inline constexpr const char* kAudioInitFail = "音频设备初始化失败";
inline constexpr const char* kAudioNotOpen = "音频设备尚未打开";
inline constexpr const char* kAudioStartFail = "音频设备启动失败";
inline constexpr const char* kPracticeSpeedLock = "跟练中不能修改速度";
inline constexpr const char* kPracticeTransposeLock = "跟练中不能修改移调";
inline constexpr const char* kNoMatchingSongs = "没有匹配的曲目";
inline constexpr const char* kImportedMidi = "导入的 MIDI";
inline constexpr const char* kMyRecording = "我的录制";
inline constexpr const char* kLocalRecording = "本机录制";

// ---- 跟练判定 ----
inline constexpr const char* kJudgeTitle    = "跟练判定";
inline constexpr const char* kPerfect       = "完美";
inline constexpr const char* kGood          = "不错";
inline constexpr const char* kMiss          = "漏音";
inline constexpr const char* kExtra         = "多余";
inline constexpr const char* kCombo         = "连击";
inline constexpr const char* kAccuracy      = "准确率";
inline constexpr const char* kScore         = "得分";

// ---- 说明卡 ----
// 注意：说明卡宽度有限（三栏布局），每行控制在 20 个汉字以内，
// 用 \n 手动折行 —— raylib 的 DrawTextEx 不会自动换行。
inline constexpr const char* kGuide1Title   = "电脑键盘演奏";
inline constexpr const char* kGuide1Body    =
    "下排 Z—M，上排 Q—P，\n黑键用 S、2 等。";
inline constexpr const char* kGuide1Body2   = "空格临时延音，[ ] 升降八度。";
inline constexpr const char* kGuide2Title   = "细腻触键体验";
inline constexpr const char* kGuide2Body    =
    "鼠标与多点触控，\n越靠近琴键下沿力度越强。";
inline constexpr const char* kGuide2Body2   = "可滑奏、可和弦，松键自动止音。";
inline constexpr const char* kGuide3Title   = "留住演奏灵感";
inline constexpr const char* kGuide3Body    =
    "一键录制回放，\n导出标准 MIDI 文件。";
inline constexpr const char* kGuide3Body2   = "放入钢琴采样即可切换真实音色。";

// ---- 页脚 ----
inline constexpr const char* kFooter        =
    "GRAND STUDIO · 让每个灵感，都有声音。   C++ / raylib / miniaudio 实现";

// ---- 状态与提示 ----
inline constexpr const char* kStatusReady   = "点击琴键，开始演奏";
inline constexpr const char* kStatusAudioOk = "音频已就绪";
inline constexpr const char* kStatusNoAudio = "音频设备不可用，界面仍可使用";
inline constexpr const char* kToastRecordStart  = "录制已开始 · 尽情演奏";
inline constexpr const char* kToastRecordEmpty  = "还没有录制到音符";
inline constexpr const char* kToastRecordDone   = "录制完成，可以回放或导出 MIDI";
inline constexpr const char* kToastPlayDone     = "回放结束";
inline constexpr const char* kToastExported     = "MIDI 文件已导出";
inline constexpr const char* kToastExportFail   = "导出失败";
inline constexpr const char* kToastImportOk     = "MIDI 导入成功";
inline constexpr const char* kToastImportFail   = "MIDI 导入失败";
inline constexpr const char* kToastFavAdded     = "已加入收藏";
inline constexpr const char* kToastFavRemoved   = "已取消收藏";
inline constexpr const char* kToastNoSong       = "请先选择一首曲目";
inline constexpr const char* kToastDragHint     = "把 .mid 文件拖进窗口即可导入";
inline constexpr const char* kSampleNone        = "未找到采样，使用合成音色";
inline constexpr const char* kSynthBadge        = "● 离线合成音色";

// ---- 用户曲库 ----
inline constexpr const char* kLibDuplicate    = "已在曲库中";
inline constexpr const char* kLibTooLarge     = "文件超过 1MB 上限";
inline constexpr const char* kLibTooMany      = "曲库已满（500 首）";
inline constexpr const char* kLibTooManyNotes = "音符数超过上限";
inline constexpr const char* kLibFull         = "曲库总容量已满";
inline constexpr const char* kLibIoError      = "曲库写入失败";

// ---- 用户曲库 A2：⋯菜单 / 轨道选择模态 / 批量导入汇总 ----
inline constexpr const char* kMenuImport        = "导入 MIDI…";
inline constexpr const char* kTrackModalTitle   = "选择旋律轨";
inline constexpr const char* kTrackWord         = "轨";
inline constexpr const char* kTrackNotes        = "音符";
inline constexpr const char* kTrackRange        = "音域";
inline constexpr const char* kConfirm           = "确认";
inline constexpr const char* kCancel            = "取消";
inline constexpr const char* kToastImportCancel = "已取消导入";
inline constexpr const char* kLibNoFileDialog   = "系统文件对话框不可用，请把 .mid 文件拖入窗口";
inline constexpr const char* kImpSumImported    = "导入 %d 首";
inline constexpr const char* kImpSumDup         = "跳过 %d 首重复";
inline constexpr const char* kImpSumNonMidi     = "%d 个非 MIDI";
inline constexpr const char* kImpSumFailed      = "失败 %d 个";

inline const std::vector<std::string>& allStrings() {
    static const std::vector<std::string> v = {
        kWindowTitle, kBrandTitle, kBrandSub,
        kEyebrow, kHeroLine1, kHeroAccent, kHeroDesc, kLiveSound,
        kInstrument, kInstrumentSub, kEngineLabel, kEngineAuto, kEngineSample, kEngineSynth,
        kVolumeLabel, kReverbLabel, kPedalLabel, kPedalWord, kOn, kOff,
        kFxLabel, kFxOff, kFxLow, kFxStandard, kFxHigh, kFxReducedMotion,
        kRailTip, kKeyRangeTip, kPolyTip, kPlayingPrefix, kPlayingSuffix,
        kRecordStart, kRecordStop, kPlayback, kStop, kExportMidi,
        kMetronome, kMetronomeOff, kBpm,
        kSearchHint, kDiffAll, kDiffEasy, kDiffHard, kFavAdd, kFavRemove, kFavOnly,
        kPlaySong, kStopSong, kExportSong, kImportHint,
        kModeFull, kModeAccomp, kModeGuide,
        kSpeed, kTranspose, kMuteMelody, kMuteAccomp, kFollowKey, kSongIdle,
        kJudgeTitle, kPerfect, kGood, kMiss, kExtra, kCombo, kAccuracy, kScore,
        kMidiTooSmall, kMidiNoHeader, kMidiBadHeader, kMidiIncomplete, kMidiSmpte, kMidiNoNotes,
        kMidiOpenFail, kWriteFail, kWriteError, kAudioInitFail, kAudioNotOpen, kAudioStartFail,
        kPracticeSpeedLock, kPracticeTransposeLock, kNoMatchingSongs, kImportedMidi,
        kMyRecording, kLocalRecording,
        kGuide1Title, kGuide1Body, kGuide1Body2,
        kGuide2Title, kGuide2Body, kGuide2Body2,
        kGuide3Title, kGuide3Body, kGuide3Body2,
        kFooter,
        kStatusReady, kStatusAudioOk, kStatusNoAudio,
        kToastRecordStart, kToastRecordEmpty, kToastRecordDone, kToastPlayDone,
        kToastExported, kToastExportFail, kToastImportOk, kToastImportFail,
        kToastFavAdded, kToastFavRemoved, kToastNoSong, kToastDragHint,
        kSampleNone, kSynthBadge,
        kLibDuplicate, kLibTooLarge, kLibTooMany, kLibTooManyNotes, kLibFull, kLibIoError,
        kMenuImport, kTrackModalTitle, kTrackWord, kTrackNotes, kTrackRange,
        kConfirm, kCancel, kToastImportCancel, kLibNoFileDialog,
        kImpSumImported, kImpSumDup, kImpSumNonMidi, kImpSumFailed,
        // 采样徽标里的数字与单位
        "组真实采样", "个采样锚点", "音源",
        // 音符名、难度、以及曲目标题等运行时拼接用到的字符
        "CDEFGAB",
        // 装饰字形。
        //
        // 注意：微软雅黑（以及多数中文 Windows 字体）**不含** ▶(U+25B6)、
        // ► ▷ ◀ ◁、♪ ♫ ♩ ♬、⌨(U+2328)、✦(U+2726) 这些符号 —— 请求了也拿不到字形，
        // 界面上会显示成 '?'。实测可用的只有：△▽○●□■◇◆☆★※§◎ 以及箭头 ↓→−。
        // 新增图标前务必先用 --screenshot 跑一次，看启动日志里的「不存在的码点」。
        "◎−·×—■☆★↓→",
    };
    return v;
}

} // namespace ui
} // namespace piano
