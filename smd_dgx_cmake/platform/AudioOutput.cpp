#include "AudioOutput.h"
#include <cstring>
#include <algorithm>

void CALLBACK AudioOutput::waveOutProc(HWAVEOUT, UINT msg, DWORD_PTR inst, DWORD_PTR, DWORD_PTR)
{
    if (msg == WOM_DONE) {
        auto* self = reinterpret_cast<AudioOutput*>(inst);
        --self->queued_;
        SetEvent(self->event_);
    }
}

AudioOutput::AudioOutput()
{
    event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    WAVEFORMATEX wfx{};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = kChannels;
    wfx.nSamplesPerSec  = kSampleRate;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = kChannels * 2;
    wfx.nAvgBytesPerSec = kSampleRate * kChannels * 2;

    waveOutOpen(&hwo_, WAVE_MAPPER, &wfx,
                reinterpret_cast<DWORD_PTR>(&waveOutProc),
                reinterpret_cast<DWORD_PTR>(this),
                CALLBACK_FUNCTION);

    for (int i = 0; i < kNumBufs; ++i) {
        hdr_[i].lpData         = reinterpret_cast<LPSTR>(buf_[i]);
        hdr_[i].dwBufferLength = sizeof(buf_[i]);
        waveOutPrepareHeader(hwo_, &hdr_[i], sizeof(WAVEHDR));
    }
}

AudioOutput::~AudioOutput()
{
    if (hwo_) {
        waveOutReset(hwo_);
        for (int i = 0; i < kNumBufs; ++i)
            waveOutUnprepareHeader(hwo_, &hdr_[i], sizeof(WAVEHDR));
        waveOutClose(hwo_);
    }
    if (event_) CloseHandle(event_);
}

void AudioOutput::write(const int16_t* stereo, int frameCount)
{
    if (!hwo_) return;
    int remaining = frameCount;
    const int16_t* src = stereo;

    while (remaining > 0) {
        // wait until at least one buffer slot is free
        while (queued_ >= kNumBufs)
            WaitForSingleObject(event_, INFINITE);

        int idx = head_;
        int chunk = std::min(remaining, kBufFrames);
        std::memcpy(buf_[idx], src, (size_t)chunk * kChannels * sizeof(int16_t));

        hdr_[idx].dwBufferLength = (DWORD)(chunk * kChannels * sizeof(int16_t));
        waveOutWrite(hwo_, &hdr_[idx], sizeof(WAVEHDR));
        ++queued_;
        head_ = (head_ + 1) % kNumBufs;
        src       += chunk * kChannels;
        remaining -= chunk;
    }
}

void AudioOutput::pause(bool p)
{
    if (!hwo_) return;
    if (p) waveOutPause(hwo_);
    else   waveOutRestart(hwo_);
}
