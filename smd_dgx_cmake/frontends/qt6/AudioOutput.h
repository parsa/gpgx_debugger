#pragma once
#include <cstdint>
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

class AudioOutput
{
public:
    static constexpr int kSampleRate  = 48000;
    static constexpr int kChannels    = 2;
    static constexpr int kBufFrames   = 2048;   // samples per buffer
    static constexpr int kNumBufs     = 4;

    AudioOutput();
    ~AudioOutput();

    void write(const int16_t* stereo, int frameCount);
    void pause(bool p);

private:
    static void CALLBACK waveOutProc(HWAVEOUT, UINT msg, DWORD_PTR inst, DWORD_PTR, DWORD_PTR);
    void freeBuffer(int idx);
    int  nextBuf() { return (head_ + 1) % kNumBufs; }

    HWAVEOUT    hwo_    = nullptr;
    WAVEHDR     hdr_[kNumBufs]{};
    int16_t     buf_[kNumBufs][kBufFrames * kChannels]{};
    HANDLE      event_  = nullptr;
    int         head_   = 0;
    int         queued_ = 0;
};
