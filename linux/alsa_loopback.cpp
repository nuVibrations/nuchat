// alsa_loopback.cpp
// Minimal low-latency ALSA full-duplex loopback for Linux.
// Captures microphone and plays it back with minimal latency.
//
// Build: g++ -std=c++17 alsa_loopback.cpp -lasound -lpthread -o alsa_loopback
// Run:   ./alsa_loopback

#include <alsa/asoundlib.h>
#include <atomic>
#include <vector>
#include <thread>
#include <iostream>

static const unsigned int SAMPLE_RATE = 48000;
static const snd_pcm_format_t FORMAT = SND_PCM_FORMAT_FLOAT_LE;
static const unsigned int CHANNELS = 1;
static const snd_pcm_uframes_t BUFFER_FRAMES = 128;

struct FloatFIFO {
    std::vector<float> buf;
    std::atomic<uint32_t> w{0}, r{0};
    uint32_t mask;
    explicit FloatFIFO(uint32_t sz) {
        uint32_t p = 1; while (p < sz) p <<= 1;
        buf.resize(p);
        mask = p - 1;
    }
    void push(const float* src, uint32_t n) {
        uint32_t wi = w.load(); uint32_t ri = r.load();
        uint32_t free = ((mask + 1) - (wi - ri)) & mask;
        if (n > free) n = free;
        for (uint32_t i = 0; i < n; ++i)
            buf[(wi + i) & mask] = src[i];
        w.store((wi + n) & mask);
    }
    void pop(float* dst, uint32_t n) {
        uint32_t wi = w.load(); uint32_t ri = r.load();
        uint32_t avail = (wi - ri) & mask;
        uint32_t got = n < avail ? n : avail;
        for (uint32_t i = 0; i < got; ++i)
            dst[i] = buf[(ri + i) & mask];
        r.store((ri + got) & mask);
        if (got < n)
            memset(dst + got, 0, (n - got) * sizeof(float));
    }
};

static FloatFIFO gFifo(1 << 16);
static bool gRunning = true;

void captureThread(snd_pcm_t* captureHandle) {
    std::vector<float> buf(BUFFER_FRAMES);
    while (gRunning) {
        snd_pcm_sframes_t frames = snd_pcm_readi(captureHandle, buf.data(), BUFFER_FRAMES);
        if (frames < 0) {
            snd_pcm_prepare(captureHandle);
            continue;
        }
        gFifo.push(buf.data(), frames);
    }
}

void playbackThread(snd_pcm_t* playbackHandle) {
    std::vector<float> buf(BUFFER_FRAMES);
    while (gRunning) {
        gFifo.pop(buf.data(), BUFFER_FRAMES);
        snd_pcm_sframes_t frames = snd_pcm_writei(playbackHandle, buf.data(), BUFFER_FRAMES);
        if (frames < 0) {
            snd_pcm_prepare(playbackHandle);
            continue;
        }
    }
}

int main() {
    snd_pcm_t *captureHandle, *playbackHandle;
    snd_pcm_hw_params_t *hwParams;

    // Open playback and capture devices
    if (snd_pcm_open(&captureHandle, "default", SND_PCM_STREAM_CAPTURE, 0) < 0) {
        std::cerr << "Cannot open capture device" << std::endl;
        return 1;
    }
    if (snd_pcm_open(&playbackHandle, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        std::cerr << "Cannot open playback device" << std::endl;
        return 1;
    }

    // Configure both devices
    for (auto handle : {captureHandle, playbackHandle}) {
        snd_pcm_hw_params_malloc(&hwParams);
        snd_pcm_hw_params_any(handle, hwParams);
        snd_pcm_hw_params_set_access(handle, hwParams, SND_PCM_ACCESS_RW_INTERLEAVED);
        snd_pcm_hw_params_set_format(handle, hwParams, FORMAT);
        snd_pcm_hw_params_set_channels(handle, hwParams, CHANNELS);
        snd_pcm_hw_params_set_rate(handle, hwParams, SAMPLE_RATE, 0);
        snd_pcm_hw_params_set_buffer_size(handle, hwParams, BUFFER_FRAMES * 4);
        snd_pcm_hw_params_set_period_size(handle, hwParams, BUFFER_FRAMES, 0);
        snd_pcm_hw_params(handle, hwParams);
        snd_pcm_hw_params_free(hwParams);
        snd_pcm_prepare(handle);
    }

    std::thread tCap(captureThread, captureHandle);
    std::thread tPlay(playbackThread, playbackHandle);

    std::cout << "Running... speak into the mic; you should hear yourself." << std::endl;
    std::cout << "Press Ctrl+C to exit." << std::endl;
    while (true) std::this_thread::sleep_for(std::chrono::seconds(1));

    gRunning = false;
    tCap.join();
    tPlay.join();

    snd_pcm_close(captureHandle);
    snd_pcm_close(playbackHandle);
    return 0;
}
