// wasapi_loopback.cpp
// Low-latency full-duplex example using Windows WASAPI.
// Captures mic input and plays back to default output in real time.
//
// Build:
//   cl /EHsc /std:c++17 wasapi_loopback.cpp /link ole32.lib avrt.lib
//
// Run:
//   ./wasapi_loopback.exe

#define _WIN32_DCOM
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <atomic>
#include <vector>
#include <thread>
#include <iostream>
#include <chrono>

static const REFERENCE_TIME HNS_PER_SEC = 10000000; // 100ns units
static const UINT32 SAMPLE_RATE = 48000;
static const UINT32 CHANNELS = 1;
static const UINT32 BUFFER_FRAMES = 128;

struct FloatFIFO {
    std::vector<float> buf;
    std::atomic<uint32_t> w{0}, r{0};
    uint32_t mask;
    explicit FloatFIFO(uint32_t framesPow2) {
        uint32_t sz = 1; while (sz < framesPow2) sz <<= 1;
        buf.resize(sz);
        mask = sz - 1;
    }
    void push(const float* src, uint32_t n) {
        uint32_t wi = w.load(std::memory_order_relaxed);
        uint32_t ri = r.load(std::memory_order_acquire);
        uint32_t freeFrames = ((mask + 1) - (wi - ri)) & mask;
        if (n > freeFrames) n = freeFrames;
        for (uint32_t i = 0; i < n; ++i)
            buf[(wi + i) & mask] = src[i];
        w.store((wi + n) & mask, std::memory_order_release);
    }
    void pop(float* dst, uint32_t n) {
        uint32_t wi = w.load(std::memory_order_acquire);
        uint32_t ri = r.load(std::memory_order_relaxed);
        uint32_t avail = (wi - ri) & mask;
        uint32_t got = (n <= avail) ? n : avail;
        for (uint32_t i = 0; i < got; ++i)
            dst[i] = buf[(ri + i) & mask];
        r.store((ri + got) & mask, std::memory_order_release);
        if (got < n)
            std::fill(dst + got, dst + n, 0.0f);
    }
};

static FloatFIFO gFifo(1 << 16);

void renderThread(IAudioClient* renderClient, IAudioRenderClient* render, WAVEFORMATEX* fmt) {
    HANDLE hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    renderClient->SetEventHandle(hEvent);
    HANDLE hTask = AvSetMmThreadCharacteristics(L"Pro Audio", NULL);
    renderClient->Start();

    UINT32 bufferFrames;
    renderClient->GetBufferSize(&bufferFrames);

    while (true) {
        WaitForSingleObject(hEvent, INFINITE);
        UINT32 padding = 0;
        renderClient->GetCurrentPadding(&padding);
        UINT32 frames = bufferFrames - padding;
        if (frames == 0) continue;

        BYTE* pData;
        render->GetBuffer(frames, &pData);
        gFifo.pop(reinterpret_cast<float*>(pData), frames);
        render->ReleaseBuffer(frames, 0);
    }
    AvRevertMmThreadCharacteristics(hTask);
}

void captureThread(IAudioClient* captureClient, IAudioCaptureClient* capture, WAVEFORMATEX* fmt) {
    HANDLE hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    captureClient->SetEventHandle(hEvent);
    HANDLE hTask = AvSetMmThreadCharacteristics(L"Pro Audio", NULL);
    captureClient->Start();

    while (true) {
        WaitForSingleObject(hEvent, INFINITE);
        UINT32 packetFrames = 0;
        BYTE* pData = nullptr;
        DWORD flags = 0;
        capture->GetNextPacketSize(&packetFrames);
        while (packetFrames > 0) {
            capture->GetBuffer(&pData, &packetFrames, &flags, NULL, NULL);
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                gFifo.push(reinterpret_cast<float*>(pData), packetFrames);
            }
            capture->ReleaseBuffer(packetFrames);
            capture->GetNextPacketSize(&packetFrames);
        }
    }
    AvRevertMmThreadCharacteristics(hTask);
}

int main() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* devEnum = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, IID_PPV_ARGS(&devEnum));

    IMMDevice* inDev = nullptr, * outDev = nullptr;
    devEnum->GetDefaultAudioEndpoint(eCapture, eCommunications, &inDev);
    devEnum->GetDefaultAudioEndpoint(eRender, eConsole, &outDev);

    IAudioClient* inClient = nullptr, * outClient = nullptr;
    inDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&inClient);
    outDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&outClient);

    WAVEFORMATEX* wfx = nullptr;
    outClient->GetMixFormat(&wfx);
    wfx->nSamplesPerSec = SAMPLE_RATE;
    wfx->nChannels = CHANNELS;
    wfx->wBitsPerSample = 32;
    wfx->nBlockAlign = CHANNELS * 4;
    wfx->nAvgBytesPerSec = SAMPLE_RATE * wfx->nBlockAlign;
    wfx->wFormatTag = WAVE_FORMAT_IEEE_FLOAT;

    REFERENCE_TIME hnsBuffer = (REFERENCE_TIME)((double)HNS_PER_SEC * BUFFER_FRAMES / SAMPLE_RATE);

    outClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                          AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                          hnsBuffer, 0, wfx, NULL);
    inClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                         AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                         hnsBuffer, 0, wfx, NULL);

    IAudioRenderClient* render = nullptr;
    outClient->GetService(IID_PPV_ARGS(&render));
    IAudioCaptureClient* capture = nullptr;
    inClient->GetService(IID_PPV_ARGS(&capture));

    std::thread tOut(renderThread, outClient, render, wfx);
    std::thread tIn(captureThread, inClient, capture, wfx);

    std::cout << "Running... speak into mic, you'll hear yourself with low latency.\n";
    tOut.join();
    tIn.join();
}
