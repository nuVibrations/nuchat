// File: vpio_loopback.cpp
// Build: clang++ -std=c++17 vpio_loopback.cpp -framework AudioToolbox -framework AudioUnit -framework CoreAudio -o vpio_loopback
// Run:   ./vpio_loopback
// Note: First run will prompt for Microphone access on macOS.

#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <atomic>
#include <vector>
#include <cstdio>
#include <cstring>
#include <pthread.h>

static AudioUnit gAU = nullptr;
static const double kSampleRate = 48000.0;
static const UInt32 kChannels = 1;
static const UInt32 kFramesPerSliceTarget = 64; // try 64 for low latency

// --- Simple lock-free-ish single-producer/single-consumer float FIFO ---
struct FloatFIFO {
    std::vector<float> buf;
    std::atomic<uint32_t> w{0}, r{0}; // indices in frames
    uint32_t mask; // size-1 (power of two)

    explicit FloatFIFO(uint32_t framesPow2) {
        uint32_t sz = 1;
        while (sz < framesPow2) sz <<= 1;
        buf.resize(sz);
        mask = sz - 1;
    }
    uint32_t push(const float* src, uint32_t n) {
        uint32_t wi = w.load(std::memory_order_relaxed);
        uint32_t ri = r.load(std::memory_order_acquire);
        uint32_t freeFrames = ((mask + 1) - (wi - ri)) & mask;
        if (n > freeFrames) n = freeFrames;
        for (uint32_t i = 0; i < n; ++i)
            buf[(wi + i) & mask] = src[i];
        w.store((wi + n) & mask, std::memory_order_release);
        return n;
    }
    uint32_t pop(float* dst, uint32_t n) {
        uint32_t wi = w.load(std::memory_order_acquire);
        uint32_t ri = r.load(std::memory_order_relaxed);
        uint32_t avail = (wi - ri) & mask;
        uint32_t got = (n <= avail) ? n : avail;
        for (uint32_t i = 0; i < got; ++i)
            dst[i] = buf[(ri + i) & mask];
        r.store((ri + got) & mask, std::memory_order_release);
        if (got < n) {
            std::memset(dst + got, 0, (n - got) * sizeof(float));
        }
        return got;
    }
};

static FloatFIFO gFifo(1 << 16); // 65536 frames (~1.36 s @ 48k)
static void rt_set_realtime() {
    pthread_t t = pthread_self();
    struct sched_param sp {};
    sp.sched_priority = 46;
    pthread_setschedparam(t, SCHED_RR, &sp);
}

static void print_error(const char* where, OSStatus s) {
    char cc[5]; *(UInt32*)cc = CFSwapInt32HostToBig(s); cc[4] = 0;
    if (isprint(cc[0]) && isprint(cc[1]) && isprint(cc[2]) && isprint(cc[3]))
        std::fprintf(stderr, "%s: OSStatus '%s'\n", where, cc);
    else
        std::fprintf(stderr, "%s: OSStatus %d\n", where, (int)s);
}

static OSStatus InputCallback(void*, AudioUnitRenderActionFlags* ioActionFlags,
                              const AudioTimeStamp* inTimeStamp,
                              UInt32, UInt32 inNumberFrames, AudioBufferList*) {
    rt_set_realtime();
    std::vector<float> temp(inNumberFrames);
    AudioBufferList abl{};
    abl.mNumberBuffers = 1;
    abl.mBuffers[0].mNumberChannels = kChannels;
    abl.mBuffers[0].mData = temp.data();
    abl.mBuffers[0].mDataByteSize = inNumberFrames * sizeof(float);
    OSStatus s = AudioUnitRender(gAU, ioActionFlags, inTimeStamp, 1, inNumberFrames, &abl);
    if (s != noErr) { print_error("AudioUnitRender (input)", s); return s; }
    gFifo.push(temp.data(), inNumberFrames);
    return noErr;
}

static OSStatus RenderCallback(void*, AudioUnitRenderActionFlags*,
                               const AudioTimeStamp*, UInt32, UInt32 inNumberFrames,
                               AudioBufferList* ioData) {
    rt_set_realtime();
    float* out = static_cast<float*>(ioData->mBuffers[0].mData);
    gFifo.pop(out, inNumberFrames);
    return noErr;
}

static void try_set_device_buffer(UInt32 frames) {
    AudioObjectID outDev = kAudioObjectUnknown, inDev = kAudioObjectUnknown;
    UInt32 sz = sizeof(AudioObjectID);
    AudioObjectPropertyAddress addrDev {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &addrDev, 0, nullptr, &sz, &outDev);
    addrDev.mSelector = kAudioHardwarePropertyDefaultInputDevice;
    sz = sizeof(AudioObjectID);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &addrDev, 0, nullptr, &sz, &inDev);

    AudioObjectPropertyAddress addr {
        kAudioDevicePropertyBufferFrameSize,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    if (outDev != kAudioObjectUnknown)
        AudioObjectSetPropertyData(outDev, &addr, 0, nullptr, sizeof(frames), &frames);
    if (inDev != kAudioObjectUnknown)
        AudioObjectSetPropertyData(inDev, &addr, 0, nullptr, sizeof(frames), &frames);
}

int main() {
    try_set_device_buffer(kFramesPerSliceTarget);
    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_VoiceProcessingIO;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp) { std::fprintf(stderr, "VoiceProcessingIO not found.\n"); return 1; }
    if (AudioComponentInstanceNew(comp, &gAU) != noErr) return 1;

    UInt32 one = 1;
    AudioUnitSetProperty(gAU, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &one, sizeof(one));
    AudioUnitSetProperty(gAU, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &one, sizeof(one));

    AudioStreamBasicDescription asbd{};
    asbd.mSampleRate = kSampleRate;
    asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    asbd.mChannelsPerFrame = kChannels;
    asbd.mBitsPerChannel = 32;
    asbd.mFramesPerPacket = 1;
    asbd.mBytesPerFrame = 4;
    asbd.mBytesPerPacket = 4;
    AudioUnitSetProperty(gAU, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &asbd, sizeof(asbd));
    AudioUnitSetProperty(gAU, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &asbd, sizeof(asbd));

    AURenderCallbackStruct inCb{InputCallback, nullptr};
    AudioUnitSetProperty(gAU, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &inCb, sizeof(inCb));
    AURenderCallbackStruct outCb{RenderCallback, nullptr};
    AudioUnitSetProperty(gAU, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &outCb, sizeof(outCb));

    AudioUnitInitialize(gAU);
    AudioOutputUnitStart(gAU);
    std::puts("Running… speak into the mic; you should hear near-instant playback. Press Ctrl+C to quit.");
    CFRunLoopRun();
    AudioOutputUnitStop(gAU);
    AudioUnitUninitialize(gAU);
    AudioComponentInstanceDispose(gAU);
    return 0;
}
