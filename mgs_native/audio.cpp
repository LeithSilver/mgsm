#include "audio.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <iostream>

AudioOut& AudioOut::Instance() {
    static AudioOut inst;
    return inst;
}

bool AudioOut::Start(int rate) {
    if (running_) return true;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::cerr << "[Audio] SDL audio init failed (" << SDL_GetError() << "), sound disabled" << std::endl;
        return false;
    }
    const SDL_AudioSpec spec = { SDL_AUDIO_S16, 1, rate };
    SDL_AudioStream* s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
        [](void* self, SDL_AudioStream* st, int additional, int total) { Pull(self, st, additional, total); }, this);
    if (!s) {
        std::cerr << "[Audio] No output device (" << SDL_GetError() << "), sound disabled" << std::endl;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    rate_ = rate;
    stream_ = s;
    chunk_.assign(CHUNK_SAMPLES, 0);
    running_ = true;
    SDL_ResumeAudioStreamDevice(s);   // device streams start paused
    std::cout << "[Audio] " << SDL_GetCurrentAudioDriver() << " output started at " << rate << " Hz" << std::endl;
    return true;
}

// SDL wants `additional` more bytes: give what the ring has, silence for the rest.
void AudioOut::Pull(void* self, void* stream, int additional, int) {
    AudioOut& a = *static_cast<AudioOut*>(self);
    int want = additional / 2;
    while (want > 0) {
        const int n = (std::min)(want, CHUNK_SAMPLES);
        size_t got = 0;
        {
            std::lock_guard<std::mutex> lock(a.mutex_);
            got = (std::min)(a.ring_.size(), static_cast<size_t>(n));
            std::copy(a.ring_.begin(), a.ring_.begin() + got, a.chunk_.begin());
            a.ring_.erase(a.ring_.begin(), a.ring_.begin() + got);
        }
        std::fill(a.chunk_.begin() + got, a.chunk_.begin() + n, int16_t(0));
        SDL_PutAudioStreamData(static_cast<SDL_AudioStream*>(stream), a.chunk_.data(), n * 2);
        want -= n;
    }
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
    SDL_DestroyAudioStream(static_cast<SDL_AudioStream*>(stream_));   // stops the device and its callbacks
    stream_ = nullptr;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}
