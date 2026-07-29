#include "EmulatorThread.h"
#include "platform/AudioOutput.h"
#include "views/EmulatorScreen.h"
#include "debugger/GpgxBackend.h"
#include "gx/gx.hpp"

// Qt headers must precede gpgx core headers: core/system.h declares a global
// `system_clock` that breaks Qt's unqualified std::chrono lookups otherwise.
#include <chrono>
#include <thread>
#include <QFile>
#include <QFileInfo>
#include <QDebug>

extern "C" {
#include <shared.h>   // defines uint8/uint32 macros first
#include <system.h>
#include <vdp_ctrl.h>
}

namespace chr = std::chrono;

EmulatorThread::EmulatorThread(EmulatorScreen* screen, GpgxBackend* backend, QObject* parent)
    : QThread(parent), screen_(screen), backend_(backend)
{}

EmulatorThread::~EmulatorThread()
{
    requestStop();
    wait();
}

bool EmulatorThread::loadRom(const QString& path)
{
    // diagnostics: verify file exists/readable before passing into gpgx
    QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) {
        const QString msg = QStringLiteral("ROM file not found:\n%1").arg(path);
        qWarning().noquote() << msg;
        emit romLoaded(false, msg);
        return false;
    }
    if (fi.size() <= 0) {
        const QString msg = QStringLiteral("ROM file is empty:\n%1").arg(path);
        qWarning().noquote() << msg;
        emit romLoaded(false, msg);
        return false;
    }

    gx::init();

    // allocate bitmap buffer (720x576 16bpp)
    static std::vector<uint8_t> bitmapBuf(720 * 576 * 2, 0);
    gx::bitmap_data() = bitmapBuf.data();

    // Use a null-terminated copy (string_view::data() is not guaranteed null-terminated)
    const QByteArray pathLocal = QFile::encodeName(path);
    qInfo().noquote() << "Loading ROM:" << path << "size:" << fi.size();

    bool ok = gx::load_rom(std::string(pathLocal.constData(), pathLocal.size()));
    if (!ok) {
        const QString msg = QStringLiteral("gpgx load_rom() failed for:\n%1\n(unsupported format or read error)").arg(path);
        qWarning().noquote() << msg;
        emit romLoaded(false, msg);
        return false;
    }
    emit romLoaded(true, QString());
    return true;
}

void EmulatorThread::requestStop()
{
    stop_.store(true);
    backend_->resume(); // unblock if paused
}

void EmulatorThread::run()
{
    audio_ = new AudioOutput();

    static int16_t audioBuf[2048 * 2]; // stereo, matches gx::SOUND_SAMPLES_SIZE

    const double fps = vdp_pal ? 50.0 : 60.0;
    const auto frameDur = chr::duration_cast<chr::nanoseconds>(chr::duration<double>(1.0 / fps));

    auto nextFrame = chr::steady_clock::now();

    while (!stop_.load()) {
        system_frame_gen(0);

        // push audio (guard: blip_read_samples crashes on count=0 without BLIP_ASSERT)
        if (snd.enabled && snd.blips[0]) {
            int samples = audio_update(audioBuf);
            if (samples > 0)
                audio_->write(audioBuf, samples);
        }

        const t_bitmap& bm = ::bitmap;
        screen_->pushFrame(bm.data, bm.width, bm.height, bm.pitch,
                           bm.viewport.x, bm.viewport.y,
                           bm.viewport.w, bm.viewport.h);

        // frame pacing
        nextFrame += frameDur;
        auto now = chr::steady_clock::now();
        if (nextFrame > now)
            std::this_thread::sleep_until(nextFrame);
        else
            nextFrame = now; // fell behind — reset
    }

    gx::shutdown();
    delete audio_;
    audio_ = nullptr;
}
