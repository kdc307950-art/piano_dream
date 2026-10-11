#include "audio/AudioDevice.h"
#include "ui/UiStrings.h"

#include "audio/MaConfig.h"
#include "miniaudio.h"

#include <algorithm>
#include <cmath>

#include "core/Note.h"

namespace piano {
namespace {

std::string maResultText(ma_result r) {
    return std::string(ma_result_description(r));
}

std::uint8_t velocityToMidi(float v) {
    const int i = static_cast<int>(std::lround(clampf(v, 0.0f, 1.0f) * 127.0f));
    return static_cast<std::uint8_t>(i < 1 ? 1 : (i > 127 ? 127 : i));
}

} // namespace

// ---------------------------------------------------------------------------
// 音频回调（运行在 miniaudio 的实时线程）
// ---------------------------------------------------------------------------

void piano_audio_callback(ma_device* device, void* output, const void* input,
                          std::uint32_t frameCount) {
    (void)input;
    if (!device) return;
    auto* self = static_cast<AudioDevice*>(device->pUserData);
    if (!self || !self->mixer_ || frameCount == 0) return;

    float* out = static_cast<float*>(output);
    Mixer& mixer = *self->mixer_;

    // 0) 静音：仍然渲染（推进时间轴、维持声部状态），只是不写输出。
    if (self->muted_.load(std::memory_order_relaxed)) {
        mixer.render(out, frameCount);
        std::fill(out, out + static_cast<std::size_t>(frameCount) * 2, 0.0f);
        return;
    }

    // 1) 主线程投递的事件：在本块起点立即生效
    NoteMsg msg;
    while (self->queue_.pop(msg)) self->applyMessage(msg, mixer);

    // 2) 按采样点分段渲染
    const std::uint64_t blockStart = mixer.currentFrame();
    std::uint32_t done = 0;
    while (done < frameCount) {
        std::uint32_t chunk = frameCount - done;
        if (self->scheduler_) {
            const std::uint32_t until =
                self->scheduler_(self->schedulerUser_, mixer, blockStart + done, chunk);
            if (until > 0 && until < chunk) chunk = until;
        }
        mixer.render(out + static_cast<std::size_t>(done) * 2, chunk);
        done += chunk;
    }
}

void AudioDevice::applyMessage(const NoteMsg& msg, Mixer& mixer) {
    switch (msg.type) {
    case MsgType::NoteOn:
        mixer.noteOn(msg.note, static_cast<float>(msg.velocity) / 127.0f, msg.source);
        break;
    case MsgType::NoteOff:
        mixer.noteOff(msg.note, msg.source);
        break;
    case MsgType::Sustain:
        mixer.setSustain(msg.velocity > 0);
        break;
    case MsgType::AllNotesOff:
        mixer.allNotesOff(false);
        break;
    case MsgType::ReleaseSource:
        mixer.releaseSource(msg.source, false);
        break;
    case MsgType::Panic:
        mixer.allNotesOff(true);
        break;
    }
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

AudioDevice::~AudioDevice() {
    stop();
}

bool AudioDevice::open(Mixer& mixer, SchedulerFn scheduler, void* schedulerUser, std::string* error) {
    stop();

    mixer_ = &mixer;
    scheduler_ = scheduler;
    schedulerUser_ = schedulerUser;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format       = ma_format_f32;
    cfg.playback.channels     = 2;
    cfg.sampleRate            = 48000;   // 固定 48kHz，保证 DSP 参数可复现
    cfg.periodSizeInFrames    = 256;     // 目标 5.3ms / 块，实际以设备能力为准
    cfg.periodSizeInMilliseconds = 0;
    cfg.performanceProfile    = ma_performance_profile_low_latency;
    cfg.dataCallback          = &piano_audio_callback;
    cfg.pUserData             = this;

    auto* dev = new ma_device();
    const ma_result r = ma_device_init(nullptr, &cfg, dev);
    if (r != MA_SUCCESS) {
        if (error) *error = std::string(ui::t(ui::kAudioInitFail)) + "：" + maResultText(r);
        delete dev;
        mixer_ = nullptr;
        scheduler_ = nullptr;
        return false;
    }

    device_       = dev;
    sampleRate_   = static_cast<double>(dev->sampleRate);
    periodFrames_ = dev->playback.internalPeriodSizeInFrames;

    // 按设备实际采样率准备 DSP
    mixer_->prepare(sampleRate_);
    return true;
}

bool AudioDevice::start(std::string* error) {
    if (!device_) {
        if (error) *error = ui::t(ui::kAudioNotOpen);
        return false;
    }
    if (running_) return true;

    const ma_result r = ma_device_start(device_);
    if (r != MA_SUCCESS) {
        if (error) *error = std::string(ui::t(ui::kAudioStartFail)) + "：" + maResultText(r);
        return false;
    }
    running_ = true;
    return true;
}

void AudioDevice::stop() {
    if (!device_) return;
    if (running_) ma_device_stop(device_);
    ma_device_uninit(device_);
    delete device_;
    device_ = nullptr;
    running_ = false;
    // 清空队列，避免重开后执行陈旧事件
    NoteMsg tmp;
    while (queue_.pop(tmp)) { /* drain */ }
}

// ---------------------------------------------------------------------------
// 主线程投递
// ---------------------------------------------------------------------------

bool AudioDevice::pushNoteOn(int midi, float velocity, Source source) {
    NoteMsg m;
    m.type     = MsgType::NoteOn;
    m.note     = static_cast<std::uint8_t>(clampNote(midi));
    m.velocity = velocityToMidi(velocity);
    m.source   = source;
    if (!queue_.push(m)) { dropped_.fetch_add(1, std::memory_order_relaxed); return false; }
    return true;
}

bool AudioDevice::pushNoteOff(int midi, Source source) {
    NoteMsg m;
    m.type     = MsgType::NoteOff;
    m.note     = static_cast<std::uint8_t>(clampNote(midi));
    m.velocity = 0;
    m.source   = source;
    if (!queue_.push(m)) { dropped_.fetch_add(1, std::memory_order_relaxed); return false; }
    return true;
}

bool AudioDevice::pushSustain(bool on) {
    NoteMsg m;
    m.type     = MsgType::Sustain;
    m.note     = 0;
    m.velocity = on ? 127 : 0;
    m.source   = Source::User;
    if (!queue_.push(m)) { dropped_.fetch_add(1, std::memory_order_relaxed); return false; }
    return true;
}

bool AudioDevice::pushAllNotesOff() {
    NoteMsg m;
    m.type = MsgType::AllNotesOff;
    if (!queue_.push(m)) { dropped_.fetch_add(1, std::memory_order_relaxed); return false; }
    return true;
}

bool AudioDevice::pushReleaseSource(Source source) {
    NoteMsg m;
    m.type   = MsgType::ReleaseSource;
    m.source = source;
    if (!queue_.push(m)) { dropped_.fetch_add(1, std::memory_order_relaxed); return false; }
    return true;
}

bool AudioDevice::pushPanic() {
    NoteMsg m;
    m.type = MsgType::Panic;
    if (!queue_.push(m)) { dropped_.fetch_add(1, std::memory_order_relaxed); return false; }
    return true;
}

} // namespace piano
