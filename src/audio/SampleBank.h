#pragma once
//
// audio/SampleBank.h —— 采样音色库
//
// 从 assets/samples/ 目录加载钢琴采样（WAV / FLAC / MP3 / OGG）。
// 文件名即锚点音高：C4.wav、Ds4.wav、Eb4.wav、060.wav 均可识别。
// 播放时按最近的锚点做变调（playbackRate = 2^((note-anchor)/12)），
// 与 HTML 原版「少量锚点 + 变调补全」的策略一致。
//
// 默认资源目录包含 FreePats Kawai Upright Piano 的 CC0 FLAC 锚点采样；
// 缺少资源时 Mixer 会自动回退到合成音色（见 README「采样与许可」）。
//
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace piano {

struct SampleBuffer {
    std::vector<float> data;        // 交错存储，长度 = frameCount * channels
    int    channels   = 1;
    double sampleRate = 48000.0;
    int    anchorNote = 60;
    float  layerVelocity = 0.5f; // 采样层的目标力度（vL≈0.25，vH≈0.85）
    std::string path;

    std::size_t frameCount() const { return channels > 0 ? data.size() / static_cast<std::size_t>(channels) : 0; }
    bool valid() const { return frameCount() > 8; }

    /// 线性插值取一对立体声样本。pos 为帧位置（可为小数）。
    inline void sampleAt(double pos, float& l, float& r) const noexcept {
        const std::size_t n = frameCount();
        if (pos < 0.0 || n < 2) { l = r = 0.0f; return; }
        const std::size_t i0 = static_cast<std::size_t>(pos);
        if (i0 + 1 >= n) { l = r = 0.0f; return; }
        const float f = static_cast<float>(pos - static_cast<double>(i0));

        if (channels == 1) {
            const float a = data[i0], b = data[i0 + 1];
            l = r = a + (b - a) * f;
        } else {
            const float* p0 = &data[i0 * static_cast<std::size_t>(channels)];
            const float* p1 = &data[(i0 + 1) * static_cast<std::size_t>(channels)];
            l = p0[0] + (p1[0] - p0[0]) * f;
            r = p0[1] + (p1[1] - p0[1]) * f;
        }
    }
};

class SampleBank {
public:
    /// 扫描目录并解码全部可识别的采样文件。返回成功加载的数量。
    /// log 若非空则追加人类可读的加载日志。
    int loadDirectory(const std::string& dir, std::string* log = nullptr);

    /// 返回与 note 距离不超过 maxDistance 的最近锚点采样；没有则返回 nullptr。
    const SampleBuffer* nearest(int note, int maxDistance, float velocity = 0.5f) const;

    bool empty() const { return sorted_.empty(); }
    std::size_t size() const { return sorted_.size(); }
    int totalFrames() const;
    void clear();

    /// 已加载的锚点音高列表（升序），用于 UI 展示。
    const std::vector<int>& anchors() const { return anchors_; }

private:
    std::vector<std::unique_ptr<SampleBuffer>> owned_;
    std::vector<const SampleBuffer*> sorted_;   // 按 anchorNote 升序
    std::vector<int> anchors_;
};

} // namespace piano
