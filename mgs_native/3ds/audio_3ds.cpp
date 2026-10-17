// The s3eSound mixer's output on the 3DS (README.md §6.28): NDSP, one mono 16-bit channel at the mixer's rate.
// Pushed samples collect in a ring and go to the DSP in kChunk-sample wave buffers (linear memory). NDSP needs
// the DSP firmware dump (sdmc:/3ds/dspfirm.cdc, made by DSP1); without it the game runs silent.
#include "audio.h"

#include <3ds.h>
#include <cstring>
#include <iostream>

namespace {
// Small chunks, many buffers: the DSP is fed in 23 ms steps, so a slow frame doesn't starve it (crackling).
constexpr int kChunk = 512;
constexpr int kBuffers = 16;
ndspWaveBuf g_wave[kBuffers];
int16_t* g_data = nullptr;   // kBuffers x kChunk, linear memory
}

AudioOut& AudioOut::Instance() {
    static AudioOut inst;
    return inst;
}

bool AudioOut::Start(int rate) {
    if (running_) return true;
    if (R_FAILED(ndspInit())) {
        std::cerr << "[Audio] NDSP unavailable (no sdmc:/3ds/dspfirm.cdc? Dump it with DSP1): no sound" << std::endl;
        return false;
    }
    g_data = static_cast<int16_t*>(linearAlloc(sizeof(int16_t) * kChunk * kBuffers));
    if (!g_data) { ndspExit(); return false; }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, static_cast<float>(rate));
    ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);
    float mix[12] = {1.0f, 1.0f};   // both speakers
    ndspChnSetMix(0, mix);
    memset(g_wave, 0, sizeof(g_wave));
    for (int i = 0; i < kBuffers; ++i) {
        g_wave[i].data_pcm16 = g_data + i * kChunk;
        g_wave[i].status = NDSP_WBUF_DONE;
    }
    rate_ = rate;
    running_ = true;
    std::cout << "[Audio] NDSP, " << rate << " Hz mono" << std::endl;
    return true;
}

// Hands full chunks to free wave buffers (mutex_ held).
static void Submit(std::deque<int16_t>& ring, int chunk) {
    for (int i = 0; i < kBuffers && static_cast<int>(ring.size()) >= chunk; ++i) {
        ndspWaveBuf& w = g_wave[i];
        if (w.status != NDSP_WBUF_DONE && w.status != NDSP_WBUF_FREE) continue;
        std::copy(ring.begin(), ring.begin() + chunk, w.data_pcm16);
        ring.erase(ring.begin(), ring.begin() + chunk);
        w.nsamples = chunk;
        DSP_FlushDataCache(w.data_pcm16, chunk * sizeof(int16_t));
        ndspChnWaveBufAdd(0, &w);
    }
}

void AudioOut::Push(const int16_t* s, size_t n) {
    if (!running_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    ring_.insert(ring_.end(), s, s + n);
    Submit(ring_, kChunk);
}

size_t AudioOut::Queued() {
    if (!running_) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    Submit(ring_, kChunk);
    size_t n = ring_.size();
    for (const ndspWaveBuf& w : g_wave)
        if (w.status == NDSP_WBUF_QUEUED || w.status == NDSP_WBUF_PLAYING) n += kChunk;
    // Report half: the mixer keeps 3 blocks (70 ms) "queued", so this keeps ~140 ms in the DSP's buffers.
    return n / 2;
}

void AudioOut::Stop() {
    if (!running_) return;
    running_ = false;
    ndspChnWaveBufClear(0);
    ndspExit();
    linearFree(g_data);
    g_data = nullptr;
}

void AudioOut::Pull(void*, void*, int, int) {}
