#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

// SDL3 audio sink for the s3eSound mixer: 16-bit mono PCM pushed from the emu thread into a ring, pulled by
// SDL's audio thread. When the ring runs dry the device gets silence, so a stalled emu thread (loading) only
// produces a gap.
class AudioOut {
public:
    static AudioOut& Instance();
    bool Start(int rate);                 // idempotent; false if no output device
    void Push(const int16_t* s, size_t n);
    size_t Queued();                      // samples waiting in the ring (not yet handed to the device)
    void Stop();
    int Rate() const { return rate_; }

private:
    AudioOut() = default;
    ~AudioOut() { Stop(); }
    static void Pull(void* self, void* stream, int additional, int total);   // SDL audio thread

    static constexpr int CHUNK_SAMPLES = 1024;   // handed to SDL per callback at most (as the old waveOut buffers)
    int rate_ = 0;
    void* stream_ = nullptr;              // SDL_AudioStream
    std::mutex mutex_;
    std::deque<int16_t> ring_;
    std::vector<int16_t> chunk_;
    std::atomic<bool> running_{false};
};
