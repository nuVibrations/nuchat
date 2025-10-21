// ios_voice_loopback.mm
// Minimal low-latency VoiceProcessingIO example for iOS.
// Compile inside an iOS app target (e.g., ViewController.mm).

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <atomic>
#import <vector>

static AudioUnit gAudioUnit = nullptr;
static const Float64 kSampleRate = 48000.0;
static const UInt32 kChannels = 1;
static const UInt32 kFramesPerBuffer = 128;

struct FloatFIFO {
    std::vector<float> buf;
    std::atomic<uint32_t> w{0}, r{0};
    uint32_t mask;
    FloatFIFO(uint32_t sz) {
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

static FloatFIFO gFifo(1 << 15);

static OSStatus InputCallback(void *inRefCon,
                              AudioUnitRenderActionFlags *ioActionFlags,
                              const AudioTimeStamp *inTimeStamp,
                              UInt32 inBusNumber,
                              UInt32 inNumberFrames,
                              AudioBufferList *ioData)
{
    AudioBufferList abl;
    float buf[kFramesPerBuffer];
    abl.mNumberBuffers = 1;
    abl.mBuffers[0].mData = buf;
    abl.mBuffers[0].mDataByteSize = inNumberFrames * sizeof(float);
    abl.mBuffers[0].mNumberChannels = 1;

    OSStatus status = AudioUnitRender(gAudioUnit, ioActionFlags,
                                      inTimeStamp, 1, inNumberFrames, &abl);
    if (status == noErr)
        gFifo.push(buf, inNumberFrames);
    return status;
}

static OSStatus RenderCallback(void *inRefCon,
                               AudioUnitRenderActionFlags *ioActionFlags,
                               const AudioTimeStamp *inTimeStamp,
                               UInt32 inBusNumber,
                               UInt32 inNumberFrames,
                               AudioBufferList *ioData)
{
    float *out = (float*)ioData->mBuffers[0].mData;
    gFifo.pop(out, inNumberFrames);
    return noErr;
}

void StartVoiceLoopback()
{
    // 1. Configure AVAudioSession
    AVAudioSession *session = [AVAudioSession sharedInstance];
    [session setCategory:AVAudioSessionCategoryPlayAndRecord
             withOptions:AVAudioSessionCategoryOptionDefaultToSpeaker
                   error:nil];
    [session setMode:AVAudioSessionModeVoiceChat error:nil];
    [session setPreferredSampleRate:kSampleRate error:nil];
    [session setPreferredIOBufferDuration:0.0027 error:nil];
    [session setActive:YES error:nil];

    // 2. Create and configure the AudioUnit
    AudioComponentDescription desc = {
        kAudioUnitType_Output,
        kAudioUnitSubType_VoiceProcessingIO,
        kAudioUnitManufacturer_Apple,
        0, 0
    };
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    AudioComponentInstanceNew(comp, &gAudioUnit);

    UInt32 enable = 1;
    AudioUnitSetProperty(gAudioUnit, kAudioOutputUnitProperty_EnableIO,
                         kAudioUnitScope_Input, 1, &enable, sizeof(enable));
    AudioUnitSetProperty(gAudioUnit, kAudioOutputUnitProperty_EnableIO,
                         kAudioUnitScope_Output, 0, &enable, sizeof(enable));

    AudioStreamBasicDescription asbd = {0};
    asbd.mSampleRate       = kSampleRate;
    asbd.mFormatID         = kAudioFormatLinearPCM;
    asbd.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    asbd.mChannelsPerFrame = kChannels;
    asbd.mBitsPerChannel   = 32;
    asbd.mBytesPerFrame    = 4;
    asbd.mFramesPerPacket  = 1;
    asbd.mBytesPerPacket   = 4;

    AudioUnitSetProperty(gAudioUnit, kAudioUnitProperty_StreamFormat,
                         kAudioUnitScope_Output, 1, &asbd, sizeof(asbd));
    AudioUnitSetProperty(gAudioUnit, kAudioUnitProperty_StreamFormat,
                         kAudioUnitScope_Input, 0, &asbd, sizeof(asbd));

    // 3. Register callbacks
    AURenderCallbackStruct inCb = { InputCallback, nullptr };
    AudioUnitSetProperty(gAudioUnit, kAudioOutputUnitProperty_SetInputCallback,
                         kAudioUnitScope_Global, 0, &inCb, sizeof(inCb));
    AURenderCallbackStruct outCb = { RenderCallback, nullptr };
    AudioUnitSetProperty(gAudioUnit, kAudioUnitProperty_SetRenderCallback,
                         kAudioUnitScope_Input, 0, &outCb, sizeof(outCb));

    // 4. Initialize and start
    AudioUnitInitialize(gAudioUnit);
    AudioOutputUnitStart(gAudioUnit);

    NSLog(@"VoiceProcessingIO started: low-latency loopback running");
}

void StopVoiceLoopback()
{
    if (gAudioUnit) {
        AudioOutputUnitStop(gAudioUnit);
        AudioUnitUninitialize(gAudioUnit);
        AudioComponentInstanceDispose(gAudioUnit);
        gAudioUnit = nullptr;
    }
}
