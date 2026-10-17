// The s3eSound mixer's output on the Vita (README.md §6.29): one BGM audio port, mono 16-bit at the mixer's rate.
// Pushed samples collect in a ring; a thread hands kGrain samples at a time to sceAudioOutOutput (which blocks
// until the port takes them), with silence when the ring runs dry, as the SDL sink does.
#include "audio.h"

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <algorithm>
#include <iostream>

namespace {
constexpr int kGrain = 512;   // samples per output call (~23 ms at 22050 Hz; a multiple of 64)
int g_port = -1;
SceUID g_thread = -1;
}

AudioOut& AudioOut::Instance() {
    static AudioOut inst;
    return inst;
}

bool AudioOut::Start(int rate) {
    if (running_) return true;
    g_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, kGrain, rate, SCE_AUDIO_OUT_MODE_MONO);
    if (g_port < 0) {
        std::cerr << "[Audio] sceAudioOutOpenPort(" << rate << " Hz) failed: 0x" << std::hex << g_port << std::dec << ", no sound" << std::endl;
        return false;
    }
    int vol[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
    sceAudioOutSetVolume(g_port, static_cast<SceAudioOutChannelFlag>(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);
    rate_ = rate;
    chunk_.assign(kGrain, 0);
    running_ = true;
    g_thread = sceKernelCreateThread("audio_out", [](SceSize, void* self) -> int {
        AudioOut& a = **static_cast<AudioOut**>(self);
        while (a.running_) Pull(&a, a.chunk_.data(), kGrain * 2, 0);
        return 0;
    }, 80, 0x4000, 0, 0, nullptr);   // (above the game's main thread, 160: README.md §6.29)
    AudioOut* self = this;
    sceKernelStartThread(g_thread, sizeof(self), &self);
    std::cout << "[Audio] sceAudioOut, " << rate << " Hz mono" << std::endl;
    return true;
}

// One grain to the port: what the ring has, silence for the rest. Blocks until the port takes it.
void AudioOut::Pull(void* self, void* stream, int additional, int) {
    AudioOut& a = *static_cast<AudioOut*>(self);
    int16_t* out = static_cast<int16_t*>(stream);
    const size_t n = static_cast<size_t>(additional / 2);
    size_t got = 0;
    {
        std::lock_guard<std::mutex> lock(a.mutex_);
        got = (std::min)(a.ring_.size(), n);
        std::copy(a.ring_.begin(), a.ring_.begin() + got, out);
        a.ring_.erase(a.ring_.begin(), a.ring_.begin() + got);
    }
    std::fill(out + got, out + n, int16_t(0));
    sceAudioOutOutput(g_port, out);
}

void AudioOut::Push(const int16_t* s, size_t n) {
    if (!running_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    ring_.insert(ring_.end(), s, s + n);
    const size_t cap = static_cast<size_t>(rate_);   // drop the oldest audio beyond 1 s of backlog
    if (ring_.size() > cap) ring_.erase(ring_.begin(), ring_.begin() + (ring_.size() - cap));
}

size_t AudioOut::Queued() {
    std::lock_guard<std::mutex> lock(mutex_);
    return ring_.size();
}

void AudioOut::Stop() {
    if (!running_) return;
    running_ = false;
    sceKernelWaitThreadEnd(g_thread, nullptr, nullptr);
    sceKernelDeleteThread(g_thread);
    sceAudioOutReleasePort(g_port);
    g_port = -1;
}
