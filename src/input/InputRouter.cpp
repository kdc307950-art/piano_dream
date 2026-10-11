#include "input/InputRouter.h"

#include "raylib.h"

#include <algorithm>
#include <vector>

namespace piano {
namespace {

constexpr float kKeyboardVelocity = 0.79f;   // 与 HTML 原版一致
constexpr int   kMousePointerId   = -1;
constexpr int   kTouchPointerBase = 1000;

} // namespace

void InputRouter::setKeyboardEnabled(bool on) {
    if (!on && keyboardEnabled_) releaseAll();
    keyboardEnabled_ = on;
}

void InputRouter::pressKey(int midi, float velocity) {
    if (sink_) sink_->onNoteOn(midi, velocity);
}

void InputRouter::releaseKey(int midi) {
    if (sink_) sink_->onNoteOff(midi);
}

void InputRouter::updateSustainPush() {
    const bool on = sustainUserActive();
    if (on == sustainPushed_) return;
    sustainPushed_ = on;
    if (sink_) sink_->onSustainChanged(on);
}

void InputRouter::setSustainManual(bool on) {
    if (sustainManual_ == on) return;
    sustainManual_ = on;
    updateSustainPush();
}

void InputRouter::shiftOctave(int delta) {
    if (!map_ || delta == 0) return;
    releaseAll();
    map_->setOctaveShift(map_->octaveShift() + delta);
}

void InputRouter::releaseAll() {
    for (auto& kv : keyVoices_) releaseKey(kv.second);
    keyVoices_.clear();
    for (auto& kv : keyDown_) kv.second = false;
    for (auto& kv : pointerNote_) releaseKey(kv.second);
    pointerNote_.clear();

    // 与 HTML 一致：失焦时松开「空格延音」，但保留手动踏板按钮的状态
    if (sustainSpace_) {
        sustainSpace_ = false;
        updateSustainPush();
    }
}

bool InputRouter::isNoteHeldByInput(int midi) const {
    for (const auto& kv : keyVoices_)
        if (kv.second == midi) return true;
    for (const auto& kv : pointerNote_)
        if (kv.second == midi) return true;
    return false;
}

void InputRouter::update() {
    if (!IsWindowFocused()) {
        if (focused_) {
            releaseAll();
            focused_ = false;
        }
        return;
    }
    focused_ = true;

    // ---------------- 电脑键盘 ----------------
    if (map_) {
        for (const auto& b : map_->bindings()) {
            const int key = static_cast<unsigned char>(b.key);
            const bool down = IsKeyDown(key);

            // 始终同步物理状态，避免屏蔽期间状态陈旧、解除屏蔽后误触发
            bool& prev = keyDown_[key];
            if (down == prev) continue;
            prev = down;

            if (!keyboardEnabled_) continue;

            if (down) {
                const int midi = map_->noteForKey(key);
                if (midi >= 0 && keyVoices_.find(key) == keyVoices_.end()) {
                    keyVoices_[key] = midi;
                    pressKey(midi, kKeyboardVelocity);
                }
            } else {
                auto it = keyVoices_.find(key);
                if (it != keyVoices_.end()) {
                    releaseKey(it->second);
                    keyVoices_.erase(it);
                }
            }
        }

        if (keyboardEnabled_) {
            // 空格：临时延音（与 HTML 原版一致）
            const bool space = IsKeyDown(KEY_SPACE);
            if (space != sustainSpace_) {
                sustainSpace_ = space;
                updateSustainPush();
            }
            // 方括号：整体升降八度
            if (IsKeyPressed(KEY_LEFT_BRACKET))  shiftOctave(-1);
            if (IsKeyPressed(KEY_RIGHT_BRACKET)) shiftOctave(+1);
        }
    }

    // ---------------- 鼠标 ----------------
    if (hitTest_) {
        const Vector2 mp = GetMousePosition();

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            pointerNote_.find(kMousePointerId) == pointerNote_.end()) {
            int midi = -1;
            float vel = 0.8f;
            if (hitTest_(mp.x, mp.y, midi, vel)) {
                pointerNote_[kMousePointerId] = midi;
                pressKey(midi, vel);
            }
        }

        auto it = pointerNote_.find(kMousePointerId);
        if (it != pointerNote_.end()) {
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                int midi = -1;
                float vel = 0.8f;
                // 滑奏：拖到另一个琴键时切换音符；拖出琴键区域则保持当前音（与 HTML 一致）
                if (hitTest_(mp.x, mp.y, midi, vel) && midi != it->second) {
                    releaseKey(it->second);
                    it->second = midi;
                    pressKey(midi, vel);
                }
            } else {
                releaseKey(it->second);
                pointerNote_.erase(it);
            }
        }
    }

    // ---------------- 触屏（多点触控） ----------------
    {
        const int touchCount = GetTouchPointCount();
        std::vector<int> seen;
        seen.reserve(static_cast<std::size_t>(touchCount));

        for (int i = 0; i < touchCount; ++i) {
            const int id = kTouchPointerBase + GetTouchPointId(i);
            seen.push_back(id);
            if (!hitTest_) continue;

            const Vector2 tp = GetTouchPosition(i);
            int midi = -1;
            float vel = 0.8f;
            if (!hitTest_(tp.x, tp.y, midi, vel)) continue;

            auto it = pointerNote_.find(id);
            if (it == pointerNote_.end()) {
                pointerNote_[id] = midi;
                pressKey(midi, vel);
            } else if (it->second != midi) {
                releaseKey(it->second);
                it->second = midi;
                pressKey(midi, vel);
            }
        }

        // 抬起的手指：对应触点从列表里消失
        for (auto it = pointerNote_.begin(); it != pointerNote_.end();) {
            if (it->first >= kTouchPointerBase &&
                std::find(seen.begin(), seen.end(), it->first) == seen.end()) {
                releaseKey(it->second);
                it = pointerNote_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

} // namespace piano
