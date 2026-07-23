#pragma once
#include <QThread>
#include <QString>
#include <atomic>
#include <cstdint>

class EmulatorScreen;
class GpgxBackend;
class AudioOutput;

class EmulatorThread : public QThread
{
    Q_OBJECT
public:
    explicit EmulatorThread(EmulatorScreen* screen, GpgxBackend* backend, QObject* parent = nullptr);
    ~EmulatorThread() override;

    bool loadRom(const QString& path);
    void requestStop();

signals:
    void frameReady();
    void romLoaded(bool ok, const QString& msg);

protected:
    void run() override;

private:
    EmulatorScreen*  screen_;
    GpgxBackend*     backend_;
    AudioOutput*     audio_  = nullptr;
    std::atomic<bool> stop_  { false };
};
