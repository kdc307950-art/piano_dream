#pragma once
//
// input/InputRouter.h —— 输入汇聚层
//
// 把三种输入统一成「按下某个音符 / 松开某个音符 / 踏板状态」：
//   - 电脑键盘（边沿检测，天然规避系统自动重复）
//   - 鼠标（单击、按住、拖动滑奏）
//   - 触屏（多点触控，每个手指独立一个声部）
//
// UI 层只提供「坐标 → 音符 + 力度」的命中测试，输入层不关心键盘怎么画。
//
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "core/Event.h"
#include "input/KeyboardMap.h"

namespace piano {

/// 输入事件接收方（由 App 实现）。
struct InputSink {
    virtual ~InputSink() = default;
    virtual void onNoteOn(int midi, float velocity) = 0;
    virtual void onNoteOff(int midi) = 0;
    virtual void onSustainChanged(bool on) = 0;
};

class InputRouter {
public:
    /// 命中测试：给定屏幕坐标，返回命中的音高与力度。返回 false 表示未命中琴键。
    using HitTest = std::function<bool(float x, float y, int& midiOut, float& velocityOut)>;

    void setSink(InputSink* sink) { sink_ = sink; }
    void setHitTest(HitTest fn) { hitTest_ = std::move(fn); }
    void setKeyboardMap(KeyboardMap* map) { map_ = map; }

    /// 每帧调用一次。
    void update();

    /// 输入框获得焦点时禁用电脑键盘演奏。
    void setKeyboardEnabled(bool on);
    bool keyboardEnabled() const noexcept { return keyboardEnabled_; }

    /// 窗口失焦 / 切换界面时释放所有按下的键。
    void releaseAll();

    void shiftOctave(int delta);

    // ---- 踏板 ----
    void setSustainManual(bool on);
    bool sustainManual() const noexcept { return sustainManual_; }
    bool sustainSpace() const noexcept { return sustainSpace_; }
    bool sustainUserActive() const noexcept { return sustainManual_ || sustainSpace_; }

    // ---- UI 查询 ----
    bool isNoteHeldByInput(int midi) const;

private:
    void pressKey(int midi, float velocity);
    void releaseKey(int midi);
    void updateSustainPush();

    InputSink* sink_ = nullptr;
    HitTest hitTest_;
    KeyboardMap* map_ = nullptr;

    // 电脑键盘：记录每个键的上一次按下状态，做边沿检测
    std::map<int, bool> keyDown_;
    std::map<int, int>  keyVoices_;   // 键码 → 当前音高

    // 指针：pointerId → 音高（鼠标用 -1）
    std::map<int, int> pointerNote_;

    bool keyboardEnabled_ = true;
    bool sustainManual_ = false;
    bool sustainSpace_ = false;
    bool sustainPushed_ = false;
    bool focused_ = true;
};

} // namespace piano
