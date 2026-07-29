#pragma once
#include <QMainWindow>
#include <QLabel>
#include <QString>
#include <QTimer>
#include <vector>
#include <cstdint>

class EmulatorScreen;
class EmulatorThread;
class GpgxBackend;
class VdpRamView;
class VdpRegView;
class ScrollView;
class SaveStateView;
class VdpSpritesView;
class PlaneExplorerView;
class SoundDebugView;
class MemoryView;
class RamSearchView;
class RamWatchView;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent*) override;

public slots:
    void openRom();
    void openRomFile(const QString& path);

private slots:
    void onRomLoaded(bool ok, const QString& msg);
    void onPaused(uint32_t pc);
    void onResumed();
    void debugPause();
    void debugResume();
    void debugStepInto();
    void debugStepOver();
    void refreshViews();

private:
    void buildMenus();
    void buildDocks();

    GpgxBackend*      backend_   = nullptr;
    EmulatorScreen*   screen_    = nullptr;
    EmulatorThread*   emuThread_ = nullptr;
    QTimer            refreshTimer_;

    VdpRamView*        vdpRamView_    = nullptr;
    VdpRegView*        vdpRegView_    = nullptr;
    ScrollView*        scrollView_    = nullptr;
    SaveStateView*     statesView_    = nullptr;
    VdpSpritesView*    spritesView_   = nullptr;
    PlaneExplorerView* planeView_     = nullptr;
    SoundDebugView*    soundView_     = nullptr;
    MemoryView*        hexView_       = nullptr;
    RamSearchView*     ramSearchView_ = nullptr;
    RamWatchView*      ramWatchView_  = nullptr;

    QLabel* statusLabel_ = nullptr;

    QAction* actPause_    = nullptr;
    QAction* actResume_   = nullptr;
    QAction* actStepInto_ = nullptr;
    QAction* actStepOver_ = nullptr;
};
