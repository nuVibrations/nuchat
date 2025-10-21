// android_voice_loopback.cpp
// Minimal low-latency Android voice loopback using Oboe (AAudio).
// Requires Oboe library: https://github.com/google/oboe
// Build via Android Studio + CMake with Oboe linked as submodule.
//
// JNI functions provided:
//   Java_com_example_voice_Loopback_start
//   Java_com_example_voice_Loopback_stop

#include <oboe/Oboe.h>
#include <atomic>
#include <vector>
#include <memory>

using namespace oboe;

struct FloatFIFO {
    std::vector<float> buf;
    std::atomic<uint32_t> w{0}, r{0};
    uint32_t mask;
    explicit FloatFIFO(uint32_t frames) {
        uint32_t p = 1; while (p < frames) p <<= 1;
        buf.resize(p); mask = p - 1;
    }
    void push(const float* src, uint32_t n) {
        auto wi = w.load(); auto ri = r.load();
        auto free = ((mask + 1) - (wi - ri)) & mask;
        if (n > free) n = free;
        for (uint32_t i = 0; i < n; ++i)
            buf[(wi + i) & mask] = src[i];
        w.store((wi + n) & mask);
    }
    void pop(float* dst, uint32_t n) {
        auto wi = w.load(); auto ri = r.load();
        auto avail = (wi - ri) & mask;
        auto got = n < avail ? n : avail;
        for (uint32_t i = 0; i < got; ++i)
            dst[i] = buf[(ri + i) & mask];
        r.store((ri + got) & mask);
        if (got < n) memset(dst + got, 0, (n - got) * sizeof(float));
    }
};

static FloatFIFO gFifo(1 << 16);

class FullDuplex : public AudioStreamCallback {
public:
    std::shared_ptr<AudioStream> inputStream, outputStream;

    DataCallbackResult onAudioReady(AudioStream* stream, void* audioData,
                                    int32_t numFrames) override {
        if (stream == inputStream.get()) {
            gFifo.push((float*)audioData, numFrames);
        } else if (stream == outputStream.get()) {
            gFifo.pop((float*)audioData, numFrames);
        }
        return DataCallbackResult::Continue;
    }

    void start() {
        AudioStreamBuilder inBuilder, outBuilder;

        inBuilder.setDirection(Direction::Input)
                 .setPerformanceMode(PerformanceMode::LowLatency)
                 .setSharingMode(SharingMode::Exclusive)
                 .setFormat(AudioFormat::Float)
                 .setChannelCount(ChannelCount::Mono)
                 .setSampleRate(48000)
                 .setCallback(this);

        outBuilder.setDirection(Direction::Output)
                  .setPerformanceMode(PerformanceMode::LowLatency)
                  .setSharingMode(SharingMode::Exclusive)
                  .setFormat(AudioFormat::Float)
                  .setChannelCount(ChannelCount::Mono)
                  .setSampleRate(48000)
                  .setCallback(this);

        inBuilder.openStream(inputStream);
        outBuilder.openStream(outputStream);

        inputStream->requestStart();
        outputStream->requestStart();
    }

    void stop() {
        if (inputStream) inputStream->requestStop();
        if (outputStream) outputStream->requestStop();
    }
};

static FullDuplex gDuplex;

extern "C" void Java_com_example_voice_Loopback_start(JNIEnv*, jobject) {
    gDuplex.start();
}

extern "C" void Java_com_example_voice_Loopback_stop(JNIEnv*, jobject) {
    gDuplex.stop();
}
