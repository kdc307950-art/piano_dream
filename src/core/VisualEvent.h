#pragma once
//
// core/VisualEvent.h —— 音频线程 → UI 线程的视觉事件
//
// Mixer 在 noteOn 成功分配声部后写入，App 主循环逐帧排空，
// 驱动「按键飘出音符」等纯视觉特效。与发声逻辑完全解耦：
// 队列满时直接丢弃，不影响音频。
//
#include <cstddef>
#include <cstdint>

#include "core/Event.h"
#include "core/SpscQueue.h"

namespace piano {

struct VisualEvent {
    std::uint64_t audioFrame = 0;            // 事件对应的混音器采样帧
    float         velocity   = 0.0f;         // 0.05–1.0（Mixer 已钳制后的值）
    std::uint8_t  midi       = 0;
    Source        source     = Source::User; // 复用 core/Event.h 的枚举
};

inline constexpr std::size_t kVisualEventQueueCapacity = 256;
using VisualEventQueue = SpscQueue<VisualEvent, kVisualEventQueueCapacity>;

} // namespace piano
