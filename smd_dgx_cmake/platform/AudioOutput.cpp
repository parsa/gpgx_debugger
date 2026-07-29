#include "AudioOutput.h"

#include <algorithm>
#include <cstring>

// ===========================================================================
// Windows: waveOut
// ===========================================================================
#ifdef _WIN32

#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

namespace { constexpr int kBufFrames = 2048; constexpr int kNumBufs = 4; }

struct AudioOutput::Impl {
    HWAVEOUT hwo    = nullptr;
    WAVEHDR  hdr[kNumBufs]{};
    int16_t  buf[kNumBufs][kBufFrames * AudioOutput::kChannels]{};
    HANDLE   event  = nullptr;
    int      head   = 0;
    volatile LONG queued = 0;

    static void CALLBACK waveProc(HWAVEOUT, UINT msg, DWORD_PTR inst, DWORD_PTR, DWORD_PTR)
    {
        if (msg != WOM_DONE) return;
        auto* self = reinterpret_cast<Impl*>(inst);
        InterlockedDecrement(&self->queued);
        SetEvent(self->event);
    }
};

AudioOutput::AudioOutput() : impl_(new Impl)
{
    impl_->event = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    WAVEFORMATEX wfx{};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = kChannels;
    wfx.nSamplesPerSec  = kSampleRate;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = kChannels * 2;
    wfx.nAvgBytesPerSec = kSampleRate * kChannels * 2;

    if (waveOutOpen(&impl_->hwo, WAVE_MAPPER, &wfx,
                    reinterpret_cast<DWORD_PTR>(&Impl::waveProc),
                    reinterpret_cast<DWORD_PTR>(impl_.get()),
                    CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
        impl_->hwo = nullptr;
        return;
    }

    for (int i = 0; i < kNumBufs; ++i) {
        impl_->hdr[i].lpData         = reinterpret_cast<LPSTR>(impl_->buf[i]);
        impl_->hdr[i].dwBufferLength = sizeof(impl_->buf[i]);
        waveOutPrepareHeader(impl_->hwo, &impl_->hdr[i], sizeof(WAVEHDR));
    }
}

AudioOutput::~AudioOutput()
{
    if (impl_->hwo) {
        waveOutReset(impl_->hwo);
        for (int i = 0; i < kNumBufs; ++i)
            waveOutUnprepareHeader(impl_->hwo, &impl_->hdr[i], sizeof(WAVEHDR));
        waveOutClose(impl_->hwo);
    }
    if (impl_->event) CloseHandle(impl_->event);
}

bool AudioOutput::isOpen() const { return impl_->hwo != nullptr; }

void AudioOutput::write(const int16_t* stereo, int frameCount)
{
    if (!impl_->hwo || !stereo) return;
    int remaining = frameCount;
    const int16_t* src = stereo;

    while (remaining > 0) {
        while (impl_->queued >= kNumBufs)          // wait for a free slot
            WaitForSingleObject(impl_->event, INFINITE);

        const int idx   = impl_->head;
        const int chunk = std::min(remaining, kBufFrames);
        std::memcpy(impl_->buf[idx], src, size_t(chunk) * kChannels * sizeof(int16_t));

        impl_->hdr[idx].dwBufferLength = DWORD(chunk * kChannels * sizeof(int16_t));
        InterlockedIncrement(&impl_->queued);
        waveOutWrite(impl_->hwo, &impl_->hdr[idx], sizeof(WAVEHDR));

        impl_->head = (impl_->head + 1) % kNumBufs;
        src       += chunk * kChannels;
        remaining -= chunk;
    }
}

void AudioOutput::pause(bool p)
{
    if (!impl_->hwo) return;
    if (p) waveOutPause(impl_->hwo);
    else   waveOutRestart(impl_->hwo);
}

// ===========================================================================
// Everywhere else: no backend yet
//
// Deliberately silent rather than absent — the hosts are otherwise portable,
// and refusing to build over the audio device would be the only thing keeping
// them Windows-only. A real backend (SDL2, PulseAudio, CoreAudio) drops in
// here without touching a single caller.
// ===========================================================================
#else

struct AudioOutput::Impl {};

AudioOutput::AudioOutput() : impl_(new Impl) {}
AudioOutput::~AudioOutput() = default;

bool AudioOutput::isOpen() const                    { return false; }
void AudioOutput::write(const int16_t*, int)        {}
void AudioOutput::pause(bool)                       {}

#endif
