#include "audio/SampleBank.h"

#include "audio/MaConfig.h"
#include "miniaudio.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>

#include "core/Note.h"

namespace piano {
namespace {

std::string lowerExt(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

bool isAudioExt(const std::string& e) {
    return e == ".wav" || e == ".wave" || e == ".flac" || e == ".mp3" ||
           e == ".ogg" || e == ".m4a" || e == ".aiff" || e == ".aif";
}

/// 从文件名（去扩展名）解析锚点音高。
int anchorFromStem(const std::string& stem) {
    const int direct = parseNoteName(stem);
    if (direct >= kMidiLow && direct <= kMidiHigh) return direct;

    // 兼容 "C4_v1"、"Piano_C4_1" 之类的前后缀
    for (std::size_t i = 0; i < stem.size(); ++i) {
        if (std::isalpha(static_cast<unsigned char>(stem[i])) == 0) continue;
        std::size_t j = i + 1;
        if (j < stem.size() && (stem[j] == '#' || stem[j] == 'b' || stem[j] == 's' ||
                                stem[j] == 'S' || stem[j] == 'B')) ++j;
        while (j < stem.size() && std::isdigit(static_cast<unsigned char>(stem[j]))) ++j;
        const int v = parseNoteName(stem.substr(i, j - i));
        if (v >= kMidiLow && v <= kMidiHigh) return v;
    }
    return -1;
}

} // namespace

int SampleBank::loadDirectory(const std::string& dir, std::string* log) {
    namespace fs = std::filesystem;

    // std::filesystem::path 在 Windows 上会把窄字符串当 UTF-8 转换，
    // 拿到 ANSI(GBK) 字节的路径会直接抛异常。这里兜住，
    // 让「路径不对」退化成一个可读的提示，而不是崩掉整个程序。
    try {
        std::error_code ec;
        if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
            if (log) *log += "[samples] 目录不存在，跳过：" + dir + "\n";
            return 0;
        }

        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            if (!isAudioExt(lowerExt(entry.path()))) continue;
            files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());

        int loaded = 0;
        for (const auto& path : files) {
            const int anchor = anchorFromStem(path.stem().string());
            if (anchor < 0) {
                if (log) *log += "[samples] 文件名无法识别音高，跳过：" + path.filename().string() + "\n";
                continue;
            }

            ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
            cfg.encodingFormat = ma_encoding_format_unknown;
            ma_decoder decoder;
            if (ma_decoder_init_file(path.string().c_str(), &cfg, &decoder) != MA_SUCCESS) {
                if (log) *log += "[samples] 解码失败，跳过：" + path.filename().string() + "\n";
                continue;
            }

            auto buf = std::make_unique<SampleBuffer>();
            buf->channels   = static_cast<int>(decoder.outputChannels);
            buf->sampleRate = static_cast<double>(decoder.outputSampleRate);
            buf->anchorNote = anchor;
            buf->path       = path.string();

            ma_uint64 total = 0;
            if (ma_decoder_get_length_in_pcm_frames(&decoder, &total) != MA_SUCCESS || total == 0) {
                total = static_cast<ma_uint64>(decoder.outputSampleRate) * 30u;  // 上限 30s 兜底
            }
            buf->data.resize(static_cast<std::size_t>(total) * static_cast<std::size_t>(buf->channels));

            ma_uint64 read = 0;
            ma_decoder_read_pcm_frames(&decoder, buf->data.data(), total, &read);
            buf->data.resize(static_cast<std::size_t>(read) * static_cast<std::size_t>(buf->channels));
            ma_decoder_uninit(&decoder);

            if (!buf->valid()) {
                if (log) *log += "[samples] 采样过短，跳过：" + path.filename().string() + "\n";
                continue;
            }

            if (log) {
                *log += "[samples] " + noteName(anchor) + "  " + path.filename().string() +
                        "  " + std::to_string(buf->frameCount()) + " 帧 / " +
                        std::to_string(buf->channels) + " 声道 / " +
                        std::to_string(static_cast<int>(buf->sampleRate)) + " Hz\n";
            }
            owned_.push_back(std::move(buf));
            ++loaded;
        }

        sorted_.clear();
        anchors_.clear();
        for (const auto& b : owned_) sorted_.push_back(b.get());
        std::sort(sorted_.begin(), sorted_.end(),
                  [](const SampleBuffer* a, const SampleBuffer* b) { return a->anchorNote < b->anchorNote; });
        for (const auto* b : sorted_) anchors_.push_back(b->anchorNote);
        return loaded;
    } catch (const std::exception& e) {
        if (log) *log += "[samples] 读取目录失败（路径编码问题？）：" + dir + " — " + e.what() + "\n";
        return 0;
    }
}

const SampleBuffer* SampleBank::nearest(int note, int maxDistance) const {
    const SampleBuffer* best = nullptr;
    int bestDist = maxDistance + 1;
    for (const auto* b : sorted_) {
        const int d = std::abs(b->anchorNote - note);
        if (d < bestDist) { bestDist = d; best = b; }
    }
    return bestDist <= maxDistance ? best : nullptr;
}

int SampleBank::totalFrames() const {
    int n = 0;
    for (const auto& b : owned_) n += static_cast<int>(b->frameCount());
    return n;
}

void SampleBank::clear() {
    owned_.clear();
    sorted_.clear();
    anchors_.clear();
}

} // namespace piano
